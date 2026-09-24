/**
 * @file switch_runtime.c
 * @brief LinkG链路切换Worker调度实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#include "switch_internal.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "switch_event.h"
#include "switch_observation.h"
#include "switch_plan.h"
#include "switch_report.h"
#include "switch_maintenance.h"
#include "switch_policy.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断周期任务错误是否属于正常运行状态变化。
 */
static bool _linkg_switch_runtime_expected_error(int error)
{
    return error == -ENOENT ||
           error == -ENODEV ||
           error == -ENETDOWN ||
           error == -EAGAIN ||
           error == -ESTALE ||
           error == -ESHUTDOWN;
}


/**
 * @brief 记录Switch周期任务的非预期错误。
 */
static void _linkg_switch_runtime_log_error(const char *task, int error)
{
    if (error == 0 || _linkg_switch_runtime_expected_error(error))
    {
        return;
    }

    LINKG_LOG_WARN("SWITCH: %s failed, error=%d", task, error);
}

/**
 * @brief 将单个Switch内部事件分发到对应业务模块。
 */
static int _linkg_switch_runtime_process_event(const linkg_switch_event_t *event, uint64_t now_us)
{
    if (event == NULL)
    {
        return -EINVAL;
    }

    switch (event->type)
    {
        case LINKG_SWITCH_EVENT_PLAN_SYNC_RX:
        case LINKG_SWITCH_EVENT_PLAN_ACK_RX:
        {
            return linkg_switch_plan_process_event(event, now_us);
        }

        case LINKG_SWITCH_EVENT_MAINTENANCE_RX:
        case LINKG_SWITCH_EVENT_MAINTENANCE_ACK_RX:
        {
            return linkg_switch_maintenance_process_event(event, now_us);
        }

        default:
        {
            return -EPROTONOSUPPORT;
        }
    }
}

/**
 * @brief 取出并处理当前待处理的Switch内部控制事件。
 */
static void _linkg_switch_runtime_process_events(uint64_t now_us)
{
    linkg_switch_event_t event;
    uint32_t             budget;
    bool                 available;
    int                  ret;

    for (budget = 0U; budget < LINKG_SWITCH_EVENT_QUEUE_CAPACITY; budget++)
    {
        pthread_mutex_lock(&g_switch.lock);
        available = linkg_switch_event_pop_locked(&event);
        pthread_mutex_unlock(&g_switch.lock);

        if (!available)
        {
            break;
        }

        ret = _linkg_switch_runtime_process_event(&event, now_us);
        _linkg_switch_runtime_log_error("control event", ret);
    }
}

/**
 * @brief 执行STA当前到期的一轮Switch观测刷新。
 */
static void _linkg_switch_runtime_process_observation(uint64_t now_us)
{
    uint8_t  peer_node_ids[LINKG_SWITCH_PEER_MAX];
    uint32_t peer_generations[LINKG_SWITCH_PEER_MAX];
    uint32_t peer_count;
    uint32_t index;
    int      ret;

    peer_count = 0U;

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized ||
        !g_switch.running ||
        g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return;
    }

    if (g_switch.next_observation_us != 0U &&
        now_us < g_switch.next_observation_us)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return;
    }

    // 周期任务不补跑错过的历史周期，只从本轮重新安排下一次。
    g_switch.next_observation_us = now_us + LINKG_SWITCH_OBSERVATION_INTERVAL_US;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        peer_node_ids[peer_count]    = g_switch.peers[index].peer_node_id;
        peer_generations[peer_count] = g_switch.peers[index].generation;
        peer_count++;
    }

    pthread_mutex_unlock(&g_switch.lock);

    for (index = 0U; index < peer_count; index++)
    {
        ret = linkg_switch_observation_refresh(peer_node_ids[index], peer_generations[index], now_us);
        _linkg_switch_runtime_log_error("observation refresh", ret);
    }
}

/**
 * @brief 执行STA当前到期的链路切换策略检查。
 */
static void _linkg_switch_runtime_process_policy(uint64_t now_us)
{
    uint64_t deadline_us;
    bool     due;
    int      ret;

    pthread_mutex_lock(&g_switch.lock);

    due = false;

    if (g_switch.initialized &&
        g_switch.running &&
        g_switch.role == LINKG_DEVICE_ROLE_STA)
    {
        deadline_us = linkg_switch_policy_next_deadline_locked();
        due         = deadline_us <= now_us;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (!due)
    {
        return;
    }

    ret = linkg_switch_policy_process(now_us);
    _linkg_switch_runtime_log_error("policy check", ret);
}

/**
 * @brief 执行AP当前到期的一轮Wi-Fi质量上报。
 */
static void _linkg_switch_runtime_process_report(uint64_t now_us)
{
    uint64_t deadline_us;
    bool     due;
    int      ret;

    pthread_mutex_lock(&g_switch.lock);

    due = false;

    if (g_switch.initialized &&
        g_switch.running &&
        g_switch.role == LINKG_DEVICE_ROLE_AP)
    {
        deadline_us = linkg_switch_report_next_deadline_locked();
        due         = deadline_us <= now_us;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (!due)
    {
        return;
    }

    ret = linkg_switch_report_process(now_us);
    _linkg_switch_runtime_log_error("quality report", ret);
}

/**
 * @brief 执行STA当前到期的发送计划同步重试或超时处理。
 */
static void _linkg_switch_runtime_process_plan(uint64_t now_us)
{
    uint64_t deadline_us;
    bool     due;
    int      ret;

    pthread_mutex_lock(&g_switch.lock);

    due = false;

    if (g_switch.initialized &&
        g_switch.running &&
        g_switch.role == LINKG_DEVICE_ROLE_STA)
    {
        deadline_us = linkg_switch_plan_next_deadline_locked();
        due         = deadline_us <= now_us;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (!due)
    {
        return;
    }

    ret = linkg_switch_plan_process(now_us);
    _linkg_switch_runtime_log_error("plan retry", ret);
}

/**
 * @brief 执行当前到期的Maintenance重试和保护超时处理。
 */
static void _linkg_switch_runtime_process_maintenance(uint64_t now_us)
{
    uint64_t deadline_us;
    bool     due;
    int      ret;

    pthread_mutex_lock(&g_switch.lock);

    due = false;

    if (g_switch.initialized &&
        g_switch.running)
    {
        deadline_us = linkg_switch_maintenance_next_deadline_locked();
        due         = deadline_us <= now_us;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (!due)
    {
        return;
    }

    ret = linkg_switch_maintenance_process(now_us);
    _linkg_switch_runtime_log_error("maintenance", ret);
}

/**
 * @brief 执行当前所有已经到期的Switch周期任务。
 */
static void _linkg_switch_runtime_process_due(uint64_t now_us)
{
    _linkg_switch_runtime_process_observation(now_us);
    _linkg_switch_runtime_process_policy(now_us);
    _linkg_switch_runtime_process_report(now_us);
    _linkg_switch_runtime_process_plan(now_us);
    _linkg_switch_runtime_process_maintenance(now_us);
}

/**
 * @brief 将最近Switch截止时间转换为poll毫秒超时。
 *
 * 返回-1表示当前没有定时任务，可无限等待eventfd。
 */
static int _linkg_switch_runtime_poll_timeout_locked(uint64_t now_us)
{
    uint64_t deadline_us;
    uint64_t candidate_us;
    uint64_t delta_us;
    uint64_t timeout_ms;

    if (!g_switch.initialized || !g_switch.running)
    {
        return 0;
    }

    if (g_switch.event_queue.count != 0U)
    {
        return 0;
    }

    deadline_us = UINT64_MAX;

    if (g_switch.role == LINKG_DEVICE_ROLE_STA)
    {
        if (g_switch.next_observation_us != 0U &&
            g_switch.next_observation_us < deadline_us)
        {
            deadline_us = g_switch.next_observation_us;
        }

        candidate_us = linkg_switch_policy_next_deadline_locked();
        if (candidate_us < deadline_us)
        {
            deadline_us = candidate_us;
        }

        candidate_us = linkg_switch_plan_next_deadline_locked();
        if (candidate_us < deadline_us)
        {
            deadline_us = candidate_us;
        }
    }
    else if (g_switch.role == LINKG_DEVICE_ROLE_AP)
    {
        candidate_us = linkg_switch_report_next_deadline_locked();
        if (candidate_us < deadline_us)
        {
            deadline_us = candidate_us;
        }
    }

    candidate_us = linkg_switch_maintenance_next_deadline_locked();
    if (candidate_us < deadline_us)
    {
        deadline_us = candidate_us;
    }

    if (deadline_us == UINT64_MAX)
    {
        return -1;
    }

    if (deadline_us <= now_us)
    {
        return 0;
    }

    delta_us = deadline_us - now_us;

    timeout_ms = delta_us / 1000ULL;
    if (delta_us % 1000ULL != 0U)
    {
        timeout_ms++;
    }

    return timeout_ms > INT_MAX ? INT_MAX : (int)timeout_ms;
}

/****************************** 工作线程 ******************************/

/**
 * @brief 调度Switch内部事件、周期观测、质量上报、计划同步及Maintenance事务。
 */
void linkg_switch_worker(linkg_thread_t *thread, void *user_data)
{
    struct pollfd descriptor;
    uint64_t      now_us;
    bool          running;
    int           timeout_ms;
    int           failure;
    int           ret;

    (void)user_data;

    pthread_mutex_lock(&g_switch.lock);

    g_switch.worker_tid       = pthread_self();
    g_switch.worker_tid_valid = true;

    pthread_mutex_unlock(&g_switch.lock);

    descriptor.fd     = linkg_thread_get_wakeup_fd(thread);
    descriptor.events = POLLIN;
    descriptor.revents = 0;

    failure = 0;

    if (descriptor.fd < 0)
    {
        failure = -ENODEV;
    }

    while (failure == 0 && linkg_thread_is_running(thread))
    {
        pthread_mutex_lock(&g_switch.lock);
        running = g_switch.initialized && g_switch.running;
        pthread_mutex_unlock(&g_switch.lock);

        if (!running)
        {
            break;
        }

        now_us = linkg_time_monotonic_us();

        // 控制事务优先于周期质量任务处理。
        _linkg_switch_runtime_process_events(now_us);

        now_us = linkg_time_monotonic_us();
        _linkg_switch_runtime_process_due(now_us);

        pthread_mutex_lock(&g_switch.lock);

        if (!g_switch.initialized || !g_switch.running)
        {
            pthread_mutex_unlock(&g_switch.lock);
            break;
        }

        timeout_ms = _linkg_switch_runtime_poll_timeout_locked(linkg_time_monotonic_us());

        pthread_mutex_unlock(&g_switch.lock);

        descriptor.revents = 0;

        ret = poll(&descriptor, 1U, timeout_ms);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            failure = -errno;
            break;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            failure = -EIO;
            break;
        }

        if ((descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                failure = ret;
            }
        }
    }

    pthread_mutex_lock(&g_switch.lock);

    g_switch.running          = false;
    g_switch.worker_tid_valid = false;

    pthread_mutex_unlock(&g_switch.lock);

    if (failure != 0)
    {
        LINKG_LOG_ERROR("SWITCH: worker failed, error=%d", failure);
    }
}
