/**
 * @file switch_plan.c
 * @brief LinkG完整发送计划管理、协议同步和通用同步结果
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-21
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
#include "linkg_time.h"

#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_tx.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_PLAN_SYNC_ID_INVALID        0U          // 无效本地逻辑同步编号
#define LINKG_SWITCH_PLAN_MESSAGE_ID_HALF_RANGE  0x80000000U // 32位消息编号前后关系半区间
#define LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US 200000ULL   // 发送计划同步重试周期
#define LINKG_SWITCH_PLAN_SYNC_TIMEOUT_US        2000000ULL  // 发送计划同步事务超时时间
#define LINKG_SWITCH_PLAN_SYNC_RECOVERY_MAX      3U          // 单个完整计划最多追加三轮恢复事务
#define LINKG_SWITCH_PLAN_SYNC_RECOVERY_BASE_US  1000000ULL  // 恢复事务初始退避时间
#define LINKG_SWITCH_PLAN_SYNC_RETRY_MAX         5U          // 单次发送计划同步最大尝试次数


/****************************** Peer运行槽位 ******************************/

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

/**
 * @brief 分配非零Peer运行代际，调用方持有Switch锁。
 */
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


/****************************** 完整发送计划校验 ******************************/

/**
 * @brief 校验本地发送计划字段组合。
 */
static bool _linkg_switch_class_plan_valid(const linkg_class_send_plan_t *plan)
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
 * @brief 校验完整Peer计划，任一业务非法时拒绝整组写入。
 */
static bool _linkg_switch_plan_valid(const linkg_send_plan_t *plan)
{
    uint32_t class_index;

    if (plan == NULL)
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (!_linkg_switch_class_plan_valid(&plan->classes[class_index]))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 判断两个本地发送计划是否完全一致。
 */
static bool _linkg_switch_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    uint32_t class_index;

    if (left == NULL || right == NULL)
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (left->classes[class_index].mode != right->classes[class_index].mode ||
            left->classes[class_index].primary_link_id != right->classes[class_index].primary_link_id ||
            left->classes[class_index].secondary_link_id != right->classes[class_index].secondary_link_id)
        {
            return false;
        }
    }

    return true;
}


/****************************** 计划协议转换 ******************************/

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
    const linkg_class_send_plan_t       *current;
    linkg_switch_wire_class_send_plan_t *destination;
    uint32_t                            class_index;
    int                                 ret;

    if (wire_plan == NULL || !_linkg_switch_plan_valid(plan))
    {
        return -EINVAL;
    }

    memset(wire_plan, 0, sizeof(*wire_plan));

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current     = &plan->classes[class_index];
        destination = &wire_plan->classes[class_index];

        if (current->mode == LINKG_SEND_MODE_NONE)
        {
            destination->mode = LINKG_SWITCH_WIRE_MODE_NONE;
            continue;
        }

        destination->mode = current->mode == LINKG_SEND_MODE_SINGLE ? LINKG_SWITCH_WIRE_MODE_SINGLE : LINKG_SWITCH_WIRE_MODE_REDUNDANT;

        ret = _linkg_switch_plan_link_to_access(current->primary_link_id, &destination->primary_access);
        if (ret != 0)
        {
            return ret;
        }

        ret = _linkg_switch_plan_link_to_access(current->secondary_link_id, &destination->secondary_access);
        if (ret != 0)
        {
            return ret;
        }

        if (destination->primary_access == LINKG_SWITCH_WIRE_ACCESS_NONE ||
            destination->primary_access == destination->secondary_access)
        {
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief 将Switch Wire逻辑发送计划转换为本机发送计划。
 */
static int _linkg_switch_plan_wire_to_local(const linkg_switch_wire_plan_sync_t *wire_plan, linkg_send_plan_t *plan)
{
    const linkg_switch_wire_class_send_plan_t *current;
    linkg_class_send_plan_t                   *destination;
    uint32_t                                  class_index;
    int                                       ret;

    if (wire_plan == NULL || plan == NULL)
    {
        return -EINVAL;
    }

    memset(plan, 0, sizeof(*plan));

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current     = &wire_plan->classes[class_index];
        destination = &plan->classes[class_index];

        if (current->mode == LINKG_SWITCH_WIRE_MODE_NONE)
        {
            if (current->primary_access != LINKG_SWITCH_WIRE_ACCESS_NONE || current->secondary_access != LINKG_SWITCH_WIRE_ACCESS_NONE)
            {
                return -EINVAL;
            }

            continue;
        }

        if (current->mode != LINKG_SWITCH_WIRE_MODE_SINGLE && current->mode != LINKG_SWITCH_WIRE_MODE_REDUNDANT)
        {
            return -EINVAL;
        }

        destination->mode = current->mode == LINKG_SWITCH_WIRE_MODE_SINGLE ? LINKG_SEND_MODE_SINGLE : LINKG_SEND_MODE_REDUNDANT;

        ret = _linkg_switch_plan_access_to_link(current->primary_access, &destination->primary_link_id);
        if (ret != 0)
        {
            return ret;
        }

        ret = _linkg_switch_plan_access_to_link(current->secondary_access, &destination->secondary_link_id);
        if (ret != 0)
        {
            return ret;
        }
    }

    return _linkg_switch_plan_valid(plan) ? 0 : -EINVAL;
}


/****************************** Maintenance计划约束 ******************************/

/**
 * @brief 判断完整计划是否实际使用指定本地Link。
 *
 * NONE不发送；SINGLE仅使用Primary，Secondary只是备用候选；
 * REDUNDANT同时使用Primary和Secondary。
 */
static bool _linkg_switch_plan_uses_link(const linkg_send_plan_t *plan, uint32_t link_id)
{
    const linkg_class_send_plan_t *current;
    uint32_t                      class_index;

    if (plan == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &plan->classes[class_index];

        if (current->mode == LINKG_SEND_MODE_NONE)
        {
            continue;
        }

        if (current->primary_link_id == link_id)
        {
            return true;
        }

        if (current->mode == LINKG_SEND_MODE_REDUNDANT && current->secondary_link_id == link_id)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断新计划是否实际使用本地或对端Maintenance禁止的Access。
 *
 * 仅约束真正参与发送的链路；不禁止SINGLE模式的备用候选。
 * 调用方必须持有g_switch.lock。
 */
static bool _linkg_switch_plan_access_blocked_locked(const linkg_switch_peer_runtime_t *peer, const linkg_send_plan_t *plan)
{
    const linkg_switch_maintenance_peer_runtime_t *maintenance;
    uint32_t                                      link_id;

    if (peer == NULL || plan == NULL)
    {
        return false;
    }

    maintenance = &peer->maintenance;

    if (linkg_switch_maintenance_access_blocked_locked(maintenance, LINKG_LINK_ACCESS_WIFI))
    {
        link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
        if (_linkg_switch_plan_uses_link(plan, link_id))
        {
            return true;
        }
    }

    if (linkg_switch_maintenance_access_blocked_locked(maintenance, LINKG_LINK_ACCESS_CELLULAR))
    {
        link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);
        if (_linkg_switch_plan_uses_link(plan, link_id))
        {
            return true;
        }
    }

    return false;
}


/****************************** 实际发送Path校验 ******************************/

/**
 * @brief 判断指定Peer当前是否存在可运行的目标Path。
 */
static int _linkg_switch_plan_validate_path(uint8_t peer_node_id, uint32_t link_id)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t          *link;
    linkg_path_t          *path;
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
 * @brief 校验当前Peer完整计划使用的全部实际发送Path。
 */
static int _linkg_switch_plan_validate_targets(uint8_t peer_node_id, const linkg_send_plan_t *plan)
{
    const linkg_class_send_plan_t *current;
    uint32_t                      class_index;
    int                           ret;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &plan->classes[class_index];
        if (current->mode == LINKG_SEND_MODE_NONE)
        {
            continue;
        }

        ret = _linkg_switch_plan_validate_path(peer_node_id, current->primary_link_id);
        if (ret != 0)
        {
            return ret;
        }

        if (current->mode == LINKG_SEND_MODE_REDUNDANT)
        {
            ret = _linkg_switch_plan_validate_path(peer_node_id, current->secondary_link_id);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    return 0;
}


/****************************** 同步事务辅助 ******************************/

/**
 * @brief 判断消息编号是否晚于当前编号，正确处理32位回绕。
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
 * @brief 分配非零Wire消息编号，调用方持有Switch锁。
 */
static uint32_t _linkg_switch_plan_allocate_message_id_locked(void)
{
    uint32_t message_id;

    message_id = ++g_switch.next_plan_message_id;
    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        message_id = ++g_switch.next_plan_message_id;
    }

    return message_id;
}

/**
 * @brief 分配稳定的非零逻辑同步编号，Peer重建后不复用。
 */
static uint32_t _linkg_switch_plan_allocate_sync_id_locked(void)
{
    uint32_t sync_id;

    sync_id = ++g_switch.next_plan_sync_id;
    if (sync_id == LINKG_SWITCH_PLAN_SYNC_ID_INVALID)
    {
        sync_id = ++g_switch.next_plan_sync_id;
    }

    return sync_id;
}

/**
 * @brief 当前是否仍存在进行中的协议同步事务。
 */
static bool _linkg_switch_plan_sync_busy(const linkg_switch_plan_sync_runtime_t *sync)
{
    return sync->state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
           sync->state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT;
}

/**
 * @brief 保存最近一次逻辑同步终态，通知由Worker从独立接口获取。
 *
 * 只保留最新结果；被取代的Operation必须同时校验sync_id，
 * 或在周期处理时发现当前sync_id不匹配后自行失效。
 * 调用方持有Switch锁。
 */
static void _linkg_switch_plan_finish_sync_locked(linkg_switch_peer_runtime_t *peer, linkg_switch_plan_sync_state_t state, int32_t status, uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t *sync;

    sync = &peer->role.sta.plan_sync;
    if (!_linkg_switch_plan_sync_busy(sync))
    {
        return;
    }

    sync->state         = state;
    sync->last_status   = status;
    sync->next_retry_us = 0U;
    sync->deadline_us   = 0U;

    sync->last_result.peer_node_id    = peer->peer_node_id;
    sync->last_result.peer_generation = peer->generation;
    sync->last_result.sync_id         = sync->sync_id;
    sync->last_result.message_id      = sync->message_id;
    sync->last_result.state           = state;
    sync->last_result.status          = status;
    sync->last_result.updated_us      = now_us;
    sync->result_pending              = true;

    // 与Switch事件队列相同，只唤醒唯一Worker，不调用业务Policy。
    if (g_switch.running)
    {
        (void)linkg_thread_wakeup(&g_switch.thread);
    }
}

/**
 * @brief 当前本地Plan覆盖旧Plan时停止旧协议事务。
 */
static void _linkg_switch_plan_cancel_sync_locked(linkg_switch_peer_runtime_t *peer, uint64_t now_us)
{
    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        return;
    }

    _linkg_switch_plan_finish_sync_locked(peer, LINKG_SWITCH_PLAN_SYNC_CANCELLED, -ECANCELED, now_us);
}

/**
 * @brief 创建一次全新的完整Plan同步，分配sync_id和message_id。
 */
static void _linkg_switch_plan_start_sync_locked(linkg_switch_peer_runtime_t *peer, const linkg_switch_wire_plan_sync_t *wire_plan, uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t *sync;

    sync                 = &peer->role.sta.plan_sync;
    sync->sync_id        = _linkg_switch_plan_allocate_sync_id_locked();
    sync->message_id     = _linkg_switch_plan_allocate_message_id_locked();
    sync->wire_plan      = *wire_plan;
    sync->state          = LINKG_SWITCH_PLAN_SYNC_WAIT_ACK;
    sync->last_status    = -EINPROGRESS;
    sync->retry_count    = 1U;
    sync->recovery_count = 0U;
    sync->next_retry_us  = now_us + LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US;
    sync->deadline_us    = now_us + LINKG_SWITCH_PLAN_SYNC_TIMEOUT_US;
}

/**
 * @brief 同一逻辑事务的新一轮协议恢复，仅更换Wire message_id。
 */
static void _linkg_switch_plan_restart_sync_locked(linkg_switch_plan_sync_runtime_t *sync, uint64_t now_us)
{
    sync->message_id  = _linkg_switch_plan_allocate_message_id_locked();
    sync->state       = LINKG_SWITCH_PLAN_SYNC_WAIT_ACK;
    sync->last_status = -EINPROGRESS;
    sync->retry_count = 1U;
    sync->recovery_count++;
    sync->next_retry_us = now_us + LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US;
    sync->deadline_us   = now_us + LINKG_SWITCH_PLAN_SYNC_TIMEOUT_US;
}

/**
 * @brief 协议轮次失败：有限恢复或发布最终结果，不修改本地Plan。
 */
static void _linkg_switch_plan_round_failed_locked(linkg_switch_peer_runtime_t *peer, int32_t status, bool remote_rejected, uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t *sync;

    sync = &peer->role.sta.plan_sync;
    if (sync->recovery_count >= LINKG_SWITCH_PLAN_SYNC_RECOVERY_MAX)
    {
        _linkg_switch_plan_finish_sync_locked(peer, remote_rejected ? LINKG_SWITCH_PLAN_SYNC_REJECTED : LINKG_SWITCH_PLAN_SYNC_UNCERTAIN, status, now_us);
        return;
    }

    sync->state         = LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT;
    sync->last_status   = status;
    sync->deadline_us   = 0U;
    sync->next_retry_us = now_us + (LINKG_SWITCH_PLAN_SYNC_RECOVERY_BASE_US << sync->recovery_count);
}

/**
 * @brief 判断离锁后仍可发送本次已创建的Plan消息。
 */
static bool _linkg_switch_plan_sync_message_current(uint8_t peer_node_id, uint32_t peer_generation, uint32_t sync_id, uint32_t message_id)
{
    linkg_switch_peer_runtime_t *peer;
    bool                        current;

    current = false;
    pthread_mutex_lock(&g_switch.lock);

    if (g_switch.initialized && g_switch.running && g_switch.role == LINKG_DEVICE_ROLE_STA)
    {
        peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
        if (peer != NULL)
        {
            current = peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK &&
                      peer->role.sta.plan_sync.sync_id == sync_id &&
                      peer->role.sta.plan_sync.message_id == message_id;
        }
    }

    pthread_mutex_unlock(&g_switch.lock);
    return current;
}

/**
 * @brief 发送已被接受的完整Plan，首包失败由协议Worker继续重试。
 */
static void _linkg_switch_plan_send_committed(uint8_t peer_node_id, uint32_t peer_generation, uint32_t sync_id, uint32_t message_id, const linkg_switch_wire_plan_sync_t *wire_plan, linkg_packet_pool_t *packet_pool)
{
    int ret;

    if (!_linkg_switch_plan_sync_message_current(peer_node_id, peer_generation, sync_id, message_id))
    {
        return;
    }

    ret = linkg_switch_tx_send_plan_sync(packet_pool, peer_node_id, message_id, wire_plan);
    if (ret != 0)
    {
        LINKG_LOG_WARN("SWITCH-PLAN: initial PLAN_SYNC send failed, peer=%u, sync=%u, error=%d", (unsigned int)peer_node_id, (unsigned int)sync_id, ret);
    }
}

/****************************** Discovery Peer计划管理 ******************************/

/**
 * @brief 供Discovery注册或更新直接Peer初始发送计划，不发起远端同步。
 */
int linkg_switch_plan_set(uint8_t peer_node_id, const linkg_send_plan_t *plan)
{
    linkg_switch_peer_runtime_t *peer;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) || !_linkg_switch_plan_valid(plan))
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
    else if (!_linkg_switch_plan_equal(&peer->plan, plan))
    {
        // 已有Peer的计划被外部重置，撤销旧同步事务。
        _linkg_switch_plan_cancel_sync_locked(peer, linkg_time_monotonic_us());
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


/****************************** 本地计划应用与STA提交 ******************************/

/**
 * @brief 仅在本机应用指定直接Peer发送计划，不触发远端计划同步。
 *
 * 用于Maintenance逃逸计划和Reconcile本地收敛；不承担STA到AP同步。
 * 任何完整Plan变化都会取消此前尚未结束的同步事务。
 * 其它模块通过sync_id检测已被替换的旧事务。
 * 不创建PLAN_SYNC事务，也不发送任何Switch控制消息。
 */
static int _linkg_switch_plan_apply_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan)
{
    linkg_switch_peer_runtime_t *peer;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) || peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID || !_linkg_switch_plan_valid(plan))
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

    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    if (previous != NULL && !_linkg_switch_plan_equal(&peer->plan, previous))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EAGAIN;
    }

    if (_linkg_switch_plan_access_blocked_locked(peer, plan))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EBUSY;
    }

    if (!_linkg_switch_plan_equal(&peer->plan, plan))
    {
        // 任何完整Plan变化都会使当前未完成同步失效；最新Plan优先。
        _linkg_switch_plan_cancel_sync_locked(peer, linkg_time_monotonic_us());
    }

    peer->plan = *plan;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/****************************** 本地Plan提交与远端同步 ******************************/

/**
 * @brief 通用完整Plan提交，不判断任何业务原因。
 *
 * 返回0表示本地Plan及其自动重试事务已建立，不表示远端ACK。
 * 每次提交生成全新sync_id；更新的提交覆盖未完成的旧同步。
 */
static int _linkg_switch_plan_commit_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, uint64_t now_us, uint32_t *out_sync_id)
{
    linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_peer_runtime_t      *peer;
    linkg_switch_wire_plan_sync_t    wire_plan;
    linkg_packet_pool_t              *packet_pool;
    uint32_t                         sync_id;
    uint32_t                         message_id;
    int                              ret;

    if (!_linkg_switch_plan_peer_id_valid(peer_node_id) ||
        peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID ||
        !_linkg_switch_plan_valid(plan) || now_us == 0U)
    {
        return -EINVAL;
    }

    if (out_sync_id != NULL)
    {
        *out_sync_id = LINKG_SWITCH_PLAN_SYNC_ID_INVALID;
    }

    ret = _linkg_switch_plan_local_to_wire(plan, &wire_plan);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_switch_plan_validate_targets(peer_node_id, plan);
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

    if (previous != NULL && !_linkg_switch_plan_equal(&peer->plan, previous))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EAGAIN;
    }

    if (_linkg_switch_plan_access_blocked_locked(peer, plan))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EBUSY;
    }

    packet_pool = g_switch.packet_pool;
    if (packet_pool == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    // 当前的任何旧同步都被新提交取代，绝不允许后续旧ACK推进新事务。
    _linkg_switch_plan_cancel_sync_locked(peer, now_us);
    peer->plan = *plan;

    _linkg_switch_plan_start_sync_locked(peer, &wire_plan, now_us);
    sync       = &peer->role.sta.plan_sync;
    sync_id    = sync->sync_id;
    message_id = sync->message_id;

    if (out_sync_id != NULL)
    {
        *out_sync_id = sync_id;
    }

    pthread_mutex_unlock(&g_switch.lock);

    _linkg_switch_plan_send_committed(peer_node_id, peer_generation, sync_id, message_id, &wire_plan, packet_pool);
    return 0;
}

/**
 * @brief 仅在本机整组应用计划，不发送PLAN_SYNC。
 */
int linkg_switch_plan_apply_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan)
{
    return _linkg_switch_plan_apply_local(peer_node_id, peer_generation, NULL, plan);
}

/**
 * @brief 本地整组立即生效，并异步推进PLAN_SYNC。
 */
int linkg_switch_plan_commit_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan, uint64_t now_us)
{
    return _linkg_switch_plan_commit_local(peer_node_id, peer_generation, NULL, plan, now_us, NULL);
}

/**
 * @brief 原计划仍匹配时整组更新，支持是否向AP同步。
 */
int linkg_switch_plan_update_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, bool synchronize, uint64_t now_us)
{
    if (previous == NULL)
    {
        return -EINVAL;
    }

    return synchronize ?
           _linkg_switch_plan_commit_local(peer_node_id, peer_generation, previous, plan, now_us, NULL) :
           _linkg_switch_plan_apply_local(peer_node_id, peer_generation, previous, plan);
}

/**
 * @brief 供Operation使用的通用条件提交，返回稳定逻辑sync_id。
 */
int linkg_switch_plan_submit_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, uint64_t now_us, uint32_t *sync_id)
{
    if (previous == NULL || sync_id == NULL)
    {
        return -EINVAL;
    }

    return _linkg_switch_plan_commit_local(peer_node_id, peer_generation, previous, plan, now_us, sync_id);
}

/****************************** 同步状态及最新结果查询 ******************************/

/**
 * @brief 获取单个STA Peer的最新PlanSync运行快照。
 */
int linkg_switch_plan_sync_get(uint8_t peer_node_id, uint32_t peer_generation, linkg_switch_plan_sync_runtime_t *snapshot)
{
    linkg_switch_peer_runtime_t *peer;

    if (snapshot == NULL || !_linkg_switch_plan_peer_id_valid(peer_node_id) ||
        peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));
    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
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

    *snapshot = peer->role.sta.plan_sync;
    pthread_mutex_unlock(&g_switch.lock);
    return 0;
}

/**
 * @brief Worker取最近一次尚未消费的同步终态结果。
 * @return 1有结果，0无结果，负数表示参数或运行状态错误。
 */
int linkg_switch_plan_take_result(linkg_switch_plan_sync_result_t *result)
{
    linkg_switch_peer_runtime_t *peer;
    uint32_t                    index;

    if (result == NULL)
    {
        return -EINVAL;
    }

    memset(result, 0, sizeof(*result));
    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];
        if (!peer->used || !peer->role.sta.plan_sync.result_pending)
        {
            continue;
        }

        *result                                 = peer->role.sta.plan_sync.last_result;
        peer->role.sta.plan_sync.result_pending = false;
        pthread_mutex_unlock(&g_switch.lock);
        return 1;
    }

    pthread_mutex_unlock(&g_switch.lock);
    return 0;
}

/****************************** AP接收完整计划 ******************************/

/**
 * @brief 处理AP收到的一条STA发送计划同步事件。
 */
static int _linkg_switch_plan_process_sync_event(const linkg_switch_event_t *event)
{
    linkg_switch_wire_plan_ack_t   ack;
    linkg_switch_ap_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t    *peer;
    linkg_packet_pool_t            *packet_pool;
    linkg_send_plan_t              plan;
    int32_t                        status;
    bool                           duplicate;
    bool                           stale;
    bool                           plan_converted;
    int                            ret;

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
        ret = _linkg_switch_plan_validate_targets(event->peer_node_id, &plan);
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

    if (status == 0 && _linkg_switch_plan_access_blocked_locked(peer, &plan))
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


/****************************** STA处理同步确认 ******************************/

/**
 * @brief 只完成协议ACK校验和同步状态迁移，不接触Policy业务。
 */
static int _linkg_switch_plan_process_ack_event(const linkg_switch_event_t *event, uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_peer_runtime_t      *peer;
    bool                             waiting_late_success;

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

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    sync = &peer->role.sta.plan_sync;

    // 仅接受当前同步事务的当前Message ID；新Plan覆盖后旧ACK必然失配。
    if (sync->sync_id == LINKG_SWITCH_PLAN_SYNC_ID_INVALID || sync->message_id != event->message_id)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    waiting_late_success = sync->state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT &&
                           event->payload.plan_ack.status == 0;

    if (sync->state != LINKG_SWITCH_PLAN_SYNC_WAIT_ACK && !waiting_late_success)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (event->payload.plan_ack.status == 0)
    {
        _linkg_switch_plan_finish_sync_locked(peer, LINKG_SWITCH_PLAN_SYNC_CONFIRMED, 0, now_us);
    }
    else
    {
        _linkg_switch_plan_round_failed_locked(peer, event->payload.plan_ack.status, true, now_us);
    }

    pthread_mutex_unlock(&g_switch.lock);
    return 0;
}

/****************************** Switch控制事件入口 ******************************/

/**
 * @brief 分发Plan协议事件，不做业务策略判断。
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
        return _linkg_switch_plan_process_ack_event(event, now_us);
    }

    return -EINVAL;
}

/****************************** Plan同步重试与Worker调度 ******************************/

/**
 * @brief 处理当前STA到期的协议重发、恢复或终态超时。
 */
int linkg_switch_plan_process(uint64_t now_us)
{
    linkg_switch_plan_sync_runtime_t sync_copy;
    linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_peer_runtime_t      *peer;
    linkg_packet_pool_t              *packet_pool;
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
        pthread_mutex_lock(&g_switch.lock);

        if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
        {
            ret = !g_switch.initialized ? -ENODEV : !g_switch.running ? -ESHUTDOWN : 0;
            pthread_mutex_unlock(&g_switch.lock);
            return ret;
        }

        peer = &g_switch.peers[index];
        if (!peer->used)
        {
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        sync = &peer->role.sta.plan_sync;
        if (!_linkg_switch_plan_sync_busy(sync))
        {
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        if (sync->state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK &&
            (now_us >= sync->deadline_us ||
             (sync->retry_count >= LINKG_SWITCH_PLAN_SYNC_RETRY_MAX && now_us >= sync->next_retry_us)))
        {
            _linkg_switch_plan_round_failed_locked(peer, -ETIMEDOUT, false, now_us);
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        if (sync->next_retry_us > now_us)
        {
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        if (sync->state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT)
        {
            _linkg_switch_plan_restart_sync_locked(sync, now_us);
        }
        else if (sync->state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK)
        {
            sync->retry_count++;
            sync->next_retry_us = now_us + LINKG_SWITCH_PLAN_SYNC_RETRY_INTERVAL_US;
        }

        sync_copy       = *sync;
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

        if (!_linkg_switch_plan_sync_message_current(peer_node_id, peer_generation, sync_copy.sync_id, sync_copy.message_id))
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
 * @brief 获取最近协议截止时间，调用方持有Switch锁。
 */
uint64_t linkg_switch_plan_next_deadline_locked(void)
{
    linkg_switch_plan_sync_runtime_t *sync;
    uint64_t                         deadline_us;
    uint32_t                         index;

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
        if (!_linkg_switch_plan_sync_busy(sync))
        {
            continue;
        }

        if (sync->next_retry_us < deadline_us)
        {
            deadline_us = sync->next_retry_us;
        }

        if (sync->state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK && sync->deadline_us < deadline_us)
        {
            deadline_us = sync->deadline_us;
        }
    }

    return deadline_us;
}
