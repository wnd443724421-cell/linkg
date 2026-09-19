/**
 * @file switch_maintenance.c
 * @brief LinkG链路切换接入维护实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-19
 */

#include "switch_maintenance.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "switch_internal.h"
#include "switch_plan.h"
#include "switch_tx.h"
#include "switch_wire.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_MAINTENANCE_END_RETRY_INTERVAL_US 250000ULL   // Maintenance END重发周期
#define LINKG_SWITCH_MAINTENANCE_END_TIMEOUT_US        5000000ULL  // Maintenance END最终确认等待时间
#define LINKG_SWITCH_MAINTENANCE_REMOTE_TIMEOUT_US     30000000ULL // 远端Maintenance最终保护超时时间
#define LINKG_SWITCH_MAINTENANCE_MESSAGE_ID_HALF_RANGE 0x80000000U // 32位事务编号前后关系半区间

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t           peer_node_id;    // 当前直接Peer节点编号
    uint32_t          peer_generation; // 当前Peer运行代际
    bool              apply_plan;      // 当前是否需要修改本地发送计划
    linkg_send_plan_t plan;            // Maintenance迁移后的本地发送计划
} linkg_switch_maintenance_peer_action_t;
/****************************** 内部辅助 ******************************/

/**
 * @brief 判断Maintenance支持的本地Access是否合法。
 */
static bool _linkg_switch_maintenance_access_valid(linkg_link_access_t access)
{
    return access == LINKG_LINK_ACCESS_WIFI ||
           access == LINKG_LINK_ACCESS_CELLULAR;
}

/**
 * @brief 判断Maintenance运行期间的Peer状态错误是否属于正常并发变化。
 */
static bool _linkg_switch_maintenance_expected_state_error(int error)
{
    return error == -ENOENT ||
           error == -ENODEV ||
           error == -ENETDOWN ||
           error == -EAGAIN ||
           error == -ESTALE ||
           error == -ESHUTDOWN;
}

/**
 * @brief 记录Maintenance处理中第一个非预期错误。
 */
static void _linkg_switch_maintenance_record_error(int error, int *first_error)
{
    if (first_error == NULL || error == 0)
    {
        return;
    }

    if (_linkg_switch_maintenance_expected_state_error(error))
    {
        return;
    }

    if (*first_error == 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 将本地Link Access转换为Maintenance Wire Access。
 */
static linkg_switch_wire_access_t _linkg_switch_maintenance_access_to_wire(linkg_link_access_t access)
{
    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        return LINKG_SWITCH_WIRE_ACCESS_WIFI;
    }

    if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        return LINKG_SWITCH_WIRE_ACCESS_CELLULAR;
    }

    return LINKG_SWITCH_WIRE_ACCESS_NONE;
}

/**
 * @brief 将Maintenance Wire Access转换为本地Link Access。
 */
static linkg_link_access_t _linkg_switch_maintenance_access_from_wire(linkg_switch_wire_access_t access)
{
    if (access == LINKG_SWITCH_WIRE_ACCESS_WIFI)
    {
        return LINKG_LINK_ACCESS_WIFI;
    }

    if (access == LINKG_SWITCH_WIRE_ACCESS_CELLULAR)
    {
        return LINKG_LINK_ACCESS_CELLULAR;
    }

    return LINKG_LINK_ACCESS_NONE;
}

/**
 * @brief 获取当前Maintenance Access对应的备用接入。
 */
static linkg_link_access_t _linkg_switch_maintenance_alternate_access(linkg_link_access_t access)
{
    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        return LINKG_LINK_ACCESS_CELLULAR;
    }

    if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        return LINKG_LINK_ACCESS_WIFI;
    }

    return LINKG_LINK_ACCESS_NONE;
}

/**
 * @brief 饱和增加微秒时间，避免极端情况下uint64_t回绕。
 */
static uint64_t _linkg_switch_maintenance_add_us(uint64_t now_us, uint64_t delta_us)
{
    if (UINT64_MAX - now_us < delta_us)
    {
        return UINT64_MAX;
    }

    return now_us + delta_us;
}

/**
 * @brief 判断Maintenance事务编号是否晚于当前已经处理的编号。
 */
static bool _linkg_switch_maintenance_message_id_newer(uint32_t message_id, uint32_t current_message_id)
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

    return delta != 0U &&
           delta < LINKG_SWITCH_MAINTENANCE_MESSAGE_ID_HALF_RANGE;
}

/**
 * @brief 分配新的非零本机Maintenance事务编号，调用方持有Switch锁。
 */
static uint32_t _linkg_switch_maintenance_allocate_message_id_locked(void)
{
    uint32_t message_id;

    message_id = ++g_switch.next_maintenance_message_id;

    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        message_id = ++g_switch.next_maintenance_message_id;
    }

    return message_id;
}

/**
 * @brief 判断当前是否仍存在等待END确认的本机Maintenance事务。
 */
static bool _linkg_switch_maintenance_end_pending_locked(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        if (g_switch.peers[index].maintenance.end_tx.active)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断指定Peer当前Access是否具备可用Path。
 *
 * 本接口只读取Switch已经缓存的状态，不主动查询Path模块。
 */
static bool _linkg_switch_maintenance_peer_access_available_locked(const linkg_switch_peer_runtime_t *peer, linkg_link_access_t access)
{
    if (peer == NULL)
    {
        return false;
    }

    if (g_switch.role == LINKG_DEVICE_ROLE_STA)
    {
        if (!peer->role.sta.observation.valid)
        {
            return false;
        }

        if (access == LINKG_LINK_ACCESS_WIFI)
        {
            return peer->role.sta.observation.wifi.available;
        }

        if (access == LINKG_LINK_ACCESS_CELLULAR)
        {
            return peer->role.sta.observation.cellular.available;
        }

        return false;
    }

    if (g_switch.role == LINKG_DEVICE_ROLE_AP)
    {
        if (access == LINKG_LINK_ACCESS_WIFI)
        {
            return peer->role.ap.wifi_path_available;
        }

        if (access == LINKG_LINK_ACCESS_CELLULAR)
        {
            return peer->role.ap.cellular_path_available;
        }
    }

    return false;
}

/**
 * @brief 判断当前发送计划是否实际使用指定Link。
 */
static bool _linkg_switch_maintenance_plan_uses_link(const linkg_send_plan_t *plan, uint32_t link_id)
{
    if (plan == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    if (plan->mode == LINKG_SEND_MODE_SINGLE ||
        plan->mode == LINKG_SEND_MODE_REDUNDANT)
    {
        return plan->primary_link_id == link_id ||
               plan->secondary_link_id == link_id;
    }

    return false;
}

/**
 * @brief 根据当前Maintenance约束构造指定Peer迁移到备用Access的本地发送计划。
 *
 * 只有当前计划实际使用被维护Access，并且备用Access可用且未被其它Maintenance
 * 禁止时才形成新的单链路发送计划。
 */
static bool _linkg_switch_maintenance_build_escape_plan_locked(const linkg_switch_peer_runtime_t *peer,
                                                                linkg_link_access_t blocked_access,
                                                                uint32_t blocked_link_id,
                                                                linkg_link_access_t alternate_access,
                                                                uint32_t alternate_link_id,
                                                                linkg_send_plan_t *plan)
{
    if (peer == NULL || plan == NULL)
    {
        return false;
    }

    if (!_linkg_switch_maintenance_access_valid(blocked_access) ||
        !_linkg_switch_maintenance_access_valid(alternate_access))
    {
        return false;
    }

    if (blocked_link_id == LINKG_LINK_ID_INVALID ||
        alternate_link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    if (!_linkg_switch_maintenance_plan_uses_link(&peer->plan, blocked_link_id))
    {
        return false;
    }

    if (!_linkg_switch_maintenance_peer_access_available_locked(peer, alternate_access))
    {
        return false;
    }

    if (linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, alternate_access))
    {
        return false;
    }

    memset(plan, 0, sizeof(*plan));

    plan->mode              = LINKG_SEND_MODE_SINGLE;
    plan->primary_link_id   = alternate_link_id;
    plan->secondary_link_id = LINKG_LINK_ID_INVALID;

    return true;
}

/**
 * @brief 向指定直接Peer发送Maintenance BEGIN或END通知。
 */
static int _linkg_switch_maintenance_send(linkg_packet_pool_t *packet_pool,
                                           uint8_t peer_node_id,
                                           uint32_t message_id,
                                           linkg_link_access_t access,
                                           linkg_switch_wire_maintenance_phase_t phase)
{
    linkg_switch_wire_maintenance_t maintenance;

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    memset(&maintenance, 0, sizeof(maintenance));

    maintenance.access = _linkg_switch_maintenance_access_to_wire(access);
    maintenance.phase  = phase;

    if (maintenance.access == LINKG_SWITCH_WIRE_ACCESS_NONE)
    {
        return -EINVAL;
    }

    return linkg_switch_tx_send_maintenance(packet_pool,
                                             peer_node_id,
                                             message_id,
                                             &maintenance);
}

/**
 * @brief 向指定直接Peer发送Maintenance END确认。
 */
static int _linkg_switch_maintenance_send_ack(linkg_packet_pool_t *packet_pool,
                                               uint8_t peer_node_id,
                                               uint32_t message_id,
                                               linkg_link_access_t access)
{
    linkg_switch_wire_maintenance_ack_t ack;

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    memset(&ack, 0, sizeof(ack));

    ack.access = _linkg_switch_maintenance_access_to_wire(access);
    if (ack.access == LINKG_SWITCH_WIRE_ACCESS_NONE)
    {
        return -EINVAL;
    }

    return linkg_switch_tx_send_maintenance_ack(packet_pool,
                                                 peer_node_id,
                                                 message_id,
                                                 &ack);
}

/**
 * @brief 处理对端Maintenance BEGIN事件。
 */
static int _linkg_switch_maintenance_process_begin(const linkg_switch_event_t *event, uint64_t now_us)
{
    linkg_switch_maintenance_remote_runtime_t *remote;
    linkg_switch_peer_runtime_t               *peer;
    linkg_send_plan_t                          plan;
    linkg_link_access_t                        access;
    linkg_link_access_t                        alternate_access;
    uint32_t                                   blocked_link_id;
    uint32_t                                   alternate_link_id;
    bool                                       apply_plan;
    int                                        ret;

    access = _linkg_switch_maintenance_access_from_wire(event->payload.maintenance.access);
    if (!_linkg_switch_maintenance_access_valid(access))
    {
        return -EINVAL;
    }

    alternate_access  = _linkg_switch_maintenance_alternate_access(access);
    blocked_link_id   = linkg_link_manager_get_id(access);
    alternate_link_id = linkg_link_manager_get_id(alternate_access);

    memset(&plan, 0, sizeof(plan));
    apply_plan = false;

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

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    remote = &peer->maintenance.remote;

    if (event->message_id == remote->last_message_id)
    {
        if (remote->active && remote->access != access)
        {
            pthread_mutex_unlock(&g_switch.lock);
            return -EPROTO;
        }

        // 同一事务BEGIN重复到达或该事务已经结束，均不重复执行副作用。
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (!_linkg_switch_maintenance_message_id_newer(event->message_id, remote->last_message_id))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    remote->active          = true;
    remote->access          = access;
    remote->last_message_id = event->message_id;
    remote->started_us      = now_us;
    remote->expires_us      = _linkg_switch_maintenance_add_us(now_us, LINKG_SWITCH_MAINTENANCE_REMOTE_TIMEOUT_US);

    apply_plan = _linkg_switch_maintenance_build_escape_plan_locked(peer,
                                                                    access,
                                                                    blocked_link_id,
                                                                    alternate_access,
                                                                    alternate_link_id,
                                                                    &plan);

    pthread_mutex_unlock(&g_switch.lock);

    LINKG_LOG_DEBUG("SWITCH-MAINT: BEGIN received, peer=%u id=%u access=%d escape_plan=%d",
                    (unsigned int)event->peer_node_id,
                    (unsigned int)event->message_id,
                    (int)access,
                    apply_plan ? 1 : 0);

    if (!apply_plan)
    {
        return 0;
    }

    ret = linkg_switch_plan_apply_local(event->peer_node_id, event->peer_generation, &plan);
    if (_linkg_switch_maintenance_expected_state_error(ret))
    {
        return 0;
    }

    return ret;
}


/**
 * @brief 处理对端Maintenance END事件并返回对应确认。
 */
static int _linkg_switch_maintenance_process_end(const linkg_switch_event_t *event, uint64_t now_us)
{
    linkg_switch_maintenance_remote_runtime_t *remote;
    linkg_packet_pool_t                       *packet_pool;
    linkg_switch_peer_runtime_t               *peer;
    linkg_link_access_t                        access;
    bool                                       send_ack;

    (void)now_us;

    access = _linkg_switch_maintenance_access_from_wire(event->payload.maintenance.access);
    if (!_linkg_switch_maintenance_access_valid(access))
    {
        return -EINVAL;
    }

    packet_pool = NULL;
    send_ack    = false;

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

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    remote = &peer->maintenance.remote;

    if (event->message_id == remote->last_message_id)
    {
        if (remote->active && remote->access != access)
        {
            pthread_mutex_unlock(&g_switch.lock);
            return -EPROTO;
        }

        remote->active     = false;
        remote->access     = LINKG_LINK_ACCESS_NONE;
        remote->started_us = 0U;
        remote->expires_us = 0U;

        packet_pool = g_switch.packet_pool;
        send_ack    = true;

        pthread_mutex_unlock(&g_switch.lock);

        LINKG_LOG_DEBUG("SWITCH-MAINT: END received, peer=%u id=%u access=%d duplicate_or_current=1",
                        (unsigned int)event->peer_node_id,
                        (unsigned int)event->message_id,
                        (int)access);

        if (!linkg_switch_peer_generation_current(event->peer_node_id, event->peer_generation))
        {
            return -ESTALE;
        }

        return _linkg_switch_maintenance_send_ack(packet_pool,
                                                   event->peer_node_id,
                                                   event->message_id,
                                                   access);
    }

    if (_linkg_switch_maintenance_message_id_newer(event->message_id, remote->last_message_id))
    {
        /**
         * BEGIN可能丢失而END先到。
         * 仍然记录该事务已经结束，避免迟到BEGIN再次激活Maintenance。
         */
        remote->last_message_id = event->message_id;
        remote->active          = false;
        remote->access          = LINKG_LINK_ACCESS_NONE;
        remote->started_us      = 0U;
        remote->expires_us      = 0U;

        packet_pool = g_switch.packet_pool;
        send_ack    = true;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (!send_ack)
    {
        return 0;
    }

    LINKG_LOG_DEBUG("SWITCH-MAINT: END received, peer=%u id=%u access=%d begin_missing=1",
                    (unsigned int)event->peer_node_id,
                    (unsigned int)event->message_id,
                    (int)access);

    if (!linkg_switch_peer_generation_current(event->peer_node_id, event->peer_generation))
    {
        return -ESTALE;
    }

    return _linkg_switch_maintenance_send_ack(packet_pool,
                                               event->peer_node_id,
                                               event->message_id,
                                               access);
}


/**
 * @brief 处理对端Maintenance END确认事件。
 */
static int _linkg_switch_maintenance_process_ack(const linkg_switch_event_t *event)
{
    linkg_switch_maintenance_end_tx_runtime_t *end_tx;
    linkg_switch_peer_runtime_t               *peer;
    linkg_link_access_t                        access;

    access = _linkg_switch_maintenance_access_from_wire(event->payload.maintenance_ack.access);
    if (!_linkg_switch_maintenance_access_valid(access))
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

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    end_tx = &peer->maintenance.end_tx;

    if (!end_tx->active)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (end_tx->message_id != event->message_id)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (end_tx->access != access)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPROTO;
    }

    memset(end_tx, 0, sizeof(*end_tx));

    pthread_mutex_unlock(&g_switch.lock);

    LINKG_LOG_DEBUG("SWITCH-MAINT: END ACK complete, peer=%u id=%u access=%d",
                    (unsigned int)event->peer_node_id,
                    (unsigned int)event->message_id,
                    (int)access);

    return 0;
}


/****************************** 使用约束 ******************************/

/**
 * @brief 判断指定Peer当前Access是否被本机或对端Maintenance禁止使用。
 *
 * 调用方必须持有g_switch.lock。
 */
bool linkg_switch_maintenance_access_blocked_locked(const linkg_switch_maintenance_peer_runtime_t *runtime, linkg_link_access_t access)
{
    if (!_linkg_switch_maintenance_access_valid(access))
    {
        return false;
    }

    if (g_switch.local_maintenance.active &&
        g_switch.local_maintenance.access == access)
    {
        return true;
    }

    if (runtime != NULL &&
        runtime->remote.active &&
        runtime->remote.access == access)
    {
        return true;
    }

    return false;
}

/****************************** 本机维护控制 ******************************/

/**
 * @brief 开始本机指定Access维护并通知全部当前直接Peer。
 */
int linkg_switch_maintenance_begin(linkg_link_access_t access)
{
    linkg_switch_maintenance_peer_action_t actions[LINKG_SWITCH_PEER_MAX];
    linkg_switch_wire_maintenance_phase_t  phase;
    linkg_packet_pool_t                   *packet_pool;
    linkg_switch_peer_runtime_t           *peer;
    linkg_link_access_t                    alternate_access;
    uint32_t                               blocked_link_id;
    uint32_t                               alternate_link_id;
    uint32_t                               message_id;
    uint32_t                               action_count;
    uint32_t                               index;
    uint64_t                               now_us;
    int                                    ret;

    if (!_linkg_switch_maintenance_access_valid(access))
    {
        return -EINVAL;
    }

    now_us = linkg_time_monotonic_us();
    if (now_us == 0U)
    {
        return -EIO;
    }

    alternate_access  = _linkg_switch_maintenance_alternate_access(access);
    blocked_link_id   = linkg_link_manager_get_id(access);
    alternate_link_id = linkg_link_manager_get_id(alternate_access);

    memset(actions, 0, sizeof(actions));

    packet_pool  = NULL;
    message_id   = LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID;
    action_count = 0U;
    phase        = LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN;

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

    if (g_switch.local_maintenance.active)
    {
        ret = g_switch.local_maintenance.access == access ? -EALREADY : -EBUSY;

        pthread_mutex_unlock(&g_switch.lock);

        return ret;
    }

    if (_linkg_switch_maintenance_end_pending_locked())
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

    message_id = _linkg_switch_maintenance_allocate_message_id_locked();

    g_switch.local_maintenance.active     = true;
    g_switch.local_maintenance.access     = access;
    g_switch.local_maintenance.message_id = message_id;
    g_switch.local_maintenance.started_us = now_us;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        actions[action_count].peer_node_id    = peer->peer_node_id;
        actions[action_count].peer_generation = peer->generation;

        actions[action_count].apply_plan =
            _linkg_switch_maintenance_build_escape_plan_locked(peer,
                                                               access,
                                                               blocked_link_id,
                                                               alternate_access,
                                                               alternate_link_id,
                                                               &actions[action_count].plan);

        action_count++;
    }

    pthread_mutex_unlock(&g_switch.lock);

    LINKG_LOG_DEBUG("SWITCH-MAINT: local BEGIN, id=%u access=%d peers=%u",
                    (unsigned int)message_id,
                    (int)access,
                    (unsigned int)action_count);

    /**
     * Maintenance是强制配置流程。
     * 单个Peer计划迁移或BEGIN发送失败不回滚本机Maintenance状态，
     * 对端后续仍可依赖链路失效和Policy完成保底迁移。
     */
    for (index = 0U; index < action_count; index++)
    {
        if (actions[index].apply_plan)
        {
            ret = linkg_switch_plan_apply_local(actions[index].peer_node_id,
                                                 actions[index].peer_generation,
                                                 &actions[index].plan);
            if (ret != 0 && !_linkg_switch_maintenance_expected_state_error(ret))
            {
                LINKG_LOG_WARN("SWITCH-MAINT: apply local escape plan failed, peer=%u, error=%d",
                               (unsigned int)actions[index].peer_node_id,
                               ret);
            }
        }

        if (!linkg_switch_peer_generation_current(actions[index].peer_node_id, actions[index].peer_generation))
        {
            continue;
        }

        ret = _linkg_switch_maintenance_send(packet_pool,
                                              actions[index].peer_node_id,
                                              message_id,
                                              access,
                                              phase);
        if (ret != 0 &&
            !_linkg_switch_maintenance_expected_state_error(ret))
        {
            LINKG_LOG_WARN("SWITCH-MAINT: send BEGIN failed, peer=%u, id=%u, error=%d",
                           (unsigned int)actions[index].peer_node_id,
                           (unsigned int)message_id,
                           ret);
        }
    }

    return 0;
}


/**
 * @brief 结束本机指定Access维护并启动全部当前直接Peer的END确认流程。
 */
int linkg_switch_maintenance_end(linkg_link_access_t access)
{
    linkg_packet_pool_t             *packet_pool;
    linkg_switch_peer_runtime_t     *peer;
    uint8_t                          peer_node_ids[LINKG_SWITCH_PEER_MAX];
    uint32_t                         peer_generations[LINKG_SWITCH_PEER_MAX];
    uint32_t                         message_id;
    uint32_t                         peer_count;
    uint32_t                         index;
    uint64_t                         now_us;
    int                              ret;

    if (!_linkg_switch_maintenance_access_valid(access))
    {
        return -EINVAL;
    }

    now_us = linkg_time_monotonic_us();
    if (now_us == 0U)
    {
        return -EIO;
    }

    memset(peer_node_ids, 0, sizeof(peer_node_ids));
    memset(peer_generations, 0, sizeof(peer_generations));

    packet_pool = NULL;
    message_id  = LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID;
    peer_count  = 0U;

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

    if (!g_switch.local_maintenance.active)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    if (g_switch.local_maintenance.access != access)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EINVAL;
    }

    packet_pool = g_switch.packet_pool;
    if (packet_pool == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    message_id = g_switch.local_maintenance.message_id;

    memset(&g_switch.local_maintenance, 0, sizeof(g_switch.local_maintenance));

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        memset(&peer->maintenance.end_tx, 0, sizeof(peer->maintenance.end_tx));

        peer->maintenance.end_tx.active        = true;
        peer->maintenance.end_tx.access        = access;
        peer->maintenance.end_tx.message_id    = message_id;
        peer->maintenance.end_tx.retry_count   = 1U;
        peer->maintenance.end_tx.next_retry_us = _linkg_switch_maintenance_add_us(now_us, LINKG_SWITCH_MAINTENANCE_END_RETRY_INTERVAL_US);
        peer->maintenance.end_tx.deadline_us   = _linkg_switch_maintenance_add_us(now_us, LINKG_SWITCH_MAINTENANCE_END_TIMEOUT_US);

        peer_node_ids[peer_count]   = peer->peer_node_id;
        peer_generations[peer_count] = peer->generation;
        peer_count++;
    }

    pthread_mutex_unlock(&g_switch.lock);

    LINKG_LOG_DEBUG("SWITCH-MAINT: local END, id=%u access=%d peers=%u",
                    (unsigned int)message_id,
                    (int)access,
                    (unsigned int)peer_count);

    /**
     * END立即发送一次。
     * 即使当前Peer Path尚未恢复导致发送失败，end_tx仍保持active，
     * Worker会在后续周期继续发送直到ACK或最终超时。
     */
    for (index = 0U; index < peer_count; index++)
    {
        if (!linkg_switch_peer_generation_current(peer_node_ids[index], peer_generations[index]))
        {
            continue;
        }

        ret = _linkg_switch_maintenance_send(packet_pool,
                                             peer_node_ids[index],
                                             message_id,
                                             access,
                                             LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_END);
        if (ret != 0 &&
            !_linkg_switch_maintenance_expected_state_error(ret))
        {
            LINKG_LOG_WARN("SWITCH-MAINT: send END failed, peer=%u, id=%u, error=%d",
                           (unsigned int)peer_node_ids[index],
                           (unsigned int)message_id,
                           ret);
        }
    }

    /**
     * maintenance_end可能由非Switch Worker线程调用。
     * 主动唤醒Switch Worker，使新建立的END retry deadline立即参与poll调度。
     */
    ret = linkg_thread_wakeup(&g_switch.thread);
    if (ret != 0)
    {
        LINKG_LOG_WARN("SWITCH-MAINT: wake worker after END failed, error=%d", ret);
    }

    return 0;
}

/****************************** 事件处理 ******************************/

/**
 * @brief 处理Worker收到的Maintenance控制事件。
 */
int linkg_switch_maintenance_process_event(const linkg_switch_event_t *event, uint64_t now_us)
{
    if (event == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    switch (event->type)
    {
        case LINKG_SWITCH_EVENT_MAINTENANCE_RX:
        {
            if (event->payload.maintenance.phase == LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN)
            {
                return _linkg_switch_maintenance_process_begin(event, now_us);
            }

            if (event->payload.maintenance.phase == LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_END)
            {
                return _linkg_switch_maintenance_process_end(event, now_us);
            }

            return -EINVAL;
        }

        case LINKG_SWITCH_EVENT_MAINTENANCE_ACK_RX:
        {
            return _linkg_switch_maintenance_process_ack(event);
        }

        default:
        {
            return -EPROTONOSUPPORT;
        }
    }
}

/****************************** 周期处理 ******************************/

/**
 * @brief 处理Maintenance远端保护超时和END重试事务。
 */
int linkg_switch_maintenance_process(uint64_t now_us)
{
    linkg_switch_maintenance_remote_runtime_t *remote;
    linkg_switch_maintenance_end_tx_runtime_t *end_tx;
    linkg_switch_peer_runtime_t               *peer;
    linkg_packet_pool_t                       *packet_pool;
    linkg_link_access_t                        retry_access;
    linkg_link_access_t                        timeout_access;
    linkg_link_access_t                        remote_timeout_access;
    uint32_t                                   retry_message_id;
    uint32_t                                   retry_count;
    uint32_t                                   timeout_message_id;
    uint32_t                                   timeout_retry_count;
    uint32_t                                   remote_timeout_message_id;
    uint32_t                                   peer_generation;
    uint32_t                                   index;
    uint8_t                                    peer_node_id;
    bool                                       send_retry;
    bool                                       remote_timeout;
    bool                                       end_timeout;
    int                                        first_error;
    int                                        ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    first_error = 0;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        packet_pool               = NULL;
        retry_access              = LINKG_LINK_ACCESS_NONE;
        timeout_access            = LINKG_LINK_ACCESS_NONE;
        remote_timeout_access     = LINKG_LINK_ACCESS_NONE;
        retry_message_id          = LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID;
        retry_count               = 0U;
        timeout_message_id        = LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID;
        timeout_retry_count       = 0U;
        remote_timeout_message_id = LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID;
        peer_generation           = LINKG_SWITCH_PEER_GENERATION_INVALID;
        peer_node_id              = 0U;
        send_retry                = false;
        remote_timeout            = false;
        end_timeout               = false;

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

        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            pthread_mutex_unlock(&g_switch.lock);
            continue;
        }

        peer_generation = peer->generation;
        peer_node_id    = peer->peer_node_id;
        remote          = &peer->maintenance.remote;
        end_tx       = &peer->maintenance.end_tx;

        if (remote->active &&
            remote->expires_us != 0U &&
            now_us >= remote->expires_us)
        {
            remote_timeout_access       = remote->access;
            remote_timeout_message_id   = remote->last_message_id;
            remote->active              = false;
            remote->access              = LINKG_LINK_ACCESS_NONE;
            remote->started_us          = 0U;
            remote->expires_us          = 0U;

            // last_message_id必须保留，用于过滤迟到的旧BEGIN。
            remote_timeout = true;
        }

        if (end_tx->active)
        {
            if (end_tx->deadline_us != 0U &&
                now_us >= end_tx->deadline_us)
            {
                timeout_access      = end_tx->access;
                timeout_message_id  = end_tx->message_id;
                timeout_retry_count = end_tx->retry_count;
                memset(end_tx, 0, sizeof(*end_tx));
                end_timeout = true;
            }
            else if (end_tx->next_retry_us != 0U &&
                     now_us >= end_tx->next_retry_us)
            {
                if (end_tx->retry_count != UINT32_MAX)
                {
                    end_tx->retry_count++;
                }

                retry_access     = end_tx->access;
                retry_message_id = end_tx->message_id;
                retry_count      = end_tx->retry_count;
                end_tx->next_retry_us = _linkg_switch_maintenance_add_us(now_us, LINKG_SWITCH_MAINTENANCE_END_RETRY_INTERVAL_US);

                packet_pool = g_switch.packet_pool;
                send_retry  = true;
            }
        }

        pthread_mutex_unlock(&g_switch.lock);

        if (remote_timeout)
        {
            LINKG_LOG_WARN("SWITCH-MAINT: remote maintenance timeout, peer=%u id=%u access=%d",
                           (unsigned int)peer_node_id,
                           (unsigned int)remote_timeout_message_id,
                           (int)remote_timeout_access);
        }

        if (end_timeout)
        {
            LINKG_LOG_WARN("SWITCH-MAINT: END ack timeout, peer=%u id=%u access=%d retries=%u",
                           (unsigned int)peer_node_id,
                           (unsigned int)timeout_message_id,
                           (int)timeout_access,
                           (unsigned int)timeout_retry_count);
        }

        if (!send_retry)
        {
            continue;
        }

        if (packet_pool == NULL)
        {
            _linkg_switch_maintenance_record_error(-ENODEV, &first_error);
            continue;
        }

        if (!linkg_switch_peer_generation_current(peer_node_id, peer_generation))
        {
            continue;
        }

        LINKG_LOG_DEBUG("SWITCH-MAINT: END retry, peer=%u id=%u access=%d retry=%u",
                        (unsigned int)peer_node_id,
                        (unsigned int)retry_message_id,
                        (int)retry_access,
                        (unsigned int)retry_count);

        ret = _linkg_switch_maintenance_send(packet_pool,
                                              peer_node_id,
                                              retry_message_id,
                                              retry_access,
                                              LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_END);

        _linkg_switch_maintenance_record_error(ret, &first_error);
    }

    return first_error;
}


/**
 * @brief 获取下一次Maintenance定时处理截止时间，调用方持有Switch锁。
 */
uint64_t linkg_switch_maintenance_next_deadline_locked(void)
{
    const linkg_switch_maintenance_remote_runtime_t *remote;
    const linkg_switch_maintenance_end_tx_runtime_t *end_tx;
    const linkg_switch_peer_runtime_t               *peer;
    uint64_t                                         deadline_us;
    uint32_t                                         index;

    if (!g_switch.initialized || !g_switch.running)
    {
        return UINT64_MAX;
    }

    deadline_us = UINT64_MAX;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        remote = &peer->maintenance.remote;
        end_tx = &peer->maintenance.end_tx;

        if (remote->active &&
            remote->expires_us != 0U &&
            remote->expires_us < deadline_us)
        {
            deadline_us = remote->expires_us;
        }

        if (!end_tx->active)
        {
            continue;
        }

        if (end_tx->next_retry_us != 0U &&
            end_tx->next_retry_us < deadline_us)
        {
            deadline_us = end_tx->next_retry_us;
        }

        if (end_tx->deadline_us != 0U &&
            end_tx->deadline_us < deadline_us)
        {
            deadline_us = end_tx->deadline_us;
        }
    }

    return deadline_us;
}
