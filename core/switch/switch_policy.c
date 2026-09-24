/**
 * @file switch_policy.c
 * @brief LinkG链路切换策略实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-24
 */

#include "switch_policy.h"

#include <errno.h>
#include <stdint.h>

#include "switch_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_POLICY_INTERVAL_US 250000ULL // STA链路切换策略检查周期

/****************************** 策略检查 ******************************/

/**
 * @brief 执行STA当前到期的链路切换策略检查。
 *
 * 正常周期到期时更新下一次检查时间。
 * 主动触发只执行额外检查，不改变原有周期。
 */
int linkg_switch_policy_process(uint64_t now_us)
{
    bool due;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

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
        return 0;
    }

    due = g_switch.next_policy_us == 0U || now_us >= g_switch.next_policy_us;

    if (!due && !g_switch.policy_check_pending)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    g_switch.policy_check_pending = false;

    if (due)
    {
        g_switch.next_policy_us = now_us + LINKG_SWITCH_POLICY_INTERVAL_US;
    }

    pthread_mutex_unlock(&g_switch.lock);

    // 后续检查当前发送计划是否为WiFi主链路、Cellular备用链路。

    // 后续检查WiFi是否已经发生明确故障，必要时直接切换。

    // 后续执行WiFi链路质量预测。

    // 后续请求健康检查，并根据确认结果决定是否切换。

    return 0;
}

/**
 * @brief 获取下一次STA策略检查截止时间，调用方持有Switch锁。
 */
uint64_t linkg_switch_policy_next_deadline_locked(void)
{
    if (!g_switch.initialized ||
        !g_switch.running ||
        g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        return UINT64_MAX;
    }

    if (g_switch.policy_check_pending)
    {
        return 0U;
    }

    return g_switch.next_policy_us;
}
