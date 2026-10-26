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
#include "linkg_transport.h"

#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_policy_verify.h"

/****************************** 事务参数 ******************************/

#define LINKG_SWITCH_OPERATION_LOAD_DURATION_US         1000000ULL  // LOAD冗余ACK后1秒迁移
#define LINKG_SWITCH_OPERATION_RECOVERY_HOLDOFF_US     10000000ULL  // 迁出Wi-Fi后最少等待10秒
#define LINKG_SWITCH_OPERATION_FAILED_HOLDOFF_US       15000000ULL  // 恢复失败后延迟15秒重试
#define LINKG_SWITCH_OPERATION_RESYNC_INTERVAL_US       2000000ULL  // 协议终态失败后重同步间隔
#define LINKG_SWITCH_OPERATION_RESYNC_MAX                     2U    // 整组事务追加重同步次数
#define LINKG_SWITCH_OPERATION_REPAIR_INTERVAL_US      10000000ULL  // 重试耗尽后每10秒修复完整Plan
#define LINKG_SWITCH_OPERATION_LOAD_ACTIVE_MAX_AGE_MS      1500ULL  // LOAD仅迁移最近1.5秒有真实用户数据的业务

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

    // 分配新的Operation ID，跳过保留的无效编号。
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

    // 三类业务的模式、主链路和备用链路均一致，才认为完整Plan相等。
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

    /****************************** 清理各业务事务暂态 ******************************/

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        operation = &policy->operations[index];

        // 仅清理本次事务状态，保留migration_reason和recovery历史。
        operation->phase              = LINKG_SWITCH_POLICY_PHASE_IDLE;
        operation->operation_id       = 0U;
        operation->reason             = LINKG_SWITCH_POLICY_REASON_NONE;
        operation->exit_target        = LINKG_SWITCH_POLICY_TARGET_NONE;
        operation->migrated_from_wifi = false;
        operation->phase_entered_us   = 0U;
        operation->started_us         = 0U;
        operation->deadline_us        = 0U;
    }

    /****************************** 清理组级事务暂态 ******************************/

    policy->group_mask             = 0U;
    policy->group_reason           = LINKG_SWITCH_POLICY_REASON_NONE;
    policy->group_phase            = LINKG_SWITCH_POLICY_PHASE_IDLE;
    policy->group_target           = LINKG_SWITCH_POLICY_TARGET_NONE;
    policy->plan_sync_id           = 0U;
    policy->next_action_us         = 0U;
    policy->recovery_count         = 0U;
    policy->group_rssi_valid       = false;
    policy->group_wifi_link_id     = LINKG_LINK_ID_INVALID;
    policy->group_cellular_link_id = LINKG_LINK_ID_INVALID;

    /****************************** 释放验证会话 ******************************/

    // 验证状态独立于迁移历史，每次结束或取消事务均重置。
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

    // 仅同步本组参与业务的阶段，不改变其他业务的Operation状态。
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
 * @brief 检查当前Plan是否符合Operation事务阶段的预期。
 *
 * exit_plan为false时，检查进入冗余后的发送计划。
 * exit_plan为true时，检查退出冗余后的最终单发计划。
 * 仅检查group_mask包含的业务，防止旧Operation覆盖外部Plan修改。
 *
 * 调用方必须持有g_switch.lock。
 */
static bool _linkg_switch_operation_group_plan_matches_locked(const linkg_switch_peer_runtime_t *peer, bool exit_plan)
{
    const linkg_switch_policy_runtime_t *policy;
    const linkg_class_send_plan_t       *current;
    uint32_t                             expected_primary;
    uint32_t                             expected_secondary;
    uint32_t                             index;

    policy = &peer->role.sta.policy;

    /****************************** 确定预期链路 ******************************/

    if (exit_plan)
    {
        // 退出冗余：根据最终目标确定单发主链路。
        expected_primary = policy->group_target == LINKG_SWITCH_POLICY_TARGET_CELLULAR ?
                           policy->group_cellular_link_id : policy->group_wifi_link_id;

        expected_secondary = LINKG_LINK_ID_INVALID;
    }
    else if (policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        // Wi-Fi恢复验证：Cellular主发，Wi-Fi冗余。
        expected_primary   = policy->group_cellular_link_id;
        expected_secondary = policy->group_wifi_link_id;
    }
    else
    {
        // Wi-Fi质量异常或负载迁移：Wi-Fi主发，Cellular冗余。
        expected_primary   = policy->group_wifi_link_id;
        expected_secondary = policy->group_cellular_link_id;
    }

    /****************************** 检查事务业务计划 ******************************/

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        // 只检查当前Operation事务涉及的业务类别。
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        current = &peer->plan.classes[index];

        if (exit_plan)
        {
            // 最终单发计划必须保持预期模式和主链路。
            if (current->mode != LINKG_SEND_MODE_SINGLE ||
                current->primary_link_id != expected_primary)
            {
                return false;
            }

            continue;
        }

        // 冗余计划必须保持预期模式及两条发送链路。
        if (current->mode != LINKG_SEND_MODE_REDUNDANT ||
            current->primary_link_id != expected_primary ||
            current->secondary_link_id != expected_secondary)
        {
            return false;
        }
    }

    return true;
}


/**
 * @brief 根据初始策略事件构建完整三业务候选发送计划。
 *
 * 仅生成新的Plan和受影响业务掩码，不修改当前Plan，不创建同步事务。
 * 调用方必须持有g_switch.lock。
 */
static uint32_t _linkg_switch_operation_build_enter_locked(const linkg_switch_peer_runtime_t *peer,
                                                           const linkg_switch_policy_event_t *event,
                                                           const bool active_classes[LINKG_TRANSPORT_CLASS_COUNT],
                                                           linkg_send_plan_t *next)
{
    const linkg_switch_observation_t *observation;
    linkg_class_send_plan_t          *item;
    uint32_t                          class_index;
    uint32_t                          index;
    uint32_t                          mask;
    uint32_t                          bit;
    bool                              wifi_usable;
    bool                              cellular_usable;

    observation       = &peer->role.sta.observation;
    wifi_usable       = _linkg_switch_operation_wifi_usable_locked(peer);
    cellular_usable   = _linkg_switch_operation_cellular_usable_locked(peer);
    mask              = 0U;
    *next             = peer->plan;

    // Cellular不可用时，无法建立新的切换计划。
    if (!cellular_usable)
    {
        return 0U;
    }

    // 除Wi-Fi硬故障外，其余策略事件都要求Wi-Fi可用。
    if (event->type != LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN && !wifi_usable)
    {
        return 0U;
    }

    switch (event->type)
    {
        /****************************** Wi-Fi硬故障 ******************************/

        case LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN:
            // 检查所有业务，将受Wi-Fi故障影响的业务直接改为Cellular单发。
            for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
            {
                item = &next->classes[index];
                bit  = LINKG_SWITCH_POLICY_CLASS_BIT(index);

                // Cellular主发的冗余业务，直接移除失效的备用链路。
                if (item->mode == LINKG_SEND_MODE_REDUNDANT &&
                    item->primary_link_id == observation->cellular.link_id &&
                    item->secondary_link_id != LINKG_LINK_ID_INVALID &&
                    item->secondary_link_id != observation->cellular.link_id)
                {
                    item->mode              = LINKG_SEND_MODE_SINGLE;
                    item->secondary_link_id = LINKG_LINK_ID_INVALID;
                    mask |= bit;
                    continue;
                }

                // 跳过未启用、已由Cellular主发或没有Cellular备用的业务。
                if (item->mode == LINKG_SEND_MODE_NONE ||
                    item->primary_link_id == observation->cellular.link_id ||
                    item->secondary_link_id != observation->cellular.link_id)
                {
                    continue;
                }

                // 不依赖当前Wi-Fi Link ID，兼容旧Wi-Fi实例已经失效的情况。
                item->mode              = LINKG_SEND_MODE_SINGLE;
                item->primary_link_id   = observation->cellular.link_id;
                item->secondary_link_id = LINKG_LINK_ID_INVALID;
                mask |= bit;
            }
            break;

        /****************************** Wi-Fi无线丢包 ******************************/

        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD:
            // 所有符合条件的Wi-Fi单发业务进入冗余质量验证。
            for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
            {
                item = &next->classes[index];

                if (item->mode != LINKG_SEND_MODE_SINGLE ||
                    item->primary_link_id != observation->wifi.link_id ||
                    item->secondary_link_id != observation->cellular.link_id)
                {
                    continue;
                }

                item->mode = LINKG_SEND_MODE_REDUNDANT;
                mask |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
            }
            break;

        /****************************** Wi-Fi队列溢出 ******************************/

        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD:
            // DATA、VIDEO、REALTIME依次尝试，只选择一类活跃业务。
            for (index = LINKG_TRANSPORT_CLASS_COUNT; index > 0U; index--)
            {
                class_index = index - 1U;

                // 只迁移真实活跃的用户业务。
                if (!active_classes[class_index])
                {
                    continue;
                }

                item = &next->classes[class_index];

                if (item->mode != LINKG_SEND_MODE_SINGLE ||
                    item->primary_link_id != observation->wifi.link_id ||
                    item->secondary_link_id != observation->cellular.link_id)
                {
                    continue;
                }

                item->mode = LINKG_SEND_MODE_REDUNDANT;
                mask |= LINKG_SWITCH_POLICY_CLASS_BIT(class_index);
                break;
            }
            break;

        /****************************** Wi-Fi恢复候选 ******************************/

        case LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY:
            // 两类恢复事件都只处理class_mask指定的Cellular单发业务。
            for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
            {
                bit = LINKG_SWITCH_POLICY_CLASS_BIT(index);

                if ((event->payload.recovery_ready.class_mask & bit) == 0U)
                {
                    continue;
                }

                item = &next->classes[index];

                // 仅处理Cellular单发、Wi-Fi备用的业务。
                if (item->mode != LINKG_SEND_MODE_SINGLE ||
                    item->primary_link_id != observation->cellular.link_id ||
                    item->secondary_link_id != observation->wifi.link_id)
                {
                    continue;
                }

                if (event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY)
                {
                    // 直接恢复：保持单发，Wi-Fi升为Primary，Cellular保留为Secondary。
                    item->mode              = LINKG_SEND_MODE_SINGLE;
                    item->primary_link_id   = observation->wifi.link_id;
                    item->secondary_link_id = observation->cellular.link_id;
                }
                else
                {
                    // 质量恢复：保留原有双发Verify流程。
                    item->mode = LINKG_SEND_MODE_REDUNDANT;
                }

                mask |= bit;
            }
            break;

        default:
            return 0U;
    }

    return mask;
}


/**
 * @brief 记录已成功提交PlanSync的组级切换事务。
 *
 * 保存本次事务原因、目标链路、观测基线及业务Operation ID。
 * Wi-Fi硬故障和Wi-Fi直接恢复等待最终单发ACK，其余事件等待进入冗余ACK。
 * 调用方必须持有g_switch.lock。
 */
static void _linkg_switch_operation_start_locked(linkg_switch_peer_runtime_t *peer, const linkg_switch_policy_event_t *event, const linkg_send_plan_t *previous, uint32_t mask, uint32_t sync_id, uint64_t now_us)
{
    const linkg_switch_observation_t *observation;
    linkg_switch_policy_runtime_t    *policy;
    linkg_switch_policy_operation_t  *operation;
    linkg_switch_policy_phase_t       phase;
    uint32_t                          index;

    observation = &peer->role.sta.observation;
    policy      = &peer->role.sta.policy;

    /****************************** 事务标识 ******************************/

    policy->group_mask   = mask;
    policy->plan_sync_id = sync_id;

    // 根据初始事件记录本次切换原因。
    switch (event->type)
    {
        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD:
            policy->group_reason = LINKG_SWITCH_POLICY_REASON_LOSS;
            break;

        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD:
            policy->group_reason = LINKG_SWITCH_POLICY_REASON_LOAD;
            break;

        case LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY:
            policy->group_reason = LINKG_SWITCH_POLICY_REASON_RECOVERY;
            break;

        case LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN:
        default:
            policy->group_reason = LINKG_SWITCH_POLICY_REASON_WIFI_DOWN;
            break;
    }

    /****************************** 事务目标与初始阶段 ******************************/

    // Wi-Fi硬故障直接迁移到Cellular，不进入冗余。
    if (event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN)
    {
        policy->group_target = LINKG_SWITCH_POLICY_TARGET_CELLULAR;
        phase                = LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK;
    }
    // 非质量恢复直接返回Wi-Fi单发，仍等待最终PlanSync ACK。
    else if (event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY)
    {
        policy->group_target = LINKG_SWITCH_POLICY_TARGET_WIFI;
        phase                = LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK;
    }
    else
    {
        // LOSS、LOAD及原RECOVERY_READY仍先进入冗余阶段。
        policy->group_target = LINKG_SWITCH_POLICY_TARGET_NONE;
        phase                = LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK;
    }

    policy->recovery_count = 0U;
    policy->next_action_us = 0U;

    /****************************** 链路及RSSI基线 ******************************/

    // 保存本次事务对应的Wi-Fi和Cellular Link实例。
    policy->group_wifi_link_id     = observation->wifi.link_id;
    policy->group_cellular_link_id = observation->cellular.link_id;

    // Wi-Fi断开后当前Link ID可能无效，回退到最近一次有效实例。
    if (policy->group_wifi_link_id == LINKG_LINK_ID_INVALID)
    {
        policy->group_wifi_link_id = policy->last_wifi_link_id;
    }

    // 优先记录当前RSSI；当前无线统计无效时使用历史基线。
    if (observation->wifi.radio.statistics_valid)
    {
        policy->group_rssi_valid = true;
        policy->group_rssi_dbm   = observation->wifi.radio.rssi_dbm;
    }
    else
    {
        policy->group_rssi_valid = policy->last_wifi_rssi_valid;
        policy->group_rssi_dbm   = policy->last_wifi_rssi_dbm;
    }

    /****************************** 初始化业务事务 ******************************/

    // 新事务开始前清理上一轮Verify运行状态。
    linkg_switch_policy_verify_reset(&peer->role.sta.verify);

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        // 只为本次实际参与切换的业务创建Operation。
        if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        operation = &policy->operations[index];

        operation->operation_id = _linkg_switch_operation_allocate_id_locked(policy);
        operation->reason       = policy->group_reason;
        operation->exit_target  = policy->group_target;

        // 记录本次业务是否从Wi-Fi迁出，用于最终ACK后的迁移历史更新。
        operation->migrated_from_wifi =
            policy->group_reason != LINKG_SWITCH_POLICY_REASON_RECOVERY &&
            previous->classes[index].primary_link_id != policy->group_cellular_link_id &&
            previous->classes[index].secondary_link_id == policy->group_cellular_link_id;
    }

    /****************************** 启动事务阶段 ******************************/

    // 设置整个事务组及所包含业务的初始阶段。
    _linkg_switch_operation_set_phase_locked(peer, phase, now_us);
}

/****************************** 事件处理 ******************************/

/**
 * @brief 检查业务最近是否有真正的USER_DATA尝试发送或成功接收。
 */
static bool _linkg_switch_operation_load_active(const linkg_transport_class_stats_t *stats, uint64_t now_ms)
{
    // 最近存在有效的用户发送活动。
    if (stats->last_user_tx_ms != 0U && stats->last_user_tx_ms <= now_ms &&
        now_ms - stats->last_user_tx_ms <= LINKG_SWITCH_OPERATION_LOAD_ACTIVE_MAX_AGE_MS)
    {
        return true;
    }

    // 或者最近存在有效的用户接收活动。
    return stats->last_user_rx_ms != 0U && stats->last_user_rx_ms <= now_ms &&
           now_ms - stats->last_user_rx_ms <= LINKG_SWITCH_OPERATION_LOAD_ACTIVE_MAX_AGE_MS;
}

/**
 * @brief 根据初始策略事件构建并提交完整三业务发送计划。
 *
 * LOAD事件先采集业务活跃状态，再检查当前事务及PlanSync状态。
 * 成功提交后建立Operation事务；Wi-Fi硬故障允许抢占旧事务。
 */
static int _linkg_switch_operation_start(const linkg_switch_policy_event_t *event, uint64_t now_us)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_transport_peer_stats_t   stats;
    linkg_send_plan_t              previous;
    linkg_send_plan_t              next;
    uint64_t                       now_ms;
    uint32_t                       mask;
    uint32_t                       sync_id;
    uint32_t                       index;
    bool                           active_classes[LINKG_TRANSPORT_CLASS_COUNT];
    bool                           hard_failure;
    int                            ret;

    hard_failure = event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN;
    memset(active_classes, 0, sizeof(active_classes));

    /****************************** LOAD业务活跃状态 ******************************/

    if (event->type == LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD)
    {
        // Transport统计在Switch锁外读取，避免嵌套锁。
        ret = linkg_transport_get_peer_stats(event->peer_node_id, &stats);
        if (ret == -ENOENT || ret == -ENODEV)
        {
            return 0;
        }

        if (ret != 0)
        {
            return ret;
        }

        // 按最近的真实用户收发活动，判断三类业务是否活跃。
        now_ms = linkg_time_elapsed_ms();

        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            active_classes[index] = _linkg_switch_operation_load_active(&stats.classes[index], now_ms);
        }
    }

    /****************************** Peer及事务状态检查 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);

    if (peer == NULL || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    policy = &peer->role.sta.policy;

    // 软件质量和恢复事件不得抢占已有事务，Wi-Fi硬故障除外。
    if (!hard_failure)
    {
        // 已存在组级切换事务，不允许再次启动。
        if (policy->group_mask != 0U)
        {
            pthread_mutex_unlock(&g_switch.lock);
            return 0;
        }

        // 完整PlanSync仍在进行，不允许启动新的软件切换事务。
        if (peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
            peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT)
        {
            pthread_mutex_unlock(&g_switch.lock);
            return 0;
        }
    }

    /****************************** 构建完整候选计划 ******************************/

    // 保存当前完整Plan，作为后续条件提交的旧计划基线。
    previous = peer->plan;

    // 根据事件构建新的完整Plan，并返回需要变更的业务集合。
    mask = _linkg_switch_operation_build_enter_locked(peer, event, active_classes, &next);

    pthread_mutex_unlock(&g_switch.lock);

    // 没有业务需要切换，或新旧Plan完全一致。
    if (mask == 0U || _linkg_switch_operation_plan_equal(&previous, &next))
    {
        return 0;
    }

    /****************************** 提交完整PlanSync ******************************/

    // 仅当当前Plan仍与previous一致时，才能提交next。
    sync_id = 0U;

    ret = linkg_switch_plan_submit_if_current(event->peer_node_id, event->peer_generation, &previous, &next, now_us, &sync_id);
    if (ret != 0)
    {
        // 计划已变化、资源被维护阻止或Peer已失效，本次放弃提交。
        if (ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE)
        {
            return 0;
        }

        return ret;
    }

    /****************************** 建立Operation事务 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);

    // 只为本次成功提交的PlanSync建立对应Operation事务。
    if (peer != NULL && peer->role.sta.plan_sync.sync_id == sync_id)
    {
        // 清理旧事务；硬故障抢占时旧PlanSync已由Plan层取消。
        _linkg_switch_operation_clear_group_locked(peer);

        // 根据事件、原计划及业务掩码建立新的组级切换事务。
        _linkg_switch_operation_start_locked(peer, event, &previous, mask, sync_id, now_us);
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}


/**
 * @brief 处理VERIFY_DONE或LOAD_TIMEOUT，提交最终单发计划。
 *
 * 验证当前Operation身份、事务阶段及冗余计划一致性。
 * 根据质量验证结果或LOAD计时选择最终发送链路。
 * 提交完整PlanSync后进入WAIT_EXIT_ACK，等待AP确认。
 *
 * 调用方不得持有g_switch.lock。
 */
static int _linkg_switch_operation_exit(const linkg_switch_policy_event_t *event, uint64_t now_us)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_class_send_plan_t        *current;
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

    /****************************** 获取当前事务 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    policy = &peer->role.sta.policy;

    // 必须存在活动事务，且当前处于验证或LOAD计时阶段。
    if (policy->group_mask == 0U ||
        (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY &&
         policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 校验事件身份 ******************************/

    // 使用本组第一个参与业务的Operation ID校验事件归属。
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
        {
            break;
        }
    }

    expected_id = policy->operations[index].operation_id;

    // VERIFY_DONE必须属于当前WAIT_VERIFY事务。
    if (event->type == LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE &&
        (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY ||
         event->payload.verify_done.operation_id != expected_id))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // LOAD_TIMEOUT必须属于当前WAIT_TIMER事务。
    if (event->type == LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT &&
        (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER ||
         event->payload.load_timeout.operation_id != expected_id))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 校验当前发送计划 ******************************/

    // 当前Plan必须仍然保持本组进入冗余时的预期状态。
    if (!_linkg_switch_operation_group_plan_matches_locked(peer, false))
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 获取链路可用状态 ******************************/

    wifi_usable     = _linkg_switch_operation_wifi_usable_locked(peer);
    cellular_usable = _linkg_switch_operation_cellular_usable_locked(peer);

    /****************************** 选择最终单发目标 ******************************/

    // 验证无法判定时，LOSS优先保留Wi-Fi，RECOVERY优先保留Cellular。
    target = policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY ?
             LINKG_SWITCH_POLICY_TARGET_CELLULAR : LINKG_SWITCH_POLICY_TARGET_WIFI;

    if (event->type == LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT)
    {
        // LOAD不比较质量，Cellular可用时直接迁移。
        target = cellular_usable ? LINKG_SWITCH_POLICY_TARGET_CELLULAR : LINKG_SWITCH_POLICY_TARGET_WIFI;
    }
    else if (event->payload.verify_done.result == LINKG_SWITCH_POLICY_VERIFY_WIFI)
    {
        // Verify确认Wi-Fi更优。
        target = LINKG_SWITCH_POLICY_TARGET_WIFI;
    }
    else if (event->payload.verify_done.result == LINKG_SWITCH_POLICY_VERIFY_CELLULAR)
    {
        // Verify确认Cellular更优。
        target = LINKG_SWITCH_POLICY_TARGET_CELLULAR;
    }

    /****************************** 最终目标可用性保护 ******************************/

    // 首选Cellular不可用时，尝试回退Wi-Fi。
    if (target == LINKG_SWITCH_POLICY_TARGET_CELLULAR && !cellular_usable)
    {
        target = LINKG_SWITCH_POLICY_TARGET_WIFI;
    }

    // 首选Wi-Fi不可用时，尝试改用Cellular。
    if (target == LINKG_SWITCH_POLICY_TARGET_WIFI && !wifi_usable)
    {
        target = LINKG_SWITCH_POLICY_TARGET_CELLULAR;
    }

    // 最终目标仍不可用，本轮不提交单发计划。
    if ((target == LINKG_SWITCH_POLICY_TARGET_WIFI && !wifi_usable) ||
        (target == LINKG_SWITCH_POLICY_TARGET_CELLULAR && !cellular_usable))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 构造最终单发计划 ******************************/

    previous = peer->plan;
    next     = previous;
    mask     = policy->group_mask;

    // 只修改本次Operation涉及的业务类别。
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        current = &next.classes[index];

        current->mode = LINKG_SEND_MODE_SINGLE;

        if (target == LINKG_SWITCH_POLICY_TARGET_WIFI)
        {
            current->primary_link_id   = policy->group_wifi_link_id;
            current->secondary_link_id = policy->group_cellular_link_id;
        }
        else
        {
            current->primary_link_id   = policy->group_cellular_link_id;
            current->secondary_link_id = policy->group_wifi_link_id;
        }
    }

    pthread_mutex_unlock(&g_switch.lock);

    /****************************** 提交最终PlanSync ******************************/

    // 条件提交完整Plan，防止覆盖锁外发生的其他Plan修改。
    sync_id = 0U;

    ret = linkg_switch_plan_submit_if_current(event->peer_node_id, event->peer_generation, &previous, &next, now_us, &sync_id);
    if (ret != 0)
    {
        if (ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE)
        {
            return 0;
        }

        return ret;
    }

    /****************************** 更新退出事务状态 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(event->peer_node_id, event->peer_generation);

    // 确认Peer、事务组和新PlanSync仍然对应本次提交。
    if (peer != NULL &&
        peer->role.sta.policy.group_mask == mask &&
        peer->role.sta.plan_sync.sync_id == sync_id)
    {
        policy = &peer->role.sta.policy;

        policy->group_target   = target;
        policy->plan_sync_id   = sync_id;
        policy->next_action_us = 0U;
        policy->recovery_count = 0U;

        // 最终单发Plan已经提交，等待AP的退出确认。
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK, now_us);

        // 保存本组各业务最终选择的链路。
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
            {
                continue;
            }

            policy->operations[index].exit_target = target;
        }

        // 退出验证阶段，清理Verify运行状态。
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

    // 初始策略事件创建或抢占切换事务；后续事件提交最终单发计划。
    switch (event->type)
    {
        case LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD:
        case LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY:
        case LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY:
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

    /****************************** 提交参与业务的迁移历史 ******************************/

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        operation = &policy->operations[index];

        if (policy->group_target == LINKG_SWITCH_POLICY_TARGET_CELLULAR)
        {
            // 确认从Wi-Fi迁移到Cellular，记录原因、RSSI基线和最短恢复等待时间。
            if (policy->group_reason != LINKG_SWITCH_POLICY_REASON_RECOVERY && operation->migrated_from_wifi)
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
                // 仍以Cellular为主，但不是新的Wi-Fi迁出，推迟下一次恢复尝试。
                operation->recovery.recovery_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_FAILED_HOLDOFF_US);
            }
        }
        else
        {
            // 最终确认Wi-Fi单发，清除旧的Cellular迁移和恢复历史。
            operation->migration_reason = LINKG_SWITCH_POLICY_MIGRATION_NONE;
            memset(&operation->recovery, 0, sizeof(operation->recovery));
        }
    }

    /****************************** 结束当前Operation ******************************/

    // 清理事务暂态，保留上面刚提交的迁移及恢复历史。
    _linkg_switch_operation_clear_group_locked(peer);
}

/**
 * @brief 快速重同步耗尽后保留未知状态，并释放本组事务供后续策略继续使用。
 *
 * AP是否应用了当前本地Plan仍未知，只记录UNKNOWN迁移历史，
 * 通过独立的低频修复任务重新确认完整Plan。
 */
static void _linkg_switch_operation_abandon_uncertain_locked(linkg_switch_peer_runtime_t *peer, uint64_t now_us)
{
    linkg_switch_policy_runtime_t   *policy;
    linkg_switch_policy_operation_t *operation;
    uint32_t                         index;

    policy = &peer->role.sta.policy;

    /****************************** 保留未知迁移历史 ******************************/

    // 只有目标为Cellular且确实从Wi-Fi迁出的业务，才记录UNKNOWN。
    if (policy->group_target == LINKG_SWITCH_POLICY_TARGET_CELLULAR)
    {
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
            {
                continue;
            }

            operation = &policy->operations[index];
            if (!operation->migrated_from_wifi)
            {
                continue;
            }

            // AP计划是否一致仍未知；质量迁移必须保留原因，以继续执行RSSI滞回。
            operation->migration_reason =
                policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOSS ? LINKG_SWITCH_POLICY_MIGRATION_LOSS :
                policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOAD ? LINKG_SWITCH_POLICY_MIGRATION_LOAD :
                LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN;
            operation->recovery.rssi_valid        = policy->group_rssi_valid;
            operation->recovery.rssi_dbm          = policy->group_rssi_dbm;
            operation->recovery.migrated_us       = now_us;
            operation->recovery.recovery_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RECOVERY_HOLDOFF_US);
        }
    }

    /****************************** 释放事务并安排后台修复 ******************************/

    // 活动Operation结束，但STA与AP的Plan仍可能不一致。
    _linkg_switch_operation_clear_group_locked(peer);

    policy->sync_repair_pending  = true;
    policy->sync_repair_id       = 0U;
    policy->sync_repair_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_REPAIR_INTERVAL_US);
}


/**
 * @brief 消费PlanSync终态结果，推进当前Operation事务。
 *
 * 无活动事务时处理后台PlanSync修复结果。
 * 有活动事务时校验sync_id，并根据同步结果推进或终止事务。
 * 进入冗余ACK成功后启动LOAD计时或Verify质量验证。
 * 最终单发ACK成功后确认迁移历史并结束Operation。
 *
 * 调用方不得持有g_switch.lock。
 */
int linkg_switch_policy_operation_on_plan_result(const linkg_switch_plan_sync_result_t *result)
{
    linkg_switch_policy_migration_reason_t recovery_reason;
    linkg_switch_peer_runtime_t           *peer;
    linkg_switch_policy_runtime_t         *policy;
    linkg_switch_policy_event_t            event;
    uint32_t                               operation_ids[LINKG_TRANSPORT_CLASS_COUNT];
    uint32_t                               index;
    uint32_t                               mask;
    uint64_t                               now_us;
    bool                                   fallback;
    int                                    ret;

    if (result == NULL || result->updated_us == 0U)
    {
        return -EINVAL;
    }

    /****************************** 初始化处理上下文 ******************************/

    // 使用Worker实际消费结果的时间，避免新Observation晚于ACK时间。
    now_us = linkg_time_monotonic_us();
    if (now_us < result->updated_us)
    {
        now_us = result->updated_us;
    }

    memset(&event, 0, sizeof(event));
    memset(operation_ids, 0, sizeof(operation_ids));

    recovery_reason = LINKG_SWITCH_POLICY_MIGRATION_NONE;
    fallback        = false;

    /****************************** 获取当前Peer ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(result->peer_node_id, result->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    policy = &peer->role.sta.policy;

    /****************************** 后台PlanSync修复结果 ******************************/

    // 没有活动Operation时，仅处理属于后台Repair的同步结果。
    if (policy->group_mask == 0U)
    {
        if (policy->sync_repair_pending &&
            policy->sync_repair_id != 0U &&
            policy->sync_repair_id == result->sync_id)
        {
            // 当前后台同步获得成功确认，结束Repair任务。
            if (result->state == LINKG_SWITCH_PLAN_SYNC_CONFIRMED &&
                peer->role.sta.plan_sync.sync_id == result->sync_id)
            {
                policy->sync_repair_pending  = false;
                policy->sync_repair_after_us = 0U;
            }
            else
            {
                // 本轮修复未获得可靠确认，安排下一次低频重试。
                policy->sync_repair_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_REPAIR_INTERVAL_US);
            }

            policy->sync_repair_id = 0U;
        }

        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 校验PlanSync事务身份 ******************************/

    // 只消费当前Operation等待的同步结果。
    if (policy->plan_sync_id != result->sync_id)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 当前PlanSync已经被其他同步覆盖，清理旧Operation。
    if (peer->role.sta.plan_sync.sync_id != result->sync_id)
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 处理同步取消 ******************************/

    // PlanSync被取消，当前Operation不再继续推进。
    if (result->state == LINKG_SWITCH_PLAN_SYNC_CANCELLED)
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 处理同步失败 ******************************/

    if (result->state != LINKG_SWITCH_PLAN_SYNC_CONFIRMED)
    {
        if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK)
        {
            /****************************** 进入冗余失败回退 ******************************/

            // 找到本组第一个参与业务的Operation ID。
            for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
            {
                if ((policy->group_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) != 0U)
                {
                    break;
                }
            }

            // 冗余未可靠确认，生成无法判定的验证结果以尝试退出。
            event.type                             = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
            event.peer_node_id                     = peer->peer_node_id;
            event.peer_generation                  = peer->generation;
            event.occurred_us                      = now_us;
            event.payload.verify_done.operation_id = policy->operations[index].operation_id;
            event.payload.verify_done.result       = LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;

            // 转为WAIT_VERIFY，使统一退出入口能够处理回退事件。
            _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY, now_us);
            fallback = true;
        }
        else if (policy->recovery_count >= LINKG_SWITCH_OPERATION_RESYNC_MAX)
        {
            /****************************** 追加重同步耗尽 ******************************/

            // 结束活动Operation，保留未知状态并安排低频后台修复。
            _linkg_switch_operation_abandon_uncertain_locked(peer, now_us);
        }
        else
        {
            /****************************** 安排追加重同步 ******************************/

            // 本地最终单发Plan已生效，但远端确认不可靠。
            _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_UNCERTAIN, now_us);
            policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RESYNC_INTERVAL_US);
        }

        pthread_mutex_unlock(&g_switch.lock);

        // 进入冗余失败时，在锁外提交安全回退事件。
        return fallback ? linkg_switch_policy_operation_process_event(&event, now_us) : 0;
    }

    /****************************** 最终单发ACK确认 ******************************/

    if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK)
    {
        // 当前本地Plan必须仍然符合最终单发目标。
        if (!_linkg_switch_operation_group_plan_matches_locked(peer, true))
        {
            _linkg_switch_operation_clear_group_locked(peer);
            pthread_mutex_unlock(&g_switch.lock);
            return 0;
        }

        // 最终PlanSync确认成功，清除遗留的后台修复状态。
        policy->sync_repair_pending  = false;
        policy->sync_repair_id       = 0U;
        policy->sync_repair_after_us = 0U;

        // 提交迁移历史，结束当前Operation事务。
        _linkg_switch_operation_finish_locked(peer, now_us);

        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 进入冗余ACK确认 ******************************/

    // 成功ACK只有在WAIT_ENTER_ACK阶段才需要启动后续验证或计时。
    if (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 当前本地Plan必须仍然符合进入冗余的预期。
    if (!_linkg_switch_operation_group_plan_matches_locked(peer, false))
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 进入冗余已得到可靠确认，清除遗留的后台修复状态。
    policy->sync_repair_pending  = false;
    policy->sync_repair_id       = 0U;
    policy->sync_repair_after_us = 0U;

    mask = policy->group_mask;

    /****************************** LOAD迁移计时 ******************************/

    if (policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOAD)
    {
        // 冗余ACK确认后开始1秒迁移计时。
        policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_LOAD_DURATION_US);
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER, now_us);

        // 记录本组参与业务的计时起点和截止时间。
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
            {
                continue;
            }

            policy->operations[index].started_us  = now_us;
            policy->operations[index].deadline_us = policy->next_action_us;
        }
    }
    /****************************** LOSS及RECOVERY质量验证 ******************************/
    else if (policy->group_reason == LINKG_SWITCH_POLICY_REASON_LOSS ||
             policy->group_reason == LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        /****************************** 固化参与业务及原始迁出原因 ******************************/

        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
            {
                continue;
            }

            operation_ids[index] = policy->operations[index].operation_id;

            if (policy->group_reason != LINKG_SWITCH_POLICY_REASON_RECOVERY)
            {
                continue;
            }

            // Cellular已按原因生成独立候选组；这里以第一个参与业务为本组基线。
            if (recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_NONE)
            {
                recovery_reason = policy->operations[index].migration_reason;
            }
            else if (recovery_reason != policy->operations[index].migration_reason)
            {
                // 外部原因发生变化时不允许混合判决，Verify安全失败后回到Cellular。
                recovery_reason = LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN;
            }
        }

        // 冗余ACK确认后启动3秒验证；LOAD恢复使用队列溢出，LOSS沿用积分。
        ret = linkg_switch_policy_verify_begin(&peer->role.sta.verify, peer->peer_node_id, peer->generation,
                                               operation_ids, mask, policy->group_reason, recovery_reason,
                                               &peer->role.sta.observation, now_us);

        // 无论Verify是否启动成功，都进入固定3秒验证窗口。
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY, now_us);
        policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_POLICY_VERIFY_DURATION_US);

        // 记录本组参与业务的验证起点和截止时间。
        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if ((mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
            {
                continue;
            }

            policy->operations[index].started_us  = now_us;
            policy->operations[index].deadline_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_POLICY_VERIFY_DURATION_US);
        }

        // Verify启动失败时，由Operation在3秒后输出INCONCLUSIVE。
        (void)ret;
    }

    pthread_mutex_unlock(&g_switch.lock);
    return 0;
}


/****************************** 后台同步修复 ******************************/


/**
 * @brief 处理快速重同步耗尽后的低频完整PlanSync修复。
 *
 * 仅在没有活动Operation事务、修复时间到期且PlanSync空闲时执行。
 * 不修改本地发送计划，不占用业务切换事务组。
 *
 * 调用方不得持有g_switch.lock。
 */
static int _linkg_switch_operation_process_repair(uint64_t now_us)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_send_plan_t              previous;
    uint32_t                       generation;
    uint32_t                       sync_id;
    uint32_t                       index;
    uint8_t                        node_id;
    bool                           due;
    int                            ret;

    due        = false;
    node_id    = 0U;
    generation = 0U;

    /****************************** 查找待修复事务 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    // 后台修复仅在运行中的STA侧执行。
    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // STA只有一个直接Peer，找到后只检查该Peer。
    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        policy = &peer->role.sta.policy;

        // 活动事务未结束或没有待修复任务，不执行后台修复。
        if (policy->group_mask != 0U || !policy->sync_repair_pending)
        {
            break;
        }

        // 修复尚未安排或尚未达到截止时间。
        if (policy->sync_repair_after_us == 0U || now_us < policy->sync_repair_after_us)
        {
            break;
        }

        // 当前PlanSync仍在进行，不能启动新的后台修复。
        if (peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
            peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT)
        {
            break;
        }

        // 保存Peer身份和完整Plan，在Switch锁外执行条件提交。
        node_id    = peer->peer_node_id;
        generation = peer->generation;
        previous   = peer->plan;
        due        = true;

        // 预先安排下一次低频修复，避免本次失败后立即重复提交。
        policy->sync_repair_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_REPAIR_INTERVAL_US);

        break;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (!due)
    {
        return 0;
    }

    /****************************** 提交完整PlanSync ******************************/

    // 保持本地Plan不变，只重新向AP同步当前完整计划。
    sync_id = 0U;

    ret = linkg_switch_plan_submit_if_current(node_id, generation, &previous, &previous, now_us, &sync_id);
    if (ret != 0)
    {
        // Plan已变化、资源暂时不可用或Peer失效，本轮放弃。
        if (ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE)
        {
            return 0;
        }

        return ret;
    }

    /****************************** 关联后台修复事务 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(node_id, generation);

    // 仅当当前仍为同一个Peer、无活动组且修复任务有效时记录sync_id。
    if (peer != NULL &&
        peer->role.sta.policy.group_mask == 0U &&
        peer->role.sta.policy.sync_repair_pending &&
        peer->role.sta.plan_sync.sync_id == sync_id)
    {
        peer->role.sta.policy.sync_repair_id = sync_id;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}


/****************************** 周期处理 ******************************/

/**
 * @brief 周期保存最近一次有效的Wi-Fi RSSI，供断链后的恢复判断使用。
 *
 * 仅在Observation、Wi-Fi链路及无线统计均有效时更新历史RSSI。
 * 调用方必须持有g_switch.lock。
 */
static void _linkg_switch_operation_sample_wifi_locked(linkg_switch_peer_runtime_t *peer)
{
    const linkg_switch_observation_t *observation;
    linkg_switch_policy_runtime_t    *policy;

    observation = &peer->role.sta.observation;

    // Observation必须有效。
    if (!observation->valid)
    {
        return;
    }

    // Wi-Fi必须处于可用状态。
    if (observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE)
    {
        return;
    }

    // 无线统计无效时，保留上一次有效RSSI。
    if (!observation->wifi.radio.statistics_valid)
    {
        return;
    }

    // 更新最近一次有效的Wi-Fi RSSI。
    policy = &peer->role.sta.policy;
    policy->last_wifi_rssi_dbm   = observation->wifi.radio.rssi_dbm;
    policy->last_wifi_rssi_valid = true;
}


/**
 * @brief 识别Reconcile或外部Plan修改导致的Wi-Fi到Cellular迁移。
 *
 * 正常Policy迁移历史由最终EXIT ACK确认。
 * 本函数仅在没有活动Operation事务时，补充未记录的外部迁移历史。
 * 每轮均更新各业务Primary Link基线，避免重复识别同一次迁移。
 *
 * 调用方必须持有g_switch.lock。
 */
static void _linkg_switch_operation_track_external_locked(linkg_switch_peer_runtime_t *peer, uint64_t now_us)
{
    const linkg_switch_observation_t *observation;
    const linkg_class_send_plan_t    *current;
    linkg_switch_policy_runtime_t    *policy;
    linkg_switch_policy_operation_t  *operation;
    uint32_t                          cellular_link_id;
    uint32_t                          previous_primary_link_id;
    uint32_t                          index;
    bool                              track_external;

    observation      = &peer->role.sta.observation;
    policy           = &peer->role.sta.policy;
    cellular_link_id = observation->cellular.link_id;

    /****************************** 更新Wi-Fi Link历史 ******************************/

    // 仅在Wi-Fi可用且Link ID有效时更新最近一次链路实例。
    if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE && observation->wifi.link_id != LINKG_LINK_ID_INVALID)
    {
        policy->last_wifi_link_id = observation->wifi.link_id;
    }

    /****************************** 外部迁移检查条件 ******************************/

    // 仅在没有活动事务、已有Plan基线且两条链路ID有效时识别外部迁移。
    track_external = policy->group_mask == 0U &&
                     policy->observed_plan_valid &&
                     policy->last_wifi_link_id != LINKG_LINK_ID_INVALID &&
                     cellular_link_id != LINKG_LINK_ID_INVALID;

    /****************************** 检查三类业务迁移 ******************************/

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current   = &peer->plan.classes[index];
        operation = &policy->operations[index];

        // 保存上一轮Primary，并始终更新本轮Plan观测基线。
        previous_primary_link_id = policy->observed_primary_link_ids[index];
        policy->observed_primary_link_ids[index] = current->primary_link_id;

        // 不满足外部迁移检查条件，跳过历史记录。
        if (!track_external)
        {
            continue;
        }

        // 上轮业务必须以Wi-Fi为主链路。
        if (previous_primary_link_id != policy->last_wifi_link_id)
        {
            continue;
        }

        // 当前业务必须已经变成Cellular单发。
        if (current->mode != LINKG_SEND_MODE_SINGLE ||
            current->primary_link_id != cellular_link_id)
        {
            continue;
        }

        // 已有迁移原因的业务不重复记录UNKNOWN历史。
        if (operation->migration_reason != LINKG_SWITCH_POLICY_MIGRATION_NONE)
        {
            continue;
        }

        /****************************** 记录外部迁移历史 ******************************/

        // 无Operation确认的迁移只能标记UNKNOWN。
        operation->migration_reason = LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN;

        // 保存迁移时刻及最近一次有效Wi-Fi RSSI。
        operation->recovery.migrated_us       = now_us;
        operation->recovery.rssi_valid        = policy->last_wifi_rssi_valid;
        operation->recovery.rssi_dbm          = policy->last_wifi_rssi_dbm;
        operation->recovery.recovery_after_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RECOVERY_HOLDOFF_US);
    }

    /****************************** 完成Plan基线更新 ******************************/

    // 首次执行仅建立基线，后续执行才允许识别主链路变化。
    policy->observed_plan_valid = true;
}



/**
 * @brief 在Switch Worker中推进当前Operation切换事务。
 *
 * 处理后台PlanSync修复、冗余质量验证、LOAD计时及有限协议重同步。
 * 外部Plan覆盖当前事务时，清理旧Operation，禁止旧结果修改新Plan。
 *
 * 调用方不得持有g_switch.lock。
 */
int linkg_switch_policy_operation_process(uint64_t now_us)
{
    linkg_switch_peer_runtime_t   *peer;
    linkg_switch_policy_runtime_t *policy;
    linkg_switch_policy_event_t    event;
    linkg_send_plan_t              previous;
    uint32_t                       peer_generation;
    uint32_t                       sync_id;
    uint32_t                       index;
    uint8_t                        peer_node_id;
    bool                           event_ready;
    bool                           resync;
    int                            ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    /****************************** 后台同步修复 ******************************/

    // 优先处理不占用活动事务组的低频PlanSync修复。
    ret = _linkg_switch_operation_process_repair(now_us);
    if (ret != 0)
    {
        return ret;
    }

    memset(&event, 0, sizeof(event));

    event_ready     = false;
    resync          = false;
    peer_node_id    = 0U;
    peer_generation = 0U;

    /****************************** 获取当前Peer ******************************/

    pthread_mutex_lock(&g_switch.lock);

    // Operation仅在运行中的STA执行。
    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // STA只存在一个直接Peer。
    peer = NULL;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        peer = &g_switch.peers[index];
        break;
    }

    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 更新跨事务状态 ******************************/

    // 保存最近有效Wi-Fi RSSI，并识别外部Plan引起的主链路迁移。
    _linkg_switch_operation_sample_wifi_locked(peer);
    _linkg_switch_operation_track_external_locked(peer, now_us);

    policy = &peer->role.sta.policy;

    // 没有活动事务组，不需要推进当前Operation。
    if (policy->group_mask == 0U)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    peer_node_id    = peer->peer_node_id;
    peer_generation = peer->generation;

    /****************************** 事务有效性检查 ******************************/

    // 当前PlanSync已被其他事务替换，清理旧Operation。
    // UNCERTAIN阶段允许暂时存在不同步的sync_id。
    if (policy->group_phase != LINKG_SWITCH_POLICY_PHASE_UNCERTAIN &&
        policy->plan_sync_id != peer->role.sta.plan_sync.sync_id)
    {
        _linkg_switch_operation_clear_group_locked(peer);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 验证或LOAD计时期间，要求当前Plan仍与本组冗余计划一致。
    if (policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY ||
        policy->group_phase == LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER)
    {
        if (!_linkg_switch_operation_group_plan_matches_locked(peer, false))
        {
            _linkg_switch_operation_clear_group_locked(peer);
            pthread_mutex_unlock(&g_switch.lock);
            return 0;
        }
    }

    /****************************** 推进当前事务阶段 ******************************/

    switch (policy->group_phase)
    {
        case LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY:
        {
            // Verify正常运行，持续采集质量数据并检查验证截止时间。
            if (peer->role.sta.verify.active)
            {
                ret = linkg_switch_policy_verify_process(&peer->role.sta.verify, &peer->role.sta.observation, now_us, &event, &event_ready);
                if (ret != 0)
                {
                    pthread_mutex_unlock(&g_switch.lock);
                    return ret;
                }
                break;
            }

            // Verify未启动时，仍等待完整3秒窗口后安全回退。
            if (now_us < policy->next_action_us)
            {
                break;
            }

            // 使用本组第一个参与业务的Operation ID标识验证结果。
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
            break;
        }

        case LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER:
        {
            // LOAD进入冗余后的1秒计时尚未到期。
            if (now_us < policy->next_action_us)
            {
                break;
            }

            // 使用本组第一个参与业务的Operation ID标识超时事件。
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
            break;
        }

        case LINKG_SWITCH_POLICY_PHASE_UNCERTAIN:
        {
            // 追加重同步必须达到截止时间，且不能超过最大次数。
            if (now_us < policy->next_action_us || policy->recovery_count >= LINKG_SWITCH_OPERATION_RESYNC_MAX)
            {
                break;
            }

            // 保存当前完整Plan，稍后在Switch锁外重新提交同步。
            previous = peer->plan;
            policy->next_action_us = _linkg_switch_operation_add_us(now_us, LINKG_SWITCH_OPERATION_RESYNC_INTERVAL_US);
            resync = true;
            break;
        }

        default:
        {
            // WAIT_ENTER_ACK、WAIT_EXIT_ACK等阶段由PlanSync结果驱动。
            break;
        }
    }

    pthread_mutex_unlock(&g_switch.lock);

    /****************************** 交付事务事件 ******************************/

    // Verify完成或LOAD计时到期，交给Operation事件入口执行最终Plan构建。
    if (event_ready)
    {
        return linkg_switch_policy_operation_process_event(&event, now_us);
    }

    // 本轮无需追加PlanSync。
    if (!resync)
    {
        return 0;
    }

    /****************************** 追加完整PlanSync ******************************/

    // 不修改本地Plan，仅重新同步当前完整三业务计划。
    sync_id = 0U;

    ret = linkg_switch_plan_submit_if_current(peer_node_id, peer_generation, &previous, &previous, now_us, &sync_id);
    if (ret != 0)
    {
        // Plan变化、资源阻止或Peer失效，本轮放弃重新同步。
        if (ret == -EAGAIN || ret == -EBUSY || ret == -ESTALE)
        {
            return 0;
        }

        return ret;
    }

    /****************************** 更新重同步事务 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);

    // 只有当前事务仍处于UNCERTAIN且sync_id匹配，才推进事务阶段。
    if (peer != NULL &&
        peer->role.sta.policy.group_phase == LINKG_SWITCH_POLICY_PHASE_UNCERTAIN &&
        peer->role.sta.plan_sync.sync_id == sync_id)
    {
        policy = &peer->role.sta.policy;

        policy->plan_sync_id = sync_id;
        policy->recovery_count++;

        // 本地已处于最终单发Plan，等待重新同步后的AP确认。
        _linkg_switch_operation_set_phase_locked(peer, LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK, now_us);
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}


/**
 * @brief 获取当前Operation最早需要处理的截止时间。
 *
 * 无活动事务时检查后台PlanSync修复时间。
 * 有活动事务时根据阶段检查Verify、LOAD计时或协议重同步时间。
 * 调用方必须持有g_switch.lock。
 *
 * @return 下一次截止时间，UINT64_MAX表示没有待调度的截止时间。
 */
uint64_t linkg_switch_policy_operation_next_deadline_locked(void)
{
    const linkg_switch_policy_runtime_t *policy;
    const linkg_switch_peer_runtime_t   *peer;
    uint64_t                             now_us;
    uint64_t                             deadline;
    uint64_t                             candidate_us;
    uint32_t                             index;

    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        return UINT64_MAX;
    }

    now_us   = linkg_time_monotonic_us();
    deadline = UINT64_MAX;

    /****************************** 遍历Peer ******************************/

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        policy       = &peer->role.sta.policy;
        candidate_us = 0U;

        /****************************** 后台同步修复 ******************************/

        if (policy->group_mask == 0U)
        {
            // 没有活动事务时，只检查待执行的后台PlanSync修复。
            if (!policy->sync_repair_pending || policy->sync_repair_after_us == 0U)
            {
                continue;
            }

            // 当前PlanSync仍在进行时，暂不调度新的修复。
            if (peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
                peer->role.sta.plan_sync.state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT)
            {
                continue;
            }

            candidate_us = policy->sync_repair_after_us;
        }
        else
        {
            /****************************** 活动事务计时 ******************************/

            switch (policy->group_phase)
            {
                case LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY:
                    // Verify已产生待交付结果，不再安排新的验证截止时间。
                    if (peer->role.sta.verify.result_pending && peer->role.sta.verify.active)
                    {
                        continue;
                    }

                    // Verify运行时使用其自身的截止时间。
                    if (peer->role.sta.verify.active)
                    {
                        candidate_us = peer->role.sta.verify.deadline_us;
                    }
                    else
                    {
                        // Verify未启动时使用Operation的后备截止时间。
                        candidate_us = policy->next_action_us;
                    }
                    break;

                case LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER:
                    // LOAD进入冗余后的1秒迁移计时。
                    candidate_us = policy->next_action_us;
                    break;

                case LINKG_SWITCH_POLICY_PHASE_UNCERTAIN:
                    // 仅在追加重同步次数尚未耗尽时调度。
                    if (policy->recovery_count >= LINKG_SWITCH_OPERATION_RESYNC_MAX)
                    {
                        continue;
                    }

                    candidate_us = policy->next_action_us;
                    break;

                default:
                    // 其他阶段不需要由Operation提供独立计时截止时间。
                    continue;
            }
        }

        /****************************** 更新最早截止时间 ******************************/

        // 仅选择尚未到期且早于当前最早截止时间的候选时间。
        if (candidate_us > now_us && candidate_us < deadline)
        {
            deadline = candidate_us;
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

    // 仅清理匹配当前Peer代际的事务，防止旧生命周期影响新Peer。
    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer != NULL)
    {
        _linkg_switch_operation_clear_group_locked(peer);
    }
}
