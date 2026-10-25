
/**
 * @file switch_policy.c
 * @brief LinkG链路切换策略统一调度
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

/****************************** 策略输入快照 ******************************/

/**
 * @brief 获取当前STA唯一直接Peer的完整策略输入快照。
 *
 * 在Switch锁内统一复制Plan、Observation及Maintenance状态。
 * 后续Operation仍需校验Peer Generation和当前Plan。
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

    // Switch必须完成初始化并处于运行状态。
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

    // 切换策略仅在STA侧执行。
    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    // STA只允许存在一个直接Peer。
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

    // 在同一把锁内获取完整策略快照。
    input->peer_node_id    = peer->peer_node_id;
    input->peer_generation = peer->generation;
    input->plan            = peer->plan;
    input->observation     = peer->role.sta.observation;

    // 保存当前两条接入链路的维护限制。
    input->wifi_blocked     = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_WIFI);
    input->cellular_blocked = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_CELLULAR);

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 判断策略快照是否有效且至少存在一类活动发送计划。
 */
static bool _linkg_switch_policy_input_valid(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       index;

    // Peer必须具有有效的Generation。
    if (input == NULL || input->peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        return false;
    }

    // Observation必须有效且属于当前Peer。
    if (!input->observation.valid || input->observation.peer_node_id != input->peer_node_id)
    {
        return false;
    }

    // 至少存在一类具有有效Primary Link的发送业务。
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current = &input->plan.classes[index];

        if (current->mode != LINKG_SEND_MODE_SINGLE &&
            current->mode != LINKG_SEND_MODE_REDUNDANT)
        {
            continue;
        }

        if (current->primary_link_id != LINKG_LINK_ID_INVALID)
        {
            return true;
        }
    }

    return false;
}

/****************************** 发送计划检查 ******************************/

/**
 * @brief 比较两份完整三业务Plan是否一致。
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
 * @brief 判断当前Plan是否存在以Cellular为主链路的业务。
 */
static bool _linkg_switch_policy_has_cellular_primary(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       cellular_link_id;
    uint32_t                       index;

    cellular_link_id = input->observation.cellular.link_id;

    if (cellular_link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    // 至少一类业务以当前Cellular Link为Primary。
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

/****************************** 策略统一调度 ******************************/

/**
 * @brief 执行STA本轮Wi-Fi异常检测与Cellular恢复候选检测。
 *
 * Wi-Fi优先处理硬故障、无线丢包和队列溢出。
 * Wi-Fi策略可能同步提交新的完整Plan，因此Cellular策略执行前
 * 必须重新获取Peer快照，并确认Peer身份和完整Plan未发生变化。
 *
 * 本模块只负责策略调度，不管理Operation、PlanSync和Verify生命周期。
 */
int linkg_switch_policy_process(uint64_t now_us)
{
    linkg_switch_policy_input_t input;
    linkg_switch_policy_input_t refreshed;
    int                         ret;

    /****************************** 初始快照 ******************************/

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_policy_get_input(&input);
    if (ret != 0)
    {
        return ret;
    }

    // 没有有效业务计划时，不执行本轮策略判断。
    if (!_linkg_switch_policy_input_valid(&input))
    {
        return 0;
    }

    /****************************** Wi-Fi策略 ******************************/

    // 优先检测Wi-Fi硬故障，再判断LOSS和LOAD。
    ret = linkg_switch_policy_wifi_process(&input, now_us);
    if (ret != 0)
    {
        return ret;
    }

    /****************************** 刷新策略快照 ******************************/

    // Wi-Fi策略可能已提交新Plan，必须重新获取当前状态。
    ret = _linkg_switch_policy_get_input(&refreshed);
    if (ret != 0)
    {
        return ret;
    }

    // Peer已变化，本轮不继续执行Cellular恢复判决。
    if (refreshed.peer_node_id != input.peer_node_id ||
        refreshed.peer_generation != input.peer_generation)
    {
        return 0;
    }

    // 完整Plan已变化，避免使用旧策略结果再次触发切换。
    if (!_linkg_switch_policy_plan_equal(&refreshed.plan, &input.plan))
    {
        return 0;
    }

    /****************************** Cellular恢复策略 ******************************/

    // 当前快照必须有效。
    if (!_linkg_switch_policy_input_valid(&refreshed))
    {
        return 0;
    }

    // 只有存在Cellular主发业务时，才需要进行Wi-Fi恢复判决。
    if (!_linkg_switch_policy_has_cellular_primary(&refreshed))
    {
        return 0;
    }

    return linkg_switch_policy_cellular_process(&refreshed, now_us);
}
