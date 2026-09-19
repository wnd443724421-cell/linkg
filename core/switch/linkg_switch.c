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
 * @brief 清理单次运行状态并保留当前Peer发送计划和本地主动消息序列。
 *
 * STA保留下一PLAN_SYNC消息编号，AP保留下一质量报告消息编号，
 * Switch同时保留下一Maintenance事务编号，避免普通stop/start后
 * 本地主动消息序列重新从零开始。
 *
 * 当前Maintenance事务、远端Maintenance状态和END确认事务均属于
 * 单次运行状态，必须在新一轮start前清理。
 */
static void _linkg_switch_reset_session_locked(void)
{
    linkg_switch_peer_runtime_t *peer;
    uint32_t                     message_id;
    uint32_t                     index;

    linkg_switch_event_reset_locked();

    memset(&g_switch.local_maintenance, 0, sizeof(g_switch.local_maintenance));

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        // Maintenance属于Peer公共单次运行状态，不随角色保留。
        memset(&peer->maintenance, 0, sizeof(peer->maintenance));

        if (!peer->used)
        {
            continue;
        }

        if (g_switch.role == LINKG_DEVICE_ROLE_STA)
        {
            message_id = peer->role.sta.next_plan_message_id;

            memset(&peer->role, 0, sizeof(peer->role));

            peer->role.sta.next_plan_message_id = message_id;
        }
        else if (g_switch.role == LINKG_DEVICE_ROLE_AP)
        {
            message_id = peer->role.ap.report_message_id;

            memset(&peer->role, 0, sizeof(peer->role));

            peer->role.ap.report_message_id = message_id;
        }
        else
        {
            memset(&peer->role, 0, sizeof(peer->role));
        }
    }

    g_switch.next_observation_us = 0U;
    g_switch.next_report_us      = 0U;
}


/**
 * @brief 清空全部Peer及单次运行状态，不回退Transport Handler运行代际。
 */
static void _linkg_switch_reset_all_locked(void)
{
    memset(g_switch.peers, 0, sizeof(g_switch.peers));
    memset(&g_switch.local_maintenance, 0, sizeof(g_switch.local_maintenance));

    linkg_switch_event_reset_locked();

    g_switch.next_peer_generation         = 0U;
    g_switch.next_maintenance_message_id  = 0U;
    g_switch.next_observation_us          = 0U;
    g_switch.next_report_us               = 0U;
    g_switch.rx_users                     = 0U;
    g_switch.worker_tid_valid             = false;
    g_switch.handler_registered           = false;
    g_switch.running                      = false;
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
        accepted       = true;
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

    _linkg_switch_reset_all_locked();

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
 * @brief 启动Switch Worker并注册Transport SWITCH交付回调。
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

    _linkg_switch_reset_session_locked();

    g_switch.next_observation_us = now_us;
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
        _linkg_switch_reset_session_locked();

        goto out;
    }

    // user_data仅作为当前运行代际的不透明整数cookie，从不解引用。
    ret = linkg_transport_register_handler(LINKG_TRANSPORT_TYPE_SWITCH,
                                           linkg_switch_transport_receive,
                                           (void *)token);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_switch.lock);
        g_switch.running = false;
        pthread_mutex_unlock(&g_switch.lock);

        cleanup_ret = linkg_thread_stop(&g_switch.thread);

        pthread_mutex_lock(&g_switch.lock);
        _linkg_switch_reset_session_locked();

        if (cleanup_ret != 0)
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
 * @brief 停止Switch Worker、注销Transport回调并排空已进入的RX访问者。
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
        _linkg_switch_reset_session_locked();
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
 * 永久Mutex、Condition和run_token保留，用于隔离延迟到达的旧Transport回调。
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

    memset(g_switch.peers, 0, sizeof(g_switch.peers));
    memset(&g_switch.local_maintenance, 0, sizeof(g_switch.local_maintenance));

    linkg_switch_event_reset_locked();

    g_switch.next_peer_generation         = 0U;
    g_switch.next_maintenance_message_id  = 0U;
    g_switch.next_observation_us          = 0U;
    g_switch.next_report_us               = 0U;
    g_switch.worker_tid_valid             = false;

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
