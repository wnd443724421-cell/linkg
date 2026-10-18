/**
 * @file switch_policy.c
 * @brief LinkG链路切换策略调度实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-24
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
 * 同一把Switch锁内复制三类业务Plan、Observation与Maintenance约束；
 * 后续计划提交使用Generation和previous条件比较，防止旧快照覆盖新计划。
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
 * @brief 判断本轮策略快照是否包含有效的Peer、观测及活动业务计划。
 *
 * 某一业务为NONE不阻止其它业务参与切换策略。
 */
static bool _linkg_switch_policy_input_valid(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       class_index;

    if (input == NULL || input->peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        return false;
    }

    if (!input->observation.valid || input->observation.peer_node_id != input->peer_node_id)
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &input->plan.classes[class_index];

        if ((current->mode == LINKG_SEND_MODE_SINGLE || current->mode == LINKG_SEND_MODE_REDUNDANT) &&
            current->primary_link_id != LINKG_LINK_ID_INVALID)
        {
            return true;
        }
    }

    return false;
}

/****************************** 业务分类 ******************************/

/**
 * @brief 判断两份完整Plan是否逐业务一致，不比较结构体填充字节。
 */
static bool _linkg_switch_policy_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    const linkg_class_send_plan_t *a;
    const linkg_class_send_plan_t *b;
    uint32_t                       class_index;

    if (left == NULL || right == NULL)
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        a = &left->classes[class_index];
        b = &right->classes[class_index];

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
 * @brief 识别本轮三类业务中是否分别存在Wi-Fi和Cellular主链路。
 *
 * 只按Primary所属Link分类，REDUNDANT的Secondary不代表另一类主链路。
 * 这里不评估信号质量，也不补建备用Path；这些职责属于其它模块。
 */
static void _linkg_switch_policy_get_primary_flags(const linkg_switch_policy_input_t *input, bool *wifi_primary, bool *cellular_primary)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       class_index;

    *wifi_primary     = false;
    *cellular_primary = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &input->plan.classes[class_index];
        if (current->mode == LINKG_SEND_MODE_NONE)
        {
            continue;
        }

        if (input->observation.wifi.link_id != LINKG_LINK_ID_INVALID &&
            current->primary_link_id == input->observation.wifi.link_id)
        {
            *wifi_primary = true;
        }
        else if (input->observation.cellular.link_id != LINKG_LINK_ID_INVALID &&
                 current->primary_link_id == input->observation.cellular.link_id)
        {
            *cellular_primary = true;
        }
    }
}

/****************************** 策略调度 ******************************/

/**
 * @brief 执行STA当前一轮链路切换策略。
 *
 * Reconcile负责补充/剔除本地Path，Policy仅处理质量驱动的方向切换。
 * 三类业务可以具有不同Primary，因此同轮可能涉及Wi-Fi和Cellular策略。
 *
 * Wi-Fi方向优先。若Wi-Fi策略修改了完整Plan，本轮不再调用Cellular策略，
 * 避免第二次提交立即取消第一次PLAN_SYNC；未修改时才允许Cellular策略处理。
 *
 * 调用前由Switch Runtime完成Observation刷新，本模块不负责周期调度。
 */
int linkg_switch_policy_process(uint64_t now_us)
{
    linkg_switch_policy_input_t input;
    linkg_switch_policy_input_t refreshed;
    bool                        wifi_primary;
    bool                        cellular_primary;
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

    _linkg_switch_policy_get_primary_flags(&input, &wifi_primary, &cellular_primary);

    if (wifi_primary)
    {
        ret = linkg_switch_policy_wifi_process(&input, now_us);
        if (ret != 0)
        {
            return ret;
        }
    }

    if (!cellular_primary)
    {
        return 0;
    }

    if (wifi_primary)
    {
        // 混合主链路：重新取快照，避免同轮两次提交相互取消PLAN_SYNC。
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

        if (!_linkg_switch_policy_input_valid(&refreshed))
        {
            return 0;
        }

        input = refreshed;
        _linkg_switch_policy_get_primary_flags(&input, &wifi_primary, &cellular_primary);
        if (!cellular_primary)
        {
            return 0;
        }
    }

    return linkg_switch_policy_cellular_process(&input, now_us);
}
