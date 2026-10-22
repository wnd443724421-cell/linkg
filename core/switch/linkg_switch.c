/**
 * @file linkg_switch.c
 * @brief LinkG链路切换生命周期及公共接口实现
 * @author Dawn
 * @version 2.1.0
 * @date 2026-09-19
 */

#include "linkg_switch.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_packet_pool.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"
#include "linkg_transport.h"
#include "linkg_transport_types.h"

#include "switch_event.h"
#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_plan.h"
#include "switch_rx.h"
#include "switch_wire.h"

/****************************** 全局上下文 ******************************/

linkg_switch_context_t g_switch =
{
    .lock         = PTHREAD_MUTEX_INITIALIZER, // 永久有效，允许延迟Transport回调安全退出
    .rx_condition = PTHREAD_COND_INITIALIZER   // 永久有效，用于等待已进入Transport回调排空
};

static pthread_mutex_t g_switch_lifecycle_lock = PTHREAD_MUTEX_INITIALIZER; // 串行化Switch生命周期操作

/****************************** 接入约束 ******************************/

/**
 * @brief 读取本机及指定Peer的Maintenance约束，供发送目标获取时复核。
 */
bool linkg_switch_access_blocked(uint8_t peer_node_id, linkg_link_access_t access)
{
    linkg_switch_peer_runtime_t *peer;
    bool                         blocked;

    pthread_mutex_lock(&g_switch.lock);
    peer    = linkg_switch_find_peer_locked(peer_node_id);
    blocked = g_switch.initialized && linkg_switch_maintenance_access_blocked_locked(peer == NULL ? NULL : &peer->maintenance, access);
    pthread_mutex_unlock(&g_switch.lock);
    return blocked;
}

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验Switch使用的外部Packet Pool容量。
 */
static int _linkg_switch_validate_packet_pool(const linkg_packet_pool_t *packet_pool)
{
    if (packet_pool == NULL || !packet_pool->initialized)
    {
        return -EINVAL;
    }

    if (packet_pool->headroom < LINKG_TRANSPORT_WIRE_HEADER_SIZE)
    {
        return -ENOSPC;
    }

    if (packet_pool->headroom > packet_pool->slot_size)
    {
        return -ENOSPC;
    }

    if (packet_pool->slot_size - packet_pool->headroom < LINKG_SWITCH_WIRE_MAX_SIZE)
    {
        return -ENOSPC;
    }

    return 0;
}

/**
 * @brief 清空Switch全部业务运行状态，不保留旧Peer与发送计划。
 *
 * 仅在Worker尚未启动或已经停止，并且Transport接收回调已经排空时调用。
 * Peer、三类Plan、同步事务、Maintenance、观测和事件队列全部清空。
 *
 * 不重置永久Mutex、Condition、Thread对象及Transport运行代际。
 * 保留进程内Peer代际和主动消息编号，避免迟到事件复用旧标识。
 * 调用方必须持有g_switch.lock。
 */
static void _linkg_switch_reset_runtime_locked(void)
{
    memset(g_switch.peers, 0, sizeof(g_switch.peers));
    memset(&g_switch.local_maintenance, 0, sizeof(g_switch.local_maintenance));

    linkg_switch_event_reset_locked();

    g_switch.next_observation_us = 0U;
    g_switch.next_reconcile_us   = 0U;
    g_switch.next_report_us      = 0U;
    g_switch.reconcile_pending   = false;
}

/**
 * @brief 等待已经进入Switch Transport回调的调用者全部退出。
 *
 * 调用方持有Switch锁，pthread_cond_wait等待期间自动释放并重新获取该锁。
 */
static int _linkg_switch_drain_receive_locked(void)
{
    int ret;

    while (g_switch.rx_users != 0U)
    {
        ret = pthread_cond_wait(&g_switch.rx_condition, &g_switch.lock);
        if (ret != 0)
        {
            return -ret;
        }
    }

    return 0;
}

/****************************** Transport接收 ******************************/

/**
 * @brief 获取当前Switch Transport回调运行许可。
 *
 * 旧注册代际不得访问新一轮Switch运行状态。
 */
bool linkg_switch_enter_receive(void *user_data, linkg_device_role_t *role, uint8_t *local_node_id)
{
    bool accepted;

    if (role == NULL || local_node_id == NULL)
    {
        return false;
    }

    accepted = false;

    pthread_mutex_lock(&g_switch.lock);

    if (g_switch.initialized &&
        g_switch.running &&
        (uintptr_t)user_data == g_switch.run_token)
    {
        g_switch.rx_users++;

        *role          = g_switch.role;
        *local_node_id = g_switch.local_node_id;
        accepted = true;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return accepted;
}

/**
 * @brief 释放Switch Transport回调运行许可。
 */
void linkg_switch_leave_receive(void)
{
    pthread_mutex_lock(&g_switch.lock);

    if (g_switch.rx_users != 0U)
    {
        g_switch.rx_users--;
    }

    if (g_switch.rx_users == 0U)
    {
        pthread_cond_broadcast(&g_switch.rx_condition);
    }

    pthread_mutex_unlock(&g_switch.lock);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化链路切换模块并借用应用层Packet Pool。
 */
int linkg_switch_init(linkg_packet_pool_t *packet_pool)
{
    const linkg_node_info_t *local;
    int                      ret;

    ret = _linkg_switch_validate_packet_pool(packet_pool);
    if (ret != 0)
    {
        return ret;
    }

    local = linkg_node_get_local();
    if (local == NULL)
    {
        return -ENODEV;
    }

    if (local->role != LINKG_DEVICE_ROLE_STA &&
        local->role != LINKG_DEVICE_ROLE_AP)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_switch_lifecycle_lock);
    pthread_mutex_lock(&g_switch.lock);

    if (g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        pthread_mutex_unlock(&g_switch_lifecycle_lock);
        return -EALREADY;
    }

    if (g_switch.rx_users != 0U)
    {
        pthread_mutex_unlock(&g_switch.lock);
        pthread_mutex_unlock(&g_switch_lifecycle_lock);
        return -EBUSY;
    }

    pthread_mutex_unlock(&g_switch.lock);

    ret = linkg_thread_init(&g_switch.thread,
                            LINKG_SWITCH_THREAD_NAME,
                            linkg_switch_worker,
                            NULL);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_switch_lifecycle_lock);
        return ret;
    }

    pthread_mutex_lock(&g_switch.lock);

    _linkg_switch_reset_runtime_locked();

    g_switch.packet_pool   = packet_pool;
    g_switch.role          = local->role;
    g_switch.local_node_id = local->node_id;
    g_switch.initialized   = true;

    pthread_mutex_unlock(&g_switch.lock);
    pthread_mutex_unlock(&g_switch_lifecycle_lock);

    LINKG_LOG_INFO("SWITCH: initialized, role=%d, node=%u",
                   (int)local->role,
                   (unsigned int)local->node_id);

    return 0;
}

/**
 * @brief 从空业务状态启动Switch Worker并注册Transport SWITCH交付回调。
 */
int linkg_switch_start(void)
{
    uintptr_t token;
    uint64_t  now_us;
    int       cleanup_ret;
    int       ret;

    pthread_mutex_lock(&g_switch_lifecycle_lock);
    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        ret = -ENODEV;
        goto out;
    }

    if (g_switch.running ||
        g_switch.handler_registered ||
        linkg_thread_is_started(&g_switch.thread))
    {
        ret = -EALREADY;
        goto out;
    }

    if (g_switch.rx_users != 0U)
    {
        ret = -EBUSY;
        goto out;
    }

    if (g_switch.run_token == UINTPTR_MAX)
    {
        ret = -EOVERFLOW;
        goto out;
    }

    now_us = linkg_time_monotonic_us();

    _linkg_switch_reset_runtime_locked();

    g_switch.next_observation_us = now_us;
    g_switch.next_reconcile_us   = now_us;
    g_switch.next_report_us      = now_us;

    token            = ++g_switch.run_token;
    g_switch.running = true;

    pthread_mutex_unlock(&g_switch.lock);

    /**
     * Switch RX会通过event_post唤醒Worker，
     * 因此必须先启动Worker，再开放Transport接收入口。
     */
    ret = linkg_thread_start(&g_switch.thread);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_switch.lock);

        g_switch.running = false;
        _linkg_switch_reset_runtime_locked();

        goto out;
    }

    // user_data仅作为当前运行代际的不透明整数cookie，从不解引用。
    ret = linkg_transport_register_handler(LINKG_TRANSPORT_TYPE_SWITCH, linkg_switch_transport_receive, (void *)token);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_switch.lock);
        g_switch.running = false;
        pthread_mutex_unlock(&g_switch.lock);

        cleanup_ret = linkg_thread_stop(&g_switch.thread);

        pthread_mutex_lock(&g_switch.lock);

        if (cleanup_ret == 0)
        {
            _linkg_switch_reset_runtime_locked();
        }
        else
        {
            ret = cleanup_ret;
        }

        goto out;
    }

    pthread_mutex_lock(&g_switch.lock);

    g_switch.handler_registered = true;

    pthread_mutex_unlock(&g_switch.lock);

    LINKG_LOG_INFO("SWITCH: started, role=%d, observation_ms=250, report_ms=250",
                   (int)g_switch.role);

    pthread_mutex_unlock(&g_switch_lifecycle_lock);

    return 0;

out:
    pthread_mutex_unlock(&g_switch.lock);
    pthread_mutex_unlock(&g_switch_lifecycle_lock);

    return ret;
}

/**
 * @brief 停止Switch Worker、注销Transport回调并清空全部业务运行状态。
 *
 * 排空已进入的RX访问者后清除Peer、发送计划和全部控制事务。
 *
 * 调用方必须从应用控制线程执行，禁止从Switch Worker内部调用。
 */
int linkg_switch_stop(void)
{
    bool registered;
    int  first_error;
    int  ret;

    pthread_mutex_lock(&g_switch_lifecycle_lock);
    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        pthread_mutex_unlock(&g_switch_lifecycle_lock);
        return 0;
    }

    if (g_switch.worker_tid_valid &&
        pthread_equal(pthread_self(), g_switch.worker_tid))
    {
        pthread_mutex_unlock(&g_switch.lock);
        pthread_mutex_unlock(&g_switch_lifecycle_lock);
        return -EDEADLK;
    }

    g_switch.running = false;
    registered       = g_switch.handler_registered;

    pthread_mutex_unlock(&g_switch.lock);

    first_error = 0;

    if (registered)
    {
        ret = linkg_transport_unregister_handler(LINKG_TRANSPORT_TYPE_SWITCH);
        if (ret != 0 && ret != -ENOENT)
        {
            first_error = ret;
        }
        else
        {
            pthread_mutex_lock(&g_switch.lock);

            g_switch.handler_registered = false;

            pthread_mutex_unlock(&g_switch.lock);
        }
    }

    ret = linkg_thread_stop(&g_switch.thread);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_switch_lifecycle_lock);
        return ret;
    }

    pthread_mutex_lock(&g_switch.lock);

    ret = _linkg_switch_drain_receive_locked();
    if (ret == 0)
    {
        _linkg_switch_reset_runtime_locked();
    }

    pthread_mutex_unlock(&g_switch.lock);
    pthread_mutex_unlock(&g_switch_lifecycle_lock);

    if (first_error != 0)
    {
        return first_error;
    }

    return ret;
}

/**
 * @brief 释放已经停止的Switch模块初始化资源。
 *
 * 永久Mutex、Condition及进程内代际计数保留，用于隔离迟到回调。
 */
int linkg_switch_deinit(void)
{
    int ret;

    pthread_mutex_lock(&g_switch_lifecycle_lock);
    pthread_mutex_lock(&g_switch.lock);

    ret = 0;

    if (!g_switch.initialized)
    {
        goto out;
    }

    if (g_switch.running ||
        g_switch.handler_registered ||
        g_switch.rx_users != 0U ||
        linkg_thread_is_started(&g_switch.thread))
    {
        ret = -EBUSY;
        goto out;
    }

    g_switch.initialized   = false;
    g_switch.packet_pool   = NULL;
    g_switch.role          = LINKG_DEVICE_ROLE_UNKNOWN;
    g_switch.local_node_id = LINKG_RESOURCE_NODE_ID_INVALID;

    // Switch退出时不保留任何Peer业务状态；安全代际编号仍留在进程内。
    _linkg_switch_reset_runtime_locked();

    pthread_mutex_unlock(&g_switch.lock);

    linkg_thread_deinit(&g_switch.thread);

    pthread_mutex_unlock(&g_switch_lifecycle_lock);

    return 0;

out:
    pthread_mutex_unlock(&g_switch.lock);
    pthread_mutex_unlock(&g_switch_lifecycle_lock);

    return ret;
}

/****************************** 计划管理 ******************************/

/**
 * @brief 设置或更新指定直接Peer发送计划。
 */
int linkg_switch_set_plan(uint8_t peer_node_id, const linkg_send_plan_t *plan)
{
    return linkg_switch_plan_set(peer_node_id, plan);
}

/**
 * @brief 获取指定直接Peer当前发送计划。
 */
int linkg_switch_get_plan(uint8_t peer_node_id, linkg_send_plan_t *plan)
{
    return linkg_switch_plan_get(peer_node_id, plan);
}

/**
 * @brief 读取指定业务计划，所有写入仍以完整Peer计划为单位。
 */
int linkg_switch_get_class_plan(uint8_t peer_node_id, linkg_transport_class_t traffic_class, linkg_class_send_plan_t *plan)
{
    linkg_send_plan_t peer_plan;
    int               ret;

    if (plan == NULL || (uint32_t)traffic_class >= LINKG_TRANSPORT_CLASS_COUNT)
    {
        return -EINVAL;
    }

    ret = linkg_switch_plan_get(peer_node_id, &peer_plan);
    if (ret != 0)
    {
        return ret;
    }

    *plan = peer_plan.classes[traffic_class];

    return 0;
}

/**
 * @brief 删除指定直接Peer全部Switch运行状态。
 */
int linkg_switch_remove_plan(uint8_t peer_node_id)
{
    return linkg_switch_plan_remove(peer_node_id);
}

/****************************** 接入维护 ******************************/

/**
 * @brief 开始本机指定Access维护。
 */
int linkg_switch_begin_maintenance(linkg_link_access_t access)
{
    return linkg_switch_maintenance_begin(access);
}

/**
 * @brief 结束本机指定Access维护。
 */
int linkg_switch_end_maintenance(linkg_link_access_t access)
{
    return linkg_switch_maintenance_end(access);
}

/****************************** 运行控制 ******************************/

/**
 * @brief 请求Switch Worker立即执行一次发送计划资源校准。
 *
 * AP和STA均支持，多次未处理请求合并为一次。
 * 不改变500ms Reconcile周期，也不提前执行STA质量切换策略。
 */
int linkg_switch_wakeup(void)
{
    int ret;

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

    if (g_switch.reconcile_pending)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    g_switch.reconcile_pending = true;

    ret = linkg_thread_wakeup(&g_switch.thread);
    if (ret != 0)
    {
        g_switch.reconcile_pending = false;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return ret;
}
