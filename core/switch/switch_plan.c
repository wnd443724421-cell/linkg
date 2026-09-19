/**
 * @file switch_plan.c
 * @brief LinkG链路切换发送计划实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#include "switch_plan.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_system_resources.h"

#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_tx.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_PLAN_MESSAGE_ID_HALF_RANGE  0x80000000U // 32位消息编号前后关系半区间
#define LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US 200000ULL   // 发送计划同步重试周期
#define LINKG_SWITCH_PLAN_SYNC_TIMEOUT_US        2000000ULL  // 发送计划同步事务超时时间
#define LINKG_SWITCH_PLAN_SYNC_RETRY_MAX         5U          // 单次发送计划同步最大尝试次数

/****************************** Peer管理 ******************************/

/**
 * @brief 校验直接Peer节点编号。
 */
static bool _linkg_switch_plan_peer_id_valid(uint8_t peer_node_id)
{
    return peer_node_id >= LINKG_RESOURCE_NODE_ID_MIN && peer_node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 查找空闲Peer运行槽位，调用方持有Switch锁。
 */
static linkg_switch_peer_runtime_t *_linkg_switch_plan_find_unused_peer_locked(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            return &g_switch.peers[index];
        }
    }

    return NULL;
}

static uint32_t _linkg_switch_plan_allocate_peer_generation_locked(void)
{
    uint32_t generation;

    generation = ++g_switch.next_peer_generation;

    if (generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        generation = ++g_switch.next_peer_generation;
    }

    return generation;
}


/**
 * @brief 查找指定直接Peer运行槽位，调用方持有Switch锁。
 */
linkg_switch_peer_runtime_t *linkg_switch_find_peer_locked(uint8_t peer_node_id)
{
    uint32_t index;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        if (g_switch.peers[index].peer_node_id == peer_node_id)
        {
            return &g_switch.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 按节点编号和运行代际查找当前直接Peer，调用方持有Switch锁。
 *
 * 只有节点编号和generation同时匹配当前Peer Runtime时才返回Peer，
 * 用于阻止旧Event或延迟Action作用到同节点编号的新一代Peer。
 */
linkg_switch_peer_runtime_t *linkg_switch_find_peer_generation_locked(uint8_t peer_node_id, uint32_t generation)
{
    linkg_switch_peer_runtime_t *peer;

    if (generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        return NULL;
    }

    peer = linkg_switch_find_peer_locked(peer_node_id);
    if (peer == NULL || peer->generation != generation)
    {
        return NULL;
    }

    return peer;
}

/**
 * @brief 判断指定节点编号和运行代际是否仍对应当前有效直接Peer。
 *
 * 本接口内部获取Switch锁，可供解锁后的延迟操作在继续执行前
 * 确认目标Peer未发生删除和重新建立。
 */
bool linkg_switch_peer_generation_current(uint8_t peer_node_id, uint32_t generation)
{
    bool current;

    current = false;

    pthread_mutex_lock(&g_switch.lock);

    if (g_switch.initialized && g_switch.running)
    {
        current = linkg_switch_find_peer_generation_locked(peer_node_id, generation) != NULL;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return current;
}

/****************************** 计划校验 ******************************/

/**
 * @brief 校验本地发送计划字段组合。
 */
static bool _linkg_switch_plan_valid(const linkg_send_plan_t *plan)
{
    if (plan == NULL)
    {
        return false;
    }

    if (plan->mode == LINKG_SEND_MODE_NONE)
    {
        return plan->primary_link_id == LINKG_LINK_ID_INVALID && plan->secondary_link_id == LINKG_LINK_ID_INVALID;
    }

    if (plan->mode == LINKG_SEND_MODE_SINGLE)
    {
        if (plan->primary_link_id == LINKG_LINK_ID_INVALID)
        {
            return false;
        }

        return plan->secondary_link_id == LINKG_LINK_ID_INVALID || plan->secondary_link_id != plan->primary_link_id;
    }

    if (plan->mode == LINKG_SEND_MODE_REDUNDANT)
    {
        return plan->primary_link_id != LINKG_LINK_ID_INVALID &&
               plan->secondary_link_id != LINKG_LINK_ID_INVALID &&
               plan->primary_link_id != plan->secondary_link_id;
    }

    return false;
}

/**
 * @brief 判断两个本地发送计划是否完全一致。
 */
static bool _linkg_switch_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    if (left == NULL || right == NULL)
    {
        return false;
    }

    return left->mode == right->mode &&
           left->primary_link_id == right->primary_link_id &&
           left->secondary_link_id == right->secondary_link_id;
}

/**
 * @brief 判断消息编号是否晚于当前编号，并正确处理32位回绕。
 */
static bool _linkg_switch_plan_message_id_newer(uint32_t message_id, uint32_t current_message_id)
{
    uint32_t delta;

    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return false;
    }

    if (current_message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return true;
    }

    delta = message_id - current_message_id;

    return delta != 0U && delta < LINKG_SWITCH_PLAN_MESSAGE_ID_HALF_RANGE;
}

/**
 * @brief 为STA发送计划同步事务分配非零消息编号，调用方持有Switch锁。
 */
static uint32_t _linkg_switch_plan_allocate_message_id_locked(linkg_switch_sta_peer_runtime_t *runtime)
{
    uint32_t message_id;

    message_id = ++runtime->next_plan_message_id;

    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        message_id = ++runtime->next_plan_message_id;
    }

    return message_id;
}

/****************************** 接入转换 ******************************/

/**
 * @brief 将本地Link编号转换为Switch Wire接入类型。
 */
static int _linkg_switch_plan_link_to_access(uint32_t link_id, linkg_switch_wire_access_t *access)
{
    uint32_t wifi_link_id;
    uint32_t cellular_link_id;

    if (access == NULL)
    {
        return -EINVAL;
    }

    *access = LINKG_SWITCH_WIRE_ACCESS_NONE;

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    wifi_link_id     = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    if (link_id == wifi_link_id)
    {
        *access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
        return 0;
    }

    if (link_id == cellular_link_id)
    {
        *access = LINKG_SWITCH_WIRE_ACCESS_CELLULAR;
        return 0;
    }

    return -ENOENT;
}

/**
 * @brief 将Switch Wire接入类型转换为本机Link编号。
 */
static int _linkg_switch_plan_access_to_link(linkg_switch_wire_access_t access, uint32_t *link_id)
{
    if (link_id == NULL)
    {
        return -EINVAL;
    }

    *link_id = LINKG_LINK_ID_INVALID;

    if (access == LINKG_SWITCH_WIRE_ACCESS_NONE)
    {
        return 0;
    }

    if (access == LINKG_SWITCH_WIRE_ACCESS_WIFI)
    {
        *link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    }
    else if (access == LINKG_SWITCH_WIRE_ACCESS_CELLULAR)
    {
        *link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);
    }
    else
    {
        return -EINVAL;
    }

    return *link_id == LINKG_LINK_ID_INVALID ? -ENOENT : 0;
}

/**
 * @brief 将本地发送计划转换为Switch Wire逻辑发送计划。
 */
static int _linkg_switch_plan_local_to_wire(const linkg_send_plan_t *plan, linkg_switch_wire_plan_sync_t *wire_plan)
{
    int ret;

    if (plan == NULL || wire_plan == NULL)
    {
        return -EINVAL;
    }

    memset(wire_plan, 0, sizeof(*wire_plan));

    if (plan->mode == LINKG_SEND_MODE_SINGLE)
    {
        wire_plan->mode = LINKG_SWITCH_WIRE_MODE_SINGLE;
    }
    else if (plan->mode == LINKG_SEND_MODE_REDUNDANT)
    {
        wire_plan->mode = LINKG_SWITCH_WIRE_MODE_REDUNDANT;
    }
    else
    {
        return -EINVAL;
    }

    ret = _linkg_switch_plan_link_to_access(plan->primary_link_id, &wire_plan->primary_access);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_switch_plan_link_to_access(plan->secondary_link_id, &wire_plan->secondary_access);
    if (ret != 0)
    {
        return ret;
    }

    if (wire_plan->primary_access == LINKG_SWITCH_WIRE_ACCESS_NONE)
    {
        return -EINVAL;
    }

    if (wire_plan->primary_access == wire_plan->secondary_access)
    {
        return -EINVAL;
    }

    if (wire_plan->mode == LINKG_SWITCH_WIRE_MODE_REDUNDANT && wire_plan->secondary_access == LINKG_SWITCH_WIRE_ACCESS_NONE)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 判断逻辑发送计划是否使用指定本地Access。
 */
static bool _linkg_switch_plan_wire_uses_access(const linkg_switch_wire_plan_sync_t *wire_plan, linkg_link_access_t access)
{
    linkg_switch_wire_access_t wire_access;

    if (wire_plan == NULL)
    {
        return false;
    }

    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        wire_access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    }
    else if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        wire_access = LINKG_SWITCH_WIRE_ACCESS_CELLULAR;
    }
    else
    {
        return false;
    }

    return wire_plan->primary_access == wire_access || wire_plan->secondary_access == wire_access;
}

/**
 * @brief 判断新计划是否使用当前Maintenance已经Block的Access。
 *
 * 调用方必须持有g_switch.lock。
 */
static bool _linkg_switch_plan_wire_blocked_locked(const linkg_switch_peer_runtime_t *peer, const linkg_switch_wire_plan_sync_t *wire_plan)
{
    const linkg_switch_maintenance_peer_runtime_t *maintenance;

    if (wire_plan == NULL)
    {
        return false;
    }

    maintenance = peer == NULL ? NULL : &peer->maintenance;

    if (_linkg_switch_plan_wire_uses_access(wire_plan, LINKG_LINK_ACCESS_WIFI) &&
        linkg_switch_maintenance_access_blocked_locked(maintenance, LINKG_LINK_ACCESS_WIFI))
    {
        return true;
    }

    return _linkg_switch_plan_wire_uses_access(wire_plan, LINKG_LINK_ACCESS_CELLULAR) &&
           linkg_switch_maintenance_access_blocked_locked(maintenance, LINKG_LINK_ACCESS_CELLULAR);
}

/**
 * @brief 判断是否存在会约束指定Peer计划写入的Maintenance事务。
 *
 * 调用方必须持有g_switch.lock。
 */
static bool _linkg_switch_plan_maintenance_active_locked(const linkg_switch_peer_runtime_t *peer)
{
    return g_switch.local_maintenance.active || (peer != NULL && peer->maintenance.remote.active);
}

/**
 * @brief 将Switch Wire逻辑发送计划转换为本机发送计划。
 */
static int _linkg_switch_plan_wire_to_local(const linkg_switch_wire_plan_sync_t *wire_plan, linkg_send_plan_t *plan)
{
    int ret;

    if (wire_plan == NULL || plan == NULL)
    {
        return -EINVAL;
    }

    memset(plan, 0, sizeof(*plan));
    plan->primary_link_id   = LINKG_LINK_ID_INVALID;
    plan->secondary_link_id = LINKG_LINK_ID_INVALID;

    if (wire_plan->mode == LINKG_SWITCH_WIRE_MODE_SINGLE)
    {
        plan->mode = LINKG_SEND_MODE_SINGLE;
    }
    else if (wire_plan->mode == LINKG_SWITCH_WIRE_MODE_REDUNDANT)
    {
        plan->mode = LINKG_SEND_MODE_REDUNDANT;
    }
    else
    {
        return -EINVAL;
    }

    ret = _linkg_switch_plan_access_to_link(wire_plan->primary_access, &plan->primary_link_id);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_switch_plan_access_to_link(wire_plan->secondary_access, &plan->secondary_link_id);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_switch_plan_valid(plan) ? 0 : -EINVAL;
}

/****************************** Path校验 ******************************/

/**
 * @brief 判断指定Peer当前是否存在可运行的目标Path。
 */
static int _linkg_switch_plan_validate_path(uint8_t peer_node_id, uint32_t link_id)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    int                   ret;

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return -ENOENT;
    }

    link = linkg_link_manager_get(link_id);
    if (link == NULL)
    {
        return -ENOENT;
    }

    if (!linkg_link_is_running(link))
    {
        return -ENETDOWN;
    }

    memset(&endpoint, 0, sizeof(endpoint));
    path = NULL;

    ret = linkg_node_acquire_path(peer_node_id, link_id, &path, &endpoint);
    if (ret != 0)
    {
        return ret;
    }

    linkg_path_release(path);

    return 0;
}

/**
 * @brief 校验AP收到的发送计划当前具备实际执行条件。
 */
static int _linkg_switch_plan_validate_remote_target(uint8_t peer_node_id, const linkg_send_plan_t *plan)
{
    int ret;

    ret = _linkg_switch_plan_validate_path(peer_node_id, plan->primary_link_id);
    if (ret != 0)
    {
        return ret;
    }

    if (plan->mode != LINKG_SEND_MODE_REDUNDANT)
    {
        return 0;
    }

    return _linkg_switch_plan_validate_path(peer_node_id, plan->secondary_link_id);
}

/****************************** AP同步处理 ******************************/

/**
 * @brief 处理AP收到的一条STA发送计划同步事件。
 */
static int _linkg_switch_plan_process_sync_event(const linkg_switch_event_t *event)
{
    linkg_switch_wire_plan_ack_t    ack;
    linkg_switch_ap_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t    *peer;
    linkg_packet_pool_t            *packet_pool;
    linkg_send_plan_t               plan;
    int32_t                         status;
    bool                            duplicate;
    bool                            stale;
    bool                            plan_converted;
    int                             ret;

    memset(&ack, 0, sizeof(ack));
    memset(&plan, 0, sizeof(plan));

    duplicate      = false;
    stale          = false;
    plan_converted = false;
    status         = 0;
    packet_pool    = NULL;

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

    if (g_switch.role != LINKG_DEVICE_ROLE_AP)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    runtime = &peer->role.ap;

    if (event->message_id == runtime->remote_plan_message_id && runtime->remote_plan_message_id != LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        duplicate   = true;
        status      = runtime->remote_plan_status;
        packet_pool = g_switch.packet_pool;
    }
    else if (!_linkg_switch_plan_message_id_newer(event->message_id, runtime->remote_plan_message_id))
    {
        stale = true;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (stale)
    {
        return 0;
    }

    if (duplicate)
    {
        ack.status = status;

        if (packet_pool == NULL)
        {
            return -ENODEV;
        }

        if (!linkg_switch_peer_generation_current(event->peer_node_id, event->peer_generation))
        {
            return -ESTALE;
        }

        return linkg_switch_tx_send_plan_ack(packet_pool, event->peer_node_id, event->message_id, &ack);
    }

    ret            = _linkg_switch_plan_wire_to_local(&event->payload.plan_sync, &plan);
    plan_converted = ret == 0;

    if (plan_converted)
    {
        ret = _linkg_switch_plan_validate_remote_target(event->peer_node_id, &plan);
    }

    status = ret;

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

    if (g_switch.role != LINKG_DEVICE_ROLE_AP)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    runtime = &peer->role.ap;

    if (plan_converted && _linkg_switch_plan_wire_blocked_locked(peer, &event->payload.plan_sync))
    {
        status = -EBUSY;
    }

    /**
     * Worker串行处理Plan事件，但仍重新检查消息编号，
     * 避免外部Peer生命周期变化后写回过期结果。
     */
    if (runtime->remote_plan_message_id != LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID &&
        !_linkg_switch_plan_message_id_newer(event->message_id, runtime->remote_plan_message_id))
    {
        if (event->message_id == runtime->remote_plan_message_id)
        {
            status = runtime->remote_plan_status;
        }
        else
        {
            pthread_mutex_unlock(&g_switch.lock);
            return 0;
        }
    }
    else
    {
        runtime->remote_plan_message_id = event->message_id;
        runtime->remote_plan_status     = status;

        if (status == 0)
        {
            peer->plan = plan;
        }
    }

    packet_pool = g_switch.packet_pool;

    pthread_mutex_unlock(&g_switch.lock);

    ack.status = status;

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    if (!linkg_switch_peer_generation_current(event->peer_node_id, event->peer_generation))
    {
        return -ESTALE;
    }

    return linkg_switch_tx_send_plan_ack(packet_pool, event->peer_node_id, event->message_id, &ack);
}


/****************************** STA确认处理 ******************************/

/**
 * @brief 处理STA收到的一条AP发送计划同步确认事件。
 */
static int _linkg_switch_plan_process_ack_event(const linkg_switch_event_t *event)
{
    linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_peer_runtime_t      *peer;

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

    peer = linkg_switch_find_peer_locked(event->peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    sync = &peer->role.sta.plan_sync;

    if (!sync->active || sync->message_id != event->message_id)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    sync->active        = false;
    sync->last_status   = event->payload.plan_ack.status;
    sync->next_retry_us = 0U;
    sync->deadline_us   = 0U;

    pthread_mutex_unlock(&g_switch.lock);

    if (event->payload.plan_ack.status == 0)
    {
        LINKG_LOG_DEBUG("SWITCH-PLAN: remote sync completed, peer=%u message=%u", (unsigned int)event->peer_node_id, (unsigned int)event->message_id);
    }
    else
    {
        LINKG_LOG_WARN("SWITCH-PLAN: remote sync rejected, peer=%u message=%u status=%d", (unsigned int)event->peer_node_id, (unsigned int)event->message_id, event->payload.plan_ack.status);
    }

    return 0;
}

/****************************** 计划管理 ******************************/

/**
 * @brief 设置或更新指定直接Peer发送计划，不触发远端同步。
 */
int linkg_switch_plan_set(uint8_t peer_node_id, const linkg_send_plan_t *plan)
{
    linkg_switch_peer_runtime_t  *peer;
    linkg_switch_wire_plan_sync_t wire_plan;
    bool                          wire_plan_valid;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) || !_linkg_switch_plan_valid(plan))
    {
        return -EINVAL;
    }

    memset(&wire_plan, 0, sizeof(wire_plan));
    wire_plan_valid = false;

    if (plan->mode != LINKG_SEND_MODE_NONE)
    {
        wire_plan_valid = _linkg_switch_plan_local_to_wire(plan, &wire_plan) == 0;
    }

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    peer = linkg_switch_find_peer_locked(peer_node_id);

    /**
     * 新Peer首次Plan属于Discovery恢复控制面的bootstrap例外。
     * 已有Peer的实际Plan变化必须继续受Maintenance Block约束。
     */
    if (peer != NULL && !_linkg_switch_plan_equal(&peer->plan, plan) && plan->mode != LINKG_SEND_MODE_NONE &&
        ((!wire_plan_valid && _linkg_switch_plan_maintenance_active_locked(peer)) ||
         (wire_plan_valid && _linkg_switch_plan_wire_blocked_locked(peer, &wire_plan))))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EBUSY;
    }

    if (peer == NULL)
    {
        peer = _linkg_switch_plan_find_unused_peer_locked();
        if (peer == NULL)
        {
            pthread_mutex_unlock(&g_switch.lock);
            return -ENOSPC;
        }

        memset(peer, 0, sizeof(*peer));
        peer->used         = true;
        peer->peer_node_id = peer_node_id;
        peer->generation   = _linkg_switch_plan_allocate_peer_generation_locked();
    }

    peer->plan = *plan;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}


/**
 * @brief 获取指定直接Peer当前发送计划。
 */
int linkg_switch_plan_get(uint8_t peer_node_id, linkg_send_plan_t *plan)
{
    linkg_switch_peer_runtime_t *peer;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) || plan == NULL)
    {
        return -EINVAL;
    }

    memset(plan, 0, sizeof(*plan));

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    peer = linkg_switch_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    *plan = peer->plan;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 删除指定直接Peer全部Switch运行状态。
 */
int linkg_switch_plan_remove(uint8_t peer_node_id)
{
    linkg_switch_peer_runtime_t *peer;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    peer = linkg_switch_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    memset(peer, 0, sizeof(*peer));

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 仅在本机应用指定直接Peer发送计划，不触发远端计划同步。
 *
 * 本接口用于Maintenance等双方已经通过其它控制协议完成协调的场景。
 * 调用成功后新的发送计划立即成为该Peer唯一生效计划，
 * 不创建PLAN_SYNC事务，也不发送任何Switch控制消息。
 */
int linkg_switch_plan_apply_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan)
{
    linkg_switch_peer_runtime_t  *peer;
    linkg_switch_wire_plan_sync_t wire_plan;
    bool                          wire_plan_valid;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) || peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID || !_linkg_switch_plan_valid(plan))
    {
        return -EINVAL;
    }

    memset(&wire_plan, 0, sizeof(wire_plan));
    wire_plan_valid = false;

    /**
     * NONE是合法的本地停发计划，但没有对应的PLAN_SYNC Wire表示。
     * 只有实际携带Access的计划才需要执行Maintenance Block检查。
     */
    if (plan->mode != LINKG_SEND_MODE_NONE)
    {
        wire_plan_valid = _linkg_switch_plan_local_to_wire(plan, &wire_plan) == 0;
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

    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    if ((!wire_plan_valid && plan->mode != LINKG_SEND_MODE_NONE && _linkg_switch_plan_maintenance_active_locked(peer)) ||
        (wire_plan_valid && _linkg_switch_plan_wire_blocked_locked(peer, &wire_plan)))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EBUSY;
    }

    peer->plan = *plan;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}


/**
 * @brief STA提交本地新发送计划并启动对应AP远端同步事务。
 *
 * 本地Plan在发送PLAN_SYNC前立即生效；发送失败不会回滚本地Plan，
 * 当前同步事务保留并由Switch Worker按截止时间继续重试。
 */
int linkg_switch_plan_commit_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan, uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_peer_runtime_t      *peer;
    linkg_switch_wire_plan_sync_t     wire_plan;
    linkg_packet_pool_t              *packet_pool;
    uint32_t                          message_id;
    int                               ret;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) || peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID ||
        !_linkg_switch_plan_valid(plan) || now_us == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_switch_plan_local_to_wire(plan, &wire_plan);
    if (ret != 0)
    {
        return ret;
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
        return -EPERM;
    }

    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    if (_linkg_switch_plan_wire_blocked_locked(peer, &wire_plan))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EBUSY;
    }

    sync       = &peer->role.sta.plan_sync;
    message_id = _linkg_switch_plan_allocate_message_id_locked(&peer->role.sta);

    peer->plan          = *plan;
    sync->active        = true;
    sync->message_id    = message_id;
    sync->wire_plan     = wire_plan;
    sync->retry_count   = 1U;
    sync->next_retry_us = now_us + LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US;
    sync->deadline_us   = now_us + LINKG_SWITCH_PLAN_SYNC_TIMEOUT_US;
    sync->last_status   = -EINPROGRESS;

    packet_pool = g_switch.packet_pool;

    pthread_mutex_unlock(&g_switch.lock);

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    if (!linkg_switch_peer_generation_current(peer_node_id, peer_generation))
    {
        return -ESTALE;
    }

    return linkg_switch_tx_send_plan_sync(packet_pool, peer_node_id, message_id, &wire_plan);
}


/****************************** 事件处理 ******************************/

/**
 * @brief 处理Switch Worker取出的发送计划控制事件。
 */
int linkg_switch_plan_process_event(const linkg_switch_event_t *event, uint64_t now_us)
{
    if (event == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (event->type == LINKG_SWITCH_EVENT_PLAN_SYNC_RX)
    {
        return _linkg_switch_plan_process_sync_event(event);
    }

    if (event->type == LINKG_SWITCH_EVENT_PLAN_ACK_RX)
    {
        return _linkg_switch_plan_process_ack_event(event);
    }

    return -EINVAL;
}

/****************************** 周期处理 ******************************/

/**
 * @brief 处理STA当前到期的发送计划同步重试和超时。
 */
int linkg_switch_plan_process(uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t sync_copy;
    linkg_switch_peer_runtime_t     *peer;
    linkg_packet_pool_t             *packet_pool;
    uint32_t                         peer_generation;
    uint32_t                         index;
    uint8_t                          peer_node_id;
    int                              first_error;
    int                              ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    first_error = 0;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        memset(&sync_copy, 0, sizeof(sync_copy));
        packet_pool     = NULL;
        peer_generation = LINKG_SWITCH_PEER_GENERATION_INVALID;
        peer_node_id    = 0U;

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

        peer = &g_switch.peers[index];

        if (!peer->used || !peer->role.sta.plan_sync.active)
        {
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        if (now_us >= peer->role.sta.plan_sync.deadline_us || peer->role.sta.plan_sync.retry_count >= LINKG_SWITCH_PLAN_SYNC_RETRY_MAX)
        {
            peer->role.sta.plan_sync.active        = false;
            peer->role.sta.plan_sync.last_status   = -ETIMEDOUT;
            peer->role.sta.plan_sync.next_retry_us = 0U;
            peer->role.sta.plan_sync.deadline_us   = 0U;

            peer_node_id = peer->peer_node_id;

            pthread_mutex_unlock(&g_switch.lock);

            LINKG_LOG_WARN("SWITCH-PLAN: remote sync timeout, peer=%u", (unsigned int)peer_node_id);
            continue;
        }

        if (now_us < peer->role.sta.plan_sync.next_retry_us)
        {
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        peer->role.sta.plan_sync.retry_count++;
        peer->role.sta.plan_sync.next_retry_us = now_us + LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US;

        sync_copy       = peer->role.sta.plan_sync;
        peer_generation = peer->generation;
        peer_node_id    = peer->peer_node_id;
        packet_pool     = g_switch.packet_pool;

        pthread_mutex_unlock(&g_switch.lock);

        if (packet_pool == NULL)
        {
            if (first_error == 0)
            {
                first_error = -ENODEV;
            }

            continue;
        }

        if (!linkg_switch_peer_generation_current(peer_node_id, peer_generation))
        {
            continue;
        }

        ret = linkg_switch_tx_send_plan_sync(packet_pool, peer_node_id, sync_copy.message_id, &sync_copy.wire_plan);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/**
 * @brief 获取当前STA发送计划同步模块的最近截止时间，调用方持有Switch锁。
 */
uint64_t linkg_switch_plan_next_deadline_locked(void)
{
    linkg_switch_plan_sync_runtime_t *sync;
    uint64_t                          deadline_us;
    uint32_t                          index;

    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        return UINT64_MAX;
    }

    deadline_us = UINT64_MAX;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        sync = &g_switch.peers[index].role.sta.plan_sync;
        if (!sync->active)
        {
            continue;
        }

        if (sync->next_retry_us < deadline_us)
        {
            deadline_us = sync->next_retry_us;
        }

        if (sync->deadline_us < deadline_us)
        {
            deadline_us = sync->deadline_us;
        }
    }

    return deadline_us;
}
