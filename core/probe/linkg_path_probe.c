/**
 * @file linkg_path_probe.c
 * @brief LinkG Path Probe生命周期、快照及同步诊断接口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#include "linkg_path_probe.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_time.h"

#include "path_probe_internal.h"

/****************************** 全局上下文 ******************************/

linkg_path_probe_context_t g_path_probe =
{
    .lock = PTHREAD_MUTEX_INITIALIZER // 永久有效，允许已被Transport复制的旧回调安全返回
};

static pthread_mutex_t g_path_probe_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t  g_path_probe_condition_once = PTHREAD_ONCE_INIT;
static int             g_path_probe_condition_error;

/****************************** 同步辅助 ******************************/

/**
 * @brief 初始化进程生命周期条件变量，与Probe单调时间保持一致。
 */
static void _linkg_path_probe_initialize_condition(void)
{
    pthread_condattr_t attributes;
    int                ret;

    ret = pthread_condattr_init(&attributes);
    if (ret != 0)
    {
        g_path_probe_condition_error = -ret;
        return;
    }

    ret = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (ret == 0)
    {
        ret = pthread_cond_init(&g_path_probe.diagnostic_condition, &attributes);
    }

    pthread_condattr_destroy(&attributes);
    g_path_probe_condition_error = ret == 0 ? 0 : -ret;
}

/**
 * @brief 排空已进入回调及诊断调用，等待期间自动释放Probe锁。
 */
static int _linkg_path_probe_drain_locked(void)
{
    int ret;

    while (g_path_probe.rx_users != 0U || g_path_probe.diagnostic.active)
    {
        ret = pthread_cond_wait(&g_path_probe.diagnostic_condition, &g_path_probe.lock);
        if (ret != 0)
        {
            return -ret;
        }
    }

    return 0;
}

/**
 * @brief 获取Transport回调的运行许可，旧注册代际不得使用新一轮资源。
 */
bool linkg_path_probe_enter_receive(void *user_data, linkg_packet_pool_t **pool, linkg_device_role_t *role, uint8_t *local_node_id)
{
    bool accepted;

    accepted = false;

    pthread_mutex_lock(&g_path_probe.lock);

    if (g_path_probe.initialized && g_path_probe.running && (uintptr_t)user_data == g_path_probe.run_token)
    {
        g_path_probe.rx_users++;
        *pool          = g_path_probe.packet_pool;
        *role          = g_path_probe.role;
        *local_node_id = g_path_probe.local_node_id;
        accepted       = true;
    }

    pthread_mutex_unlock(&g_path_probe.lock);

    return accepted;
}

/**
 * @brief 释放回调运行许可，最后一个回调唤醒停止等待方。
 */
void linkg_path_probe_leave_receive(void)
{
    pthread_mutex_lock(&g_path_probe.lock);

    if (g_path_probe.rx_users != 0U)
    {
        g_path_probe.rx_users--;
    }

    if (g_path_probe.rx_users == 0U)
    {
        pthread_cond_broadcast(&g_path_probe.diagnostic_condition);
    }

    pthread_mutex_unlock(&g_path_probe.lock);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Probe，借用应用层Packet Pool，要求Node及Transport已初始化。
 */
int linkg_path_probe_init(linkg_packet_pool_t *packet_pool)
{
    const linkg_node_info_t *local;
    uint64_t                 now_us;
    int                      ret;

    if (packet_pool == NULL || !packet_pool->initialized)
    {
        return -EINVAL;
    }

    if (packet_pool->headroom < LINKG_TRANSPORT_WIRE_HEADER_SIZE || packet_pool->headroom > packet_pool->slot_size || packet_pool->slot_size - packet_pool->headroom < LINKG_PATH_PROBE_WIRE_SIZE)
    {
        return -ENOSPC;
    }

    pthread_mutex_lock(&g_path_probe_lifecycle_lock);
    ret = pthread_once(&g_path_probe_condition_once, _linkg_path_probe_initialize_condition);
    if (ret != 0 || g_path_probe_condition_error != 0)
    {
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return ret != 0 ? -ret : g_path_probe_condition_error;
    }

    pthread_mutex_lock(&g_path_probe.lock);
    if (g_path_probe.initialized)
    {
        pthread_mutex_unlock(&g_path_probe.lock);
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return -EALREADY;
    }
    pthread_mutex_unlock(&g_path_probe.lock);

    local = linkg_node_get_local();
    if (local == NULL || (local->role != LINKG_DEVICE_ROLE_AP && local->role != LINKG_DEVICE_ROLE_STA))
    {
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return -ENODEV;
    }

    ret = linkg_thread_init(&g_path_probe.thread, LINKG_PATH_PROBE_THREAD_NAME, linkg_path_probe_worker, NULL);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return ret;
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_path_probe.lock);
    linkg_path_probe_runtime_reset_locked();
    g_path_probe.packet_pool   = packet_pool;
    g_path_probe.role          = local->role;
    g_path_probe.local_node_id = local->node_id;
    g_path_probe.initialized   = true;
    g_path_probe.running       = false;

    if (g_path_probe.next_sequence == 0U)
    {
        // 仅降低跨进程旧包碰撞概率，不把此序列号当作认证凭据。
        g_path_probe.next_sequence = (uint32_t)(now_us ^ (now_us >> 32U) ^ (uint64_t)getpid());
    }

    pthread_mutex_unlock(&g_path_probe.lock);
    pthread_mutex_unlock(&g_path_probe_lifecycle_lock);

    return 0;
}

/**
 * @brief 启动周期Worker并注册PATH_PROBE交付回调。
 */
int linkg_path_probe_start(void)
{
    uintptr_t token;
    int       cleanup_ret;
    int       ret;

    pthread_mutex_lock(&g_path_probe_lifecycle_lock);
    pthread_mutex_lock(&g_path_probe.lock);

    if (!g_path_probe.initialized)
    {
        ret = -ENODEV;
        goto out;
    }

    if (g_path_probe.running || g_path_probe.handler_registered || linkg_thread_is_started(&g_path_probe.thread))
    {
        ret = -EALREADY;
        goto out;
    }

    if (g_path_probe.rx_users != 0U || g_path_probe.diagnostic.active)
    {
        ret = -EBUSY;
        goto out;
    }

    if (g_path_probe.run_token == UINTPTR_MAX)
    {
        ret = -EOVERFLOW;
        goto out;
    }

    linkg_path_probe_runtime_reset_locked();
    token                = ++g_path_probe.run_token;
    g_path_probe.running = true;
    pthread_mutex_unlock(&g_path_probe.lock);

    // user_data为Linux目标平台上的不透明整数cookie，从不解引用。
    ret = linkg_transport_register_handler(LINKG_TRANSPORT_TYPE_PATH_PROBE, linkg_path_probe_transport_receive, (void *)token);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_path_probe.lock);
        g_path_probe.running = false;
        linkg_path_probe_cancel_all_locked(ret, linkg_time_monotonic_us());
        goto out;
    }

    pthread_mutex_lock(&g_path_probe.lock);
    g_path_probe.handler_registered = true;
    pthread_mutex_unlock(&g_path_probe.lock);

    ret = linkg_thread_start(&g_path_probe.thread);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_path_probe.lock);
        g_path_probe.running = false;
        linkg_path_probe_cancel_all_locked(ret, linkg_time_monotonic_us());
        pthread_mutex_unlock(&g_path_probe.lock);

        cleanup_ret = linkg_transport_unregister_handler(LINKG_TRANSPORT_TYPE_PATH_PROBE);

        pthread_mutex_lock(&g_path_probe.lock);
        if (cleanup_ret == 0 || cleanup_ret == -ENOENT)
        {
            g_path_probe.handler_registered = false;
        }
        (void)_linkg_path_probe_drain_locked();
        goto out;
    }

    LINKG_LOG_INFO("PATH-PROBE: started, role=%d, normal_ms=1000, sta_cellular_primary_ms=100", (int)g_path_probe.role);
    pthread_mutex_unlock(&g_path_probe_lifecycle_lock);

    return 0;

out:
    pthread_mutex_unlock(&g_path_probe.lock);
    pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
    return ret;
}

/**
 * @brief 停止Worker、注销回调并排空访问者，禁止持有Probe锁等待join。
 *
 * 应从应用控制线程调用，且先于Discovery、Node、Scheduler、Transport和Packet Pool停止。
 */
int linkg_path_probe_stop(void)
{
    bool registered;
    int  first_error;
    int  ret;

    pthread_mutex_lock(&g_path_probe_lifecycle_lock);
    pthread_mutex_lock(&g_path_probe.lock);

    if (!g_path_probe.initialized)
    {
        pthread_mutex_unlock(&g_path_probe.lock);
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return 0;
    }

    if (g_path_probe.worker_tid_valid && pthread_equal(pthread_self(), g_path_probe.worker_tid))
    {
        pthread_mutex_unlock(&g_path_probe.lock);
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return -EDEADLK;
    }

    g_path_probe.running = false;
    registered           = g_path_probe.handler_registered;
    linkg_path_probe_cancel_all_locked(-ESHUTDOWN, linkg_time_monotonic_us());
    pthread_mutex_unlock(&g_path_probe.lock);

    first_error = 0;

    if (registered)
    {
        ret = linkg_transport_unregister_handler(LINKG_TRANSPORT_TYPE_PATH_PROBE);
        if (ret != 0 && ret != -ENOENT)
        {
            first_error = ret;
        }
        else
        {
            pthread_mutex_lock(&g_path_probe.lock);
            g_path_probe.handler_registered = false;
            pthread_mutex_unlock(&g_path_probe.lock);
        }
    }

    ret = linkg_thread_stop(&g_path_probe.thread);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
        return ret;
    }

    pthread_mutex_lock(&g_path_probe.lock);
    ret = _linkg_path_probe_drain_locked();
    if (ret == 0)
    {
        linkg_path_probe_runtime_reset_locked();
    }
    pthread_mutex_unlock(&g_path_probe.lock);
    pthread_mutex_unlock(&g_path_probe_lifecycle_lock);

    return first_error != 0 ? first_error : ret;
}

/**
 * @brief 释放已停止模块资源，永久同步对象保留用于隔离延迟旧回调。
 */
int linkg_path_probe_deinit(void)
{
    int ret;

    pthread_mutex_lock(&g_path_probe_lifecycle_lock);
    pthread_mutex_lock(&g_path_probe.lock);

    ret = 0;
    if (!g_path_probe.initialized)
    {
        goto out;
    }

    if (g_path_probe.running || g_path_probe.handler_registered || g_path_probe.rx_users != 0U || g_path_probe.diagnostic.active || linkg_thread_is_started(&g_path_probe.thread))
    {
        ret = -EBUSY;
        goto out;
    }

    g_path_probe.initialized = false;
    g_path_probe.packet_pool = NULL;
    linkg_path_probe_runtime_reset_locked();
    pthread_mutex_unlock(&g_path_probe.lock);

    linkg_thread_deinit(&g_path_probe.thread);
    pthread_mutex_unlock(&g_path_probe_lifecycle_lock);

    return 0;

out:
    pthread_mutex_unlock(&g_path_probe.lock);
    pthread_mutex_unlock(&g_path_probe_lifecycle_lock);
    return ret;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 复制周期快照，不等待网络；Path存在与Probe最近可达性是不同字段。
 */
int linkg_path_probe_get_peer_snapshot(uint8_t peer_node_id, linkg_path_probe_peer_snapshot_t *snapshot)
{
    linkg_path_probe_peer_runtime_t *peer;
    uint32_t                         index;
    uint32_t                         class_index;
    int                              ret;

    if (snapshot == NULL || peer_node_id < LINKG_RESOURCE_NODE_ID_MIN || peer_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&g_path_probe.lock);

    ret = -ENOENT;
    if (!g_path_probe.initialized || !g_path_probe.running)
    {
        ret = g_path_probe.initialized ? -ENETDOWN : -ENODEV;
        goto out;
    }

    for (index = 0U; index < LINKG_PATH_PROBE_PEER_MAX; index++)
    {
        peer = &g_path_probe.peers[index];
        if (!peer->used || peer->peer_node_id != peer_node_id)
        {
            continue;
        }

        snapshot->peer_node_id     = peer_node_id;
        snapshot->wifi.active      = peer->wifi.active;
        snapshot->wifi.link_id     = peer->wifi.link_id;
        snapshot->cellular.active  = peer->cellular.active;
        snapshot->cellular.link_id = peer->cellular.link_id;

        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            snapshot->wifi.classes[class_index]     = peer->wifi.classes[class_index].snapshot;
            snapshot->cellular.classes[class_index] = peer->cellular.classes[class_index].snapshot;
        }

        ret = 0;
        break;
    }

out:
    pthread_mutex_unlock(&g_path_probe.lock);
    return ret;
}

/****************************** 主动诊断 ******************************/

/**
 * @brief 校验诊断请求，限定发送窗口和最小间隔以避免无界突发。
 */
static int _linkg_path_probe_validate_diagnostic(const linkg_path_probe_diagnostic_request_t *request)
{
    uint64_t window_us;

    if (request == NULL || request->peer_node_id < LINKG_RESOURCE_NODE_ID_MIN || request->peer_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    if ((request->access != LINKG_LINK_ACCESS_WIFI && request->access != LINKG_LINK_ACCESS_CELLULAR) || request->traffic_class < LINKG_TRANSPORT_CLASS_REALTIME || request->traffic_class >= LINKG_TRANSPORT_CLASS_COUNT)
    {
        return -EINVAL;
    }

    if (request->packet_count == 0U || request->packet_count > LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX || request->response_timeout_ms == 0U || request->response_timeout_ms > LINKG_PATH_PROBE_DIAGNOSTIC_TIMEOUT_MAX_MS || request->send_window_ms > LINKG_PATH_PROBE_DIAGNOSTIC_WINDOW_MAX_MS)
    {
        return -EINVAL;
    }

    window_us = (uint64_t)request->send_window_ms * 1000U;
    if (request->packet_count > 1U && window_us / request->packet_count < LINKG_PATH_PROBE_DIAGNOSTIC_MIN_GAP_US)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief STA同步执行固定节拍多包诊断，Worker异步发送，调用线程仅等待完成。
 *
 * 返回0表示所有样本已经完成，100%响应超时仍是有效诊断结果。
 * 负值表示参数、角色、忙、生命周期或路径变更错误，result可能只有部分统计。
 * 调用期间屏蔽pthread取消，避免取消点遗留活动任务及停止等待者。
 */
int linkg_path_probe_diagnose(const linkg_path_probe_diagnostic_request_t *request, linkg_path_probe_diagnostic_result_t *result)
{
    linkg_path_probe_diagnostic_request_t  request_copy;
    linkg_path_probe_diagnostic_runtime_t *diagnostic;
    linkg_path_probe_peer_runtime_t       *peer;
    linkg_path_probe_path_runtime_t       *path;
    struct timespec                        deadline;
    uint64_t                               now_us;
    uint64_t                               deadline_us;
    uint32_t                               index;
    int                                    old_cancel_state;
    int                                    ret;

    if (result == NULL)
    {
        return -EINVAL;
    }

    memset(result, 0, sizeof(*result));
    ret = _linkg_path_probe_validate_diagnostic(request);
    if (ret != 0)
    {
        return ret;
    }

    request_copy = *request;
    ret          = pthread_setcancelstate(PTHREAD_CANCEL_DISABLE, &old_cancel_state);
    if (ret != 0)
    {
        return -ret;
    }

    pthread_mutex_lock(&g_path_probe.lock);

    if (!g_path_probe.initialized || !g_path_probe.running)
    {
        ret = g_path_probe.initialized ? -ESHUTDOWN : -ENODEV;
        goto out;
    }

    if (g_path_probe.role != LINKG_DEVICE_ROLE_STA)
    {
        ret = -EPERM;
        goto out;
    }

    if (g_path_probe.worker_tid_valid && pthread_equal(pthread_self(), g_path_probe.worker_tid))
    {
        ret = -EDEADLK;
        goto out;
    }

    if (g_path_probe.diagnostic.active)
    {
        ret = -EBUSY;
        goto out;
    }

    peer = NULL;
    for (index = 0U; index < LINKG_PATH_PROBE_PEER_MAX; index++)
    {
        if (g_path_probe.peers[index].used && g_path_probe.peers[index].peer_node_id == request_copy.peer_node_id)
        {
            peer = &g_path_probe.peers[index];
            break;
        }
    }

    if (peer == NULL)
    {
        ret = -ENOENT;
        goto out;
    }

    path = request_copy.access == LINKG_LINK_ACCESS_WIFI ? &peer->wifi : &peer->cellular;
    if (!path->active)
    {
        ret = -ENETDOWN;
        goto out;
    }

    now_us     = linkg_time_monotonic_us();
    diagnostic = &g_path_probe.diagnostic;
    memset(diagnostic, 0, sizeof(*diagnostic));

    diagnostic->active            = true;
    diagnostic->request           = request_copy;
    diagnostic->id                = ++g_path_probe.next_diagnostic_id;
    diagnostic->path_generation   = path->generation;
    diagnostic->link_id           = path->link_id;
    diagnostic->started_us        = now_us;
    diagnostic->send_window_us    = request_copy.packet_count == 1U ? 0U : (uint64_t)request_copy.send_window_ms * 1000U;
    diagnostic->next_send_us      = now_us;
    diagnostic->final_deadline_us = now_us + diagnostic->send_window_us + (uint64_t)request_copy.response_timeout_ms * 1000U + LINKG_PATH_PROBE_DIAGNOSTIC_DISPATCH_GRACE_US;
    deadline_us                   = diagnostic->final_deadline_us;
    deadline.tv_sec               = (time_t)(deadline_us / 1000000U);
    deadline.tv_nsec              = (long)((deadline_us % 1000000U) * 1000U);

    pthread_mutex_unlock(&g_path_probe.lock);

    // 活动诊断使stop等待调用者，故此处thread和eventfd不会被deinit。
    ret = linkg_thread_wakeup(&g_path_probe.thread);

    pthread_mutex_lock(&g_path_probe.lock);
    if (ret != 0)
    {
        linkg_path_probe_cancel_diagnostic_locked(ret, linkg_time_monotonic_us());
    }

    while (!diagnostic->completed)
    {
        ret = pthread_cond_timedwait(&g_path_probe.diagnostic_condition, &g_path_probe.lock, &deadline);
        if (ret == ETIMEDOUT)
        {
            linkg_path_probe_expire_locked(linkg_time_monotonic_us());
        }
        else if (ret != 0)
        {
            linkg_path_probe_cancel_diagnostic_locked(-ret, linkg_time_monotonic_us());
        }
    }

    *result = diagnostic->result;
    ret                = diagnostic->status;
    diagnostic->active = false;
    pthread_cond_broadcast(&g_path_probe.diagnostic_condition);

out:
    pthread_mutex_unlock(&g_path_probe.lock);
    (void)pthread_setcancelstate(old_cancel_state, NULL);
    return ret;
}
