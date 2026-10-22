/**
 * @file switch_policy.c
 * @brief LinkG链路切换策略调度实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-22
 */

#include "switch_policy.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"

#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_policy_internal.h"

/****************************** Policy输入快照 ******************************/

/**
 * @brief 获取当前STA唯一直接Peer的完整策略输入快照。
 *
 * 在同一把Switch锁内复制Plan、Observation和Maintenance约束。
 * Operation后续仍须以Peer Generation和当前Plan复核事务身份。
 */
static int _linkg_switch_policy_get_input(linkg_switch_policy_input_t *input)
{
    linkg_switch_peer_runtime_t *peer;
    uint32_t                     index;

    if (input == NULL)
    {
        return -EINVAL;
    }

    memset(input, 0, sizeof(*input));

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (!g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    peer = NULL;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        if (peer != NULL)
        {
            pthread_mutex_unlock(&g_switch.lock);
            return -E2BIG;
        }

        peer = &g_switch.peers[index];
    }

    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    input->peer_node_id     = peer->peer_node_id;
    input->peer_generation  = peer->generation;
    input->plan             = peer->plan;
    input->observation      = peer->role.sta.observation;
    input->wifi_blocked     = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_WIFI);
    input->cellular_blocked = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_CELLULAR);

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 判断当前快照是否具备可供策略处理的活动发送计划。
 */
static bool _linkg_switch_policy_input_valid(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       index;

    if (input == NULL || input->peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        return false;
    }

    if (!input->observation.valid || input->observation.peer_node_id != input->peer_node_id)
    {
        return false;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current = &input->plan.classes[index];

        if ((current->mode == LINKG_SEND_MODE_SINGLE || current->mode == LINKG_SEND_MODE_REDUNDANT) &&
            current->primary_link_id != LINKG_LINK_ID_INVALID)
        {
            return true;
        }
    }

    return false;
}

/****************************** 发送计划分类 ******************************/

/**
 * @brief 比较完整三业务Plan，避免旧策略快照覆盖新事务。
 */
static bool _linkg_switch_policy_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    const linkg_class_send_plan_t *a;
    const linkg_class_send_plan_t *b;
    uint32_t                       index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        a = &left->classes[index];
        b = &right->classes[index];

        if (a->mode != b->mode ||
            a->primary_link_id != b->primary_link_id ||
            a->secondary_link_id != b->secondary_link_id)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 判断当前计划是否仍有Cellular作为主链路的业务。
 */
static bool _linkg_switch_policy_has_cellular_primary(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       index;
    uint32_t                       cellular_link_id;

    cellular_link_id = input->observation.cellular.link_id;
    if (cellular_link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current = &input->plan.classes[index];
        if (current->mode != LINKG_SEND_MODE_NONE &&
            current->primary_link_id == cellular_link_id)
        {
            return true;
        }
    }

    return false;
}

/****************************** 策略调度 ******************************/

/**
 * @brief 调度STA本轮Wi-Fi异常检测及Cellular恢复候选检测。
 *
 * Wi-Fi优先检查明确硬故障，不依赖Observation的当前Wi-Fi Link ID
 * 能否匹配旧Plan；Quality检测及事务提交由各子模块处理。
 *
 * Wi-Fi子模块可能同步调用Operation提交完整Plan，因此随后必须重新
 * 取得Peer及Plan快照，防止同轮Cellular恢复事件覆盖新PlanSync。
 * 事务计时及PlanSync结果消费由Switch Runtime独立调度。
 */
int linkg_switch_policy_process(uint64_t now_us)
{
    linkg_switch_policy_input_t input;
    linkg_switch_policy_input_t refreshed;
    int                         ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_policy_get_input(&input);
    if (ret != 0)
    {
        return ret;
    }

    if (!_linkg_switch_policy_input_valid(&input))
    {
        return 0;
    }

    // 无条件检查Wi-Fi硬故障；子模块自行判断本轮是否存在可迁移业务。
    ret = linkg_switch_policy_wifi_process(&input, now_us);
    if (ret != 0)
    {
        return ret;
    }

    // Wi-Fi事件可能已由Operation修改Plan，需要重新读取整个Peer快照。
    ret = _linkg_switch_policy_get_input(&refreshed);
    if (ret != 0)
    {
        return ret;
    }

    if (refreshed.peer_node_id != input.peer_node_id ||
        refreshed.peer_generation != input.peer_generation ||
        !_linkg_switch_policy_plan_equal(&refreshed.plan, &input.plan))
    {
        return 0;
    }

    if (!_linkg_switch_policy_input_valid(&refreshed) ||
        !_linkg_switch_policy_has_cellular_primary(&refreshed))
    {
        return 0;
    }

    return linkg_switch_policy_cellular_process(&refreshed, now_us);
}
