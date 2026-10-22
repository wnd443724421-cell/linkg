/**
 * @file switch_policy_operation.c
 * @brief LinkG三类业务切换事务、PlanSync结果及质量验证推进
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-22
 */

#include "switch_policy_operation.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_time.h"

#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_policy_verify.h"

/****************************** 事务参数 ******************************/

#define LINKG_SWITCH_OPERATION_LOAD_DURATION_US         1000000ULL  // LOAD冗余ACK后1秒迁移
#define LINKG_SWITCH_OPERATION_RECOVERY_HOLDOFF_US     10000000ULL  // 迁出Wi-Fi后最少等待10秒
#define LINKG_SWITCH_OPERATION_FAILED_HOLDOFF_US       15000000ULL  // 恢复失败后延迟15秒重试
#define LINKG_SWITCH_OPERATION_RESYNC_INTERVAL_US       2000000ULL  // 协议终态失败后重同步间隔
#define LINKG_SWITCH_OPERATION_RESYNC_MAX                     2U    // 整组事务追加重同步次数

/****************************** 基础辅助 ******************************/

/**
 * @brief 饱和计算未来单调时刻。
 */
static uint64_t _linkg_switch_operation_add_us(uint64_t now_us, uint64_t interval_us)
{
    return now_us > UINT64_MAX - interval_us ? UINT64_MAX : now_us + interval_us;
}

/**
 * @brief 生成Peer内部非零事务编号；只能持Switch锁调用。
 */
static uint32_t _linkg_switch_operation_allocate_id_locked(linkg_switch_policy_runtime_t *runtime)
{
    uint32_t id;

    id = ++runtime->next_operation_id;
    if (id == LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
    {
        id = ++runtime->next_operation_id;
    }

    return id;
}

/**
 * @brief 检查两份完整发送计划是否相等。
 */
static bool _linkg_switch_operation_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if (left->classes[index].mode != right->classes[index].mode ||
            left->classes[index].primary_link_id != right->classes[index].primary_link_id ||
            left->classes[index].secondary_link_id != right->classes[index].secondary_link_id)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 检查指定路径已确认可用且未被Maintenance阻止。
 */
static bool _linkg_switch_operation_cellular_usable_locked(const linkg_switch_peer_runtime_t *peer)
{
    return peer->role.sta.observation.cellular.state == LINKG_SWITCH_OBSERVATION_USABLE &&
           peer->role.sta.observation.cellular.link_id != LINKG_LINK_ID_INVALID &&
           !linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_CELLULAR);
}

/**
 * @brief 检查当前Wi-Fi候选可用且未被Maintenance阻止。
 */
static bool _linkg_switch_operation_wifi_usable_locked(const linkg_switch_peer_runtime_t *peer)
{
    return peer->role.sta.observation.wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
           peer->role.sta.observation.wifi.link_id != LINKG_LINK_ID_INVALID &&
           !linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_WIFI);
}

/**
 * @brief 释放组级暂态但保留各业务迁移历史与Peer事务编号。
 *
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_operation_clear_group_locked(linkg_switch_peer_runtime_t *peer)
{
    linkg_switch_policy_runtime_t   *policy;
    linkg_switch_policy_operation_t *operation;
    uint32_t                         index;

    policy = &peer->role.sta.policy;
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        operation = &policy->operations[index];
        operation->phase            = LINKG_SWITCH_POLICY_PHASE_IDLE;
        operation->operation_id     = 0U;
        operation->reason           = LINKG_SWITCH_POLICY_REASON_NONE;
        operation->exit_target      = LINKG_SWITCH_POLICY_TARGET_NONE;
        operation->phase_entered_us = 0U;
        operation->started_us       = 0U;
        operation->deadline_us      = 0U;
    }

    policy->group_mask       = 0U;
    policy->group_reason     = LINKG_SWITCH_POLICY_REASON_NONE;
    policy->group_phase      = LINKG_SWITCH_POLICY_PHASE_IDLE;
    policy->group_target     = LINKG_SWITCH_POLICY_TARGET_NONE;
    policy->plan_sync_id     = 0U;
    policy->next_action_us   = 0U;
    policy->recovery_count   = 0U;
    policy->group_rssi_valid     = false;
    policy->group_wifi_link_id   = LINKG_LINK_ID_INVALID;
    policy->group_cellular_link_id = LINKG_LINK_ID_INVALID;
    linkg_switch_policy_verify_reset(&peer->role.sta.verify);
}

/**
 * @brief 设置组内所有业务的相同事务阶段。
 */
static void _linkg_switch_operation_set_phase_locked(linkg_switch_peer_runtime_t *peer, linkg_switch_policy_phase_t phase, uint64_t now_us)
{
    linkg_switch_policy_runtime_t *policy;
    uint32_t                       index;

    policy              = &peer->role.sta.policy;
    policy->group_phase = phase;
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        policy->operations[index].phase            = phase;
        policy->operations[index].phase_entered_us = now_us;
    }
}

/**
 * @brief 复核当前Plan与组内事务阶段相匹配，防止覆盖Reconcile或Maintenance修改。
 */
static bool _linkg_switch_operation_group_plan_matches_locked(const linkg_switch_peer_runtime_t *peer, bool exit_plan)
{
    const linkg_switch_policy_runtime_t *policy;
    const linkg_class_send_plan_t       *item;
    uint32_t                             expected_primary;
    uint32_t                             expected_secondary;
    uint32_t                             index;

    policy = &peer->role.sta.policy;
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        item = &peer->plan.classes[index];
        if (exit_plan)
        {
            expected_primary = policy->group_target == LINKG_SWITCH_POLICY_TARGET_CELLULAR ?
                               policy->group_cellular_link_id : policy->group_wifi_link_id;
            if (item->mode != LINKG_SEND_MODE_SINGLE || item->primary_link_id != expected_primary)
            {
                return false;
            }
        }
        else
        {
            expected_primary = policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY ?
                               policy->group_cellular_link_id : policy->group_wifi_link_id;
            expected_secondary = policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY ?
                                 policy->group_wifi_link_id : policy->group_cellular_link_id;
            if (item->mode != LINKG_SEND_MODE_REDUNDANT ||
                item->primary_link_id != expected_primary ||
                item->secondary_link_id != expected_secondary)
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief 根据当前Plan挑选业务并建立目标完整Plan；不分配事务。
 */
static uint32_t _linkg_switch_operation_build_enter_locked(const linkg_switch_peer_runtime_t *peer,
                                                           const linkg_switch_policy_event_t *event,
                                                           linkg_send_plan_t *next)
{
    const linkg_switch_observation_t *observation;
    linkg_class_send_plan_t          *item;
    uint32_t                          mask;
    uint32_t                          index;
    uint32_t                          bit;
    bool                              wifi_usable;
    bool                              cellular_usable;

    observation       = &peer->role.sta.observation;
    wifi_usable       = _linkg_switch_operation_wifi_usable_locked(peer);
    cellular_usable   = _linkg_switch_operation_cellular_usable_locked(peer);
    mask              = 0U;
    *next             = peer->plan;

    if (!cellular_usable)
    {
        return 0U;
    }

    // LOAD按照DATA、VIDEO、REALTIME优先顺序，只迁移一类业务。
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        uint32_t class_index = event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD ?
                               LINKG_TRANSPORT_CLASS_COUNT - 1U - index : index;
        bit  = LINKG_SWITCH_POLICY_CLASS_BIT(class_index);
        item = &next->classes[class_index];

        if (event->type == LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY)
        {
            if (!wifi_usable ||
                (event->payload.recovery_ready.class_mask & bit) == 0U ||
                item->mode != LINKG_SEND_MODE_SINGLE ||
                item->primary_link_id != observation->cellular.link_id ||
                item->secondary_link_id != observation->wifi.link_id)
            {
                continue;
            }
        }
        else if (event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN)
        {
            // Wi-Fi明确失效时，连Cellular为主的REDUNDANT也必须退出冗余。
            if (item->mode == LINKG_SEND_MODE_REDUNDANT &&
                item->primary_link_id == observation->cellular.link_id &&
                item->secondary_link_id == observation->wifi.link_id)
            {
                item->mode              = LINKG_SEND_MODE_SINGLE;
                item->secondary_link_id = LINKG_LINK_ID_INVALID;
                mask |= bit;
                continue;
            }

            if (item->mode == LINKG_SEND_MODE_NONE ||
                item->primary_link_id != observation->wifi.link_id ||
                item->secondary_link_id != observation->cellular.link_id)
            {
                continue;
            }

            item->mode              = LINKG_SEND_MODE_SINGLE;
            item->primary_link_id   = observation->cellular.link_id;
            item->secondary_link_id = LINKG_LINK_ID_INVALID;
            mask |= bit;
            continue;
        }
        else if (!wifi_usable || item->mode != LINKG_SEND_MODE_SINGLE ||
                 item->primary_link_id != observation->wifi.link_id ||
                 item->secondary_link_id != observation->cellular.link_id)
        {
            continue;
        }

        item->mode = LINKG_SEND_MODE_REDUNDANT;
        mask |= bit;
        if (event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD)
        {
            break;
        }
    }

    return mask;
}

/**
 * @brief 记录创建成功的组级事务；调用方持有Switch锁。
 */
static void _linkg_switch_operation_start_locked(linkg_switch_peer_runtime_t *peer, const linkg_switch_policy_event_t *event,
                                                  uint32_t mask, uint32_t sync_id, uint64_t now_us)
{
    linkg_switch_policy_runtime_t   *policy;
    linkg_switch_policy_operation_t *operation;
    uint32_t                         index;

    policy               = &peer->role.sta.policy;
    policy->group_mask   = mask;
    policy->plan_sync_id = sync_id;
    policy->group_reason = event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD ? LINKG_SWITCH_POLICY_REASON_LOAD :
                           event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD ? LINKG_SWITCH_POLICY_REASON_LOSS :
                           event->type == LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY ? LINKG_SWITCH_POLICY_REASON_RECOVERY :
                           LINKG_SWITCH_POLICY_REASON_WIFI_DOWN;
    policy->group_target = policy->group_reason == LINKG_SWITCH_POLICY_REASON_WIFI_DOWN ?
                           LINKG_SWITCH_POLICY_TARGET_CELLULAR : LINKG_SWITCH_POLICY_TARGET_NONE;
    policy->recovery_count       = 0U;
    policy->next_action_us       = 0U;
    policy->group_wifi_link_id   = peer->role.sta.observation.wifi.link_id;
    policy->group_cellular_link_id = peer->role.sta.observation.cellular.link_id;
    policy->group_rssi_valid = peer->role.sta.observation.wifi.radio.statistics_valid || policy->last_wifi_rssi_valid;
    policy->group_rssi_dbm = peer->role.sta.observation.wifi.radio.statistics_valid ?
                             peer->role.sta.observation.wifi.radio.rssi_dbm : policy->last_wifi_rssi_dbm;

    linkg_switch_policy_verify_reset(&peer->role.sta.verify);

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        operation = &policy->operations[index];
        operation->operation_id = _linkg_switch_operation_allocate_id_locked(policy);
        operation->reason       = policy->group_reason;
        operation->exit_target  = policy->group_target;
    }

    _linkg_switch_operation_set_phase_locked(peer,
        policy->group_reason == LINKG_SWITCH_POLICY_REASON_WIFI_DOWN ?
        LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK : LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK, now_us);
}

/****************************** 事件处理 ******************************/

/**
 * @brief 提交Wi-Fi硬故障或软质量触发的完整Plan事务。
 */
static int _linkg_switch_operation_start(const linkg_switch_policy_event_t *event, uint64_t now_us)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_send_plan_t              previous;
    linkg_send_plan_t              next;
    uint32_t                       mask;
    uint32_t                       sync_id;
    bool                           hard_failure;
    int                            ret;

    hard_failure = event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN;
    pthread_mutex_lock(&g_switch.lock);
    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    policy = &peer->role.sta.policy;
    if (!hard_failure && (policy->group_mask != 0U ||
                          peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
                          peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    previous = peer->plan;
    mask     = _linkg_switch_operation_build_enter_locked(peer, event, &next);
    pthread_mutex_unlock(&g_switch.lock);

    if (mask == 0U || _linkg_switch_operation_plan_equal(&previous, &next))
    {
        return 0;
    }

    sync_id = 0U;
    ret = linkg_switch_plan_submit_if_current(event->peer_node_id, event->peer_generation,
                                              &previous, &next, now_us, &sync_id);
    if (ret != 0)
    {
        return ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE ? 0 : ret;
    }

    pthread_mutex_lock(&g_switch.lock);
    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer != NULL && peer->role.sta.plan_sync.sync_id == sync_id)
    {
        // 硬故障抢占此前验证；旧Plan同步被Plan层取消。
        _linkg_switch_operation_clear_group_locked(peer);
        _linkg_switch_operation_start_locked(peer, event, mask, sync_id, now_us);
    }
    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 为VERIFY_DONE或LOAD_TIMEOUT构造最终单发计划。
 */
static int _linkg_switch_operation_exit(const linkg_switch_policy_event_t *event, uint64_t now_us)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_send_plan_t              previous;
    linkg_send_plan_t              next;
    linkg_switch_policy_target_t   target;
    uint32_t                       mask;
    uint32_t                       sync_id;
    uint32_t                       index;
    uint32_t                       expected_id;
    bool                           wifi_usable;
    bool                           cellular_usable;
    int                            ret;

    pthread_mutex_lock(&g_switch.lock);
    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    policy = &peer->role.sta.policy;
    if (policy->group_mask == 0U ||
        (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY &&
         policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
        {
            break;
        }
    }

    expected_id = policy->operations[index].operation_id;
    if ((event->type == LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE &&
         (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY ||
          event->payload.verify_done.operation_id != expected_id)) ||
        (event->type == LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT &&
         (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER ||
          event->payload.load_timeout.operation_id != expected_id)))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (!_linkg_switch_operation_group_plan_matches_locked(peer, false))
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    wifi_usable     = _linkg_switch_operation_wifi_usable_locked(peer);
    cellular_usable = _linkg_switch_operation_cellular_usable_locked(peer);
    target          = policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY ?
                      LINKG_SWITCH_POLICY_TARGET_CELLULAR : LINKG_SWITCH_POLICY_TARGET_WIFI;

    if (event->type == LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT)
    {
        target = cellular_usable ? LINKG_SWITCH_POLICY_TARGET_CELLULAR : LINKG_SWITCH_POLICY_TARGET_WIFI;
    }
    else if (event->payload.verify_done.result == LINKG_SWITCH_POLICY_VERIFY_WIFI)
    {
        target = LINKG_SWITCH_POLICY_TARGET_WIFI;
    }
    else if (event->payload.verify_done.result == LINKG_SWITCH_POLICY_VERIFY_CELLULAR)
    {
        target = LINKG_SWITCH_POLICY_TARGET_CELLULAR;
    }

    if (target == LINKG_SWITCH_POLICY_TARGET_CELLULAR && !cellular_usable)
    {
        target = LINKG_SWITCH_POLICY_TARGET_WIFI;
    }
    if (target == LINKG_SWITCH_POLICY_TARGET_WIFI && !wifi_usable)
    {
        target = LINKG_SWITCH_POLICY_TARGET_CELLULAR;
    }
    if ((target == LINKG_SWITCH_POLICY_TARGET_WIFI && !wifi_usable) ||
        (target == LINKG_SWITCH_POLICY_TARGET_CELLULAR && !cellular_usable))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    previous = peer->plan;
    next     = previous;
    mask     = policy->group_mask;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        next.classes[index].mode              = LINKG_SEND_MODE_SINGLE;
        next.classes[index].primary_link_id   = target == LINKG_SWITCH_POLICY_TARGET_WIFI ?
                                                policy->group_wifi_link_id : policy->group_cellular_link_id;
        next.classes[index].secondary_link_id = target == LINKG_SWITCH_POLICY_TARGET_WIFI ?
                                                policy->group_cellular_link_id : policy->group_wifi_link_id;
    }

    pthread_mutex_unlock(&g_switch.lock);

    sync_id = 0U;
    ret = linkg_switch_plan_submit_if_current(event->peer_node_id, event->peer_generation,
                                              &previous, &next, now_us, &sync_id);
    if (ret != 0)
    {
        return ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE ? 0 : ret;
    }

    pthread_mutex_lock(&g_switch.lock);
    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer != NULL && peer->role.sta.policy.group_mask == mask &&
        peer->role.sta.plan_sync.sync_id == sync_id)
    {
        policy                 = &peer->role.sta.policy;
        policy->group_target   = target;
        policy->plan_sync_id   = sync_id;
        policy->next_action_us = 0U;
        policy->recovery_count = 0U;
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK, now_us);
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
            {
                policy->operations[index].exit_target = target;
            }
        }
        linkg_switch_policy_verify_reset(&peer->role.sta.verify);
    }
    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 消费初始策略事件或已有事务关联的后续验证事件。
 */
int linkg_switch_policy_operation_process_event(const linkg_switch_policy_event_t *event, uint64_t now_us)
{
    if (event == NULL || now_us == 0U ||
        event->peer_node_id == 0U || event->peer_node_id == UINT8_MAX ||
        event->peer_generation == 0U)
    {
        return -EINVAL;
    }

    switch (event->type)
    {
        case LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD:
        case LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY:
            return _linkg_switch_operation_start(event, now_us);

        case LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE:
        case LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT:
            return _linkg_switch_operation_exit(event, now_us);

        default:
            return -EINVAL;
    }
}

/****************************** Plan同步结果 ******************************/

/**
 * @brief 在成功退出冗余并得到ACK后提交迁移历史和恢复基线。
 */
static void _linkg_switch_operation_finish_locked(linkg_switch_peer_runtime_t *peer, uint64_t now_us)
{
    linkg_switch_policy_runtime_t   *policy;
    linkg_switch_policy_operation_t *operation;
    uint32_t                         index;

    policy = &peer->role.sta.policy;
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        operation = &policy->operations[index];
        if (policy->group_target == LINKG_SWITCH_POLICY_TARGET_CELLULAR)
        {
            if (policy->group_reason != LINKG_SWITCH_POLICY_REASON_RECOVERY)
            {
                operation->migration_reason =
                    policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOAD ? LINKG_SWITCH_POLICY_MIGRATION_LOAD :
                    policy->group_reason == LINKG_SWITCH_POLICY_REASON_WIFI_DOWN ? LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN :
                    LINKG_SWITCH_POLICY_MIGRATION_LOSS;
                operation->recovery.migrated_us       = now_us;
                operation->recovery.rssi_valid        = policy->group_rssi_valid;
                operation->recovery.rssi_dbm          = policy->group_rssi_dbm;
                operation->recovery.recovery_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RECOVERY_HOLDOFF_US);
            }
            else
            {
                operation->recovery.recovery_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_FAILED_HOLDOFF_US);
            }
        }
        else
        {
            operation->migration_reason = LINKG_SWITCH_POLICY_MIGRATION_NONE;
            memset(&operation->recovery, 0, sizeof(operation->recovery));
        }
    }

    _linkg_switch_operation_clear_group_locked(peer);
}

/**
 * @brief 消费PlanSync终态；只接受本组sync_id，不用message_id代表业务事务。
 */
int linkg_switch_policy_operation_on_plan_result(const linkg_switch_plan_sync_result_t *result)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_switch_policy_event_t    event;
    uint32_t                       operation_ids[LINKG_TRANSPORT_CLASS_COUNT];
    uint32_t                       index;
    uint32_t                       mask;
    uint64_t                       now_us;
    int                            ret;
    bool                           fallback;

    if (result == NULL || result->updated_us == 0U)
    {
        return -EINVAL;
    }

    fallback = false;
    // ACK产生于事件队列处理阶段；随后Observation可能已刷新。
    // 以本次Worker实际消费结果的时刻开始验证，避免新快照时间晚于旧ACK时间。
    now_us   = linkg_time_monotonic_us();
    if (now_us < result->updated_us)
    {
        now_us = result->updated_us;
    }
    memset(&event, 0, sizeof(event));
    memset(operation_ids, 0, sizeof(operation_ids));

    pthread_mutex_lock(&g_switch.lock);
    peer = linkg_switch_find_peer_generation_locked(result->peer_node_id, result->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    policy = &peer->role.sta.policy;
    if (policy->group_mask == 0U || policy->plan_sync_id != result->sync_id)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 旧终态可能滞留在结果槽位，不能推进已经被另一份Plan覆盖的同步。
    if (peer->role.sta.plan_sync.sync_id != result->sync_id)
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (result->state == LINKG_SWITCH_PLAN_SYNC_CANCELLED)
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (result->state != LINKG_SWITCH_PLAN_SYNC_CONFIRMED)
    {
        if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK)
        {
            // 未得到可靠的进入ACK时尽力退回原主链路；5G恢复尝试退回Cellular。
            for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
            {
                if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
                {
                    break;
                }
            }
            event.type                             = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
            event.peer_node_id                     = peer->peer_node_id;
            event.peer_generation                  = peer->generation;
            event.occurred_us                      = now_us;
            event.payload.verify_done.operation_id = policy->operations[index].operation_id;
            event.payload.verify_done.result       = LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
            _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY, now_us);
            fallback = true;
        }
        else
        {
            // 本地单发已经生效，远端未知；有限重试同一完整Plan。
            _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_UNCERTAIN, now_us);
            policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RESYNC_INTERVAL_US);
        }
        pthread_mutex_unlock(&g_switch.lock);
        return fallback ? linkg_switch_policy_operation_process_event(&event, now_us) : 0;
    }

    if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK)
    {
        if (!_linkg_switch_operation_group_plan_matches_locked(peer, true))
        {
            _linkg_switch_operation_clear_group_locked(peer);
            pthread_mutex_unlock(&g_switch.lock);
            return 0;
        }
        _linkg_switch_operation_finish_locked(peer, now_us);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (!_linkg_switch_operation_group_plan_matches_locked(peer, false))
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    mask = policy->group_mask;
    if (policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOAD)
    {
        policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_LOAD_DURATION_US);
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER, now_us);
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
            {
                policy->operations[index].started_us  = now_us;
                policy->operations[index].deadline_us = policy->next_action_us;
            }
        }
    }
    else if (policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOSS ||
             policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            operation_ids[index] = (mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U ?
                                   policy->operations[index].operation_id : 0U;
        }

        ret = linkg_switch_policy_verify_begin(&peer->role.sta.verify,
                                               peer->peer_node_id, peer->generation,
                                               operation_ids, mask, policy->group_reason,
                                               &peer->role.sta.observation, now_us);
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY, now_us);
        policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_POLICY_VERIFY_DURATION_US);
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
            {
                policy->operations[index].started_us  = now_us;
                policy->operations[index].deadline_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_POLICY_VERIFY_DURATION_US);
            }
        }
        // 若ACK时观测尚未准备好，周期处理按INCONCLUSIVE安全回退。
        (void)ret;
    }
    pthread_mutex_unlock(&g_switch.lock);
    return 0;
}

/****************************** 周期处理 ******************************/

/**
 * @brief 保存最新稳定Wi-Fi RSSI，供物理断线后的恢复门槛使用。
 */
static void _linkg_switch_operation_sample_wifi_locked(linkg_switch_peer_runtime_t *peer)
{
    linkg_switch_policy_runtime_t *policy;

    if (!peer->role.sta.observation.valid ||
        peer->role.sta.observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        !peer->role.sta.observation.wifi.radio.statistics_valid)
    {
        return;
    }

    policy = &peer->role.sta.policy;
    policy->last_wifi_rssi_dbm   = peer->role.sta.observation.wifi.radio.rssi_dbm;
    policy->last_wifi_rssi_valid = true;
}

/**
 * @brief 识别Reconcile或外部计划导致的Wi-Fi到Cellular迁移。
 *
 * 正常Policy迁移历史由EXIT ACK确认；此处只补全没有Operation记录的迁移。
 */
static void _linkg_switch_operation_track_external_locked(linkg_switch_peer_runtime_t *peer, uint64_t now_us)
{
    linkg_switch_policy_runtime_t   *policy;
    linkg_switch_policy_operation_t *operation;
    uint32_t                         cellular_link_id;
    uint32_t                         index;

    policy           = &peer->role.sta.policy;
    cellular_link_id = peer->role.sta.observation.cellular.link_id;
    if (peer->role.sta.observation.wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
        peer->role.sta.observation.wifi.link_id != LINKG_LINK_ID_INVALID)
    {
        policy->last_wifi_link_id = peer->role.sta.observation.wifi.link_id;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        operation = &policy->operations[index];
        if (policy->group_mask == 0U && policy->observed_plan_valid &&
            policy->last_wifi_link_id != LINKG_LINK_ID_INVALID &&
            policy->observed_primary_link_ids[index] == policy->last_wifi_link_id &&
            peer->plan.classes[index].mode == LINKG_SEND_MODE_SINGLE &&
            peer->plan.classes[index].primary_link_id == cellular_link_id &&
            cellular_link_id != LINKG_LINK_ID_INVALID &&
            operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_NONE)
        {
            operation->migration_reason             = LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN;
            operation->recovery.migrated_us        = now_us;
            operation->recovery.rssi_valid         = policy->last_wifi_rssi_valid;
            operation->recovery.rssi_dbm           = policy->last_wifi_rssi_dbm;
            operation->recovery.recovery_after_us  = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RECOVERY_HOLDOFF_US);
        }

        policy->observed_primary_link_ids[index] = peer->plan.classes[index].primary_link_id;
    }

    policy->observed_plan_valid = true;
}

/**
 * @brief 在Worker处理到期验证、LOAD计时和有限协议重同步。
 *
 * 任何外部Plan覆盖都会使当前组失效，禁止旧结果应用到新Plan。
 */
int linkg_switch_policy_operation_process(uint64_t now_us)
{
    linkg_switch_peer_runtime_t      *peer;
    linkg_switch_policy_runtime_t    *policy;
    linkg_switch_policy_event_t       event;
    linkg_send_plan_t                 previous;
    uint32_t                          peer_generation;
    uint32_t                          sync_id;
    uint32_t                          index;
    uint8_t                           peer_node_id;
    bool                              event_ready;
    bool                              resync;
    int                               ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    memset(&event, 0, sizeof(event));
    event_ready = false;
    resync      = false;
    peer_node_id = 0U;
    peer_generation = 0U;

    pthread_mutex_lock(&g_switch.lock);
    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    peer = NULL;
    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (g_switch.peers[index].used)
        {
            peer = &g_switch.peers[index];
            break;
        }
    }

    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    _linkg_switch_operation_sample_wifi_locked(peer);
    _linkg_switch_operation_track_external_locked(peer, now_us);
    policy = &peer->role.sta.policy;
    if (policy->group_mask == 0U)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    peer_node_id    = peer->peer_node_id;
    peer_generation = peer->generation;
    if (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_UNCERTAIN &&
        policy->plan_sync_id != peer->role.sta.plan_sync.sync_id)
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if ((policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY ||
         policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER) &&
        !_linkg_switch_operation_group_plan_matches_locked(peer, false))
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY)
    {
        if (peer->role.sta.verify.active)
        {
            ret = linkg_switch_policy_verify_process(&peer->role.sta.verify,
                &peer->role.sta.observation, now_us, &event, &event_ready);
            if (ret != 0)
            {
                pthread_mutex_unlock(&g_switch.lock);
                return ret;
            }
        }
        else if (now_us >= policy->next_action_us)
        {
            // ACK时观测无效仍遵守固定3秒窗口，到期后才输出INCONCLUSIVE。
            for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
            {
                if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
                {
                    break;
                }
            }
            event.type                             = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
            event.peer_node_id                     = peer_node_id;
            event.peer_generation                  = peer_generation;
            event.occurred_us                      = now_us;
            event.payload.verify_done.operation_id = policy->operations[index].operation_id;
            event.payload.verify_done.result       = LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
            event_ready = true;
        }
    }
    else if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER &&
             now_us >= policy->next_action_us)
    {
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
            {
                break;
            }
        }
        event.type                              = LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT;
        event.peer_node_id                      = peer_node_id;
        event.peer_generation                   = peer_generation;
        event.occurred_us                       = now_us;
        event.payload.load_timeout.operation_id = policy->operations[index].operation_id;
        event_ready = true;
    }
    else if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_UNCERTAIN &&
             now_us >= policy->next_action_us &&
             policy->recovery_count < LINKG_SWITCH_OPERATION_RESYNC_MAX)
    {
        previous = peer->plan;
        policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RESYNC_INTERVAL_US);
        resync = true;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (event_ready)
    {
        return linkg_switch_policy_operation_process_event(&event, now_us);
    }

    if (!resync)
    {
        return 0;
    }

    sync_id = 0U;
    ret = linkg_switch_plan_submit_if_current(peer_node_id, peer_generation,
                                              &previous, &previous, now_us, &sync_id);
    if (ret != 0)
    {
        return ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE ? 0 : ret;
    }

    pthread_mutex_lock(&g_switch.lock);
    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer != NULL && peer->role.sta.policy.group_phase == LINKG_SWITCH_POLICY_PHASE_UNCERTAIN &&
        peer->role.sta.plan_sync.sync_id == sync_id)
    {
        peer->role.sta.policy.plan_sync_id = sync_id;
        peer->role.sta.policy.recovery_count++;
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK, now_us);
    }
    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 返回当前组最早的协议后续动作或质量验证截止时刻。
 */
uint64_t linkg_switch_policy_operation_next_deadline_locked(void)
{
    const linkg_switch_policy_runtime_t *policy;
    const linkg_switch_peer_runtime_t   *peer;
    uint32_t                             index;
    uint64_t                             deadline;
    uint64_t                             now_us;

    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        return UINT64_MAX;
    }

    deadline = UINT64_MAX;
    now_us   = linkg_time_monotonic_us();
    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];
        if (!peer->used)
        {
            continue;
        }

        policy = &peer->role.sta.policy;
        if (policy->group_mask == 0U)
        {
            continue;
        }

        if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY)
        {
            if (peer->role.sta.verify.active && !peer->role.sta.verify.result_pending &&
                peer->role.sta.verify.deadline_us > now_us &&
                peer->role.sta.verify.deadline_us < deadline)
            {
                deadline = peer->role.sta.verify.deadline_us;
            }
            else if (!peer->role.sta.verify.active &&
                     policy->next_action_us > now_us && policy->next_action_us < deadline)
            {
                deadline = policy->next_action_us;
            }
        }
        else if ((policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER ||
                  (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_UNCERTAIN &&
                   policy->recovery_count < LINKG_SWITCH_OPERATION_RESYNC_MAX)) &&
                 policy->next_action_us > now_us && policy->next_action_us < deadline)
        {
            deadline = policy->next_action_us;
        }
    }

    return deadline;
}

/****************************** 生命周期 ******************************/

/**
 * @brief Peer移除时释放尚未消费的验证会话与全部Policy事务暂态。
 */
void linkg_switch_policy_operation_reset_locked(uint8_t peer_node_id, uint32_t peer_generation)
{
    linkg_switch_peer_runtime_t *peer;

    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer != NULL)
    {
        _linkg_switch_operation_clear_group_locked(peer);
    }
}
