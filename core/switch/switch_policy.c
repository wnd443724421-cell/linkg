/**
 * @file switch_policy.c
 * @brief LinkG链路切换策略实现
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
#include "switch_policy_internal.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取当前STA唯一直接Peer的本轮Policy输入快照。
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
 * @brief 判断本轮Policy输入是否具备基础处理条件。
 */
static bool _linkg_switch_policy_input_valid(const linkg_switch_policy_input_t *input)
{
    if (input == NULL)
    {
        return false;
    }

    if (input->peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        return false;
    }

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id)
    {
        return false;
    }

    if (input->plan.mode != LINKG_SEND_MODE_SINGLE &&
        input->plan.mode != LINKG_SEND_MODE_REDUNDANT)
    {
        return false;
    }

    if (input->plan.primary_link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    return true;
}

/**
 * @brief 根据当前Plan和Observation确定主链路接入类型。
 */
static linkg_link_access_t _linkg_switch_policy_get_primary_access(const linkg_switch_policy_input_t *input)
{
    if (input == NULL || input->plan.primary_link_id == LINKG_LINK_ID_INVALID)
    {
        return LINKG_LINK_ACCESS_NONE;
    }

    if (input->plan.primary_link_id == input->observation.wifi.link_id)
    {
        return LINKG_LINK_ACCESS_WIFI;
    }

    if (input->plan.primary_link_id == input->observation.cellular.link_id)
    {
        return LINKG_LINK_ACCESS_CELLULAR;
    }

    return LINKG_LINK_ACCESS_NONE;
}

/****************************** 策略处理 ******************************/

/**
 * @brief 执行STA当前一轮链路切换策略处理。
 *
 * 调用前由Switch Runtime保证已经完成本轮Observation刷新。
 * Policy不负责周期调度，也不负责处理Worker主动唤醒请求。
 */
int linkg_switch_policy_process(uint64_t now_us)
{
    linkg_switch_policy_input_t input;
    linkg_link_access_t         primary_access;
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

    primary_access = _linkg_switch_policy_get_primary_access(&input);

    if (primary_access == LINKG_LINK_ACCESS_WIFI)
    {
        return linkg_switch_policy_wifi_process(&input, now_us);
    }

    if (primary_access == LINKG_LINK_ACCESS_CELLULAR)
    {
        return linkg_switch_policy_cellular_process(&input, now_us);
    }

    return 0;
}
