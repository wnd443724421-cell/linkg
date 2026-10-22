
/**
 * @file switch_policy_state.h
 * @brief LinkG三类业务策略事务状态及恢复上下文
 */

#ifndef SWITCH_POLICY_STATE_H
#define SWITCH_POLICY_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_transport_types.h"

/****************************** 业务数量 ******************************/

#define LINKG_SWITCH_POLICY_OPERATION_MAX LINKG_TRANSPORT_CLASS_COUNT

/****************************** 切换事务原因 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_REASON_NONE = 0, // 当前没有切换事务
    LINKG_SWITCH_POLICY_REASON_LOSS,     // Wi-Fi丢包异常，触发冗余质量验证
    LINKG_SWITCH_POLICY_REASON_LOAD,     // Wi-Fi队列溢出，触发冗余定时迁移
    LINKG_SWITCH_POLICY_REASON_RECOVERY, // Cellular业务尝试恢复到Wi-Fi
    LINKG_SWITCH_POLICY_REASON_WIFI_DOWN // Wi-Fi明确硬故障，直接迁移
} linkg_switch_policy_reason_t;

/****************************** 单业务切换事务阶段 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_PHASE_IDLE = 0,        // 当前业务没有活动切换事务

    LINKG_SWITCH_POLICY_PHASE_PENDING_ENTER,   // 已创建进入冗余事务，等待提交Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK,  // 本地冗余已生效，等待AP确认

    LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY,     // 冗余已确认，等待质量比较结果
    LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER,      // 冗余已确认，等待迁移计时到期

    LINKG_SWITCH_POLICY_PHASE_PENDING_EXIT,    // 已决定最终目标，等待提交单发Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK,   // 本地单发已生效，等待AP确认

    LINKG_SWITCH_POLICY_PHASE_UNCERTAIN        // 事务结果不确定，等待协调恢复
} linkg_switch_policy_phase_t;

/****************************** 事务最终单发目标 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_TARGET_NONE = 0, // 尚未确定最终单发目标
    LINKG_SWITCH_POLICY_TARGET_WIFI,     // 最终使用Wi-Fi单发
    LINKG_SWITCH_POLICY_TARGET_CELLULAR  // 最终使用Cellular单发
} linkg_switch_policy_target_t;

/****************************** 最近一次迁出Wi-Fi的原因 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_MIGRATION_NONE = 0,     // 没有已记录的迁移历史
    LINKG_SWITCH_POLICY_MIGRATION_LOSS,         // 因Wi-Fi无线丢包迁出
    LINKG_SWITCH_POLICY_MIGRATION_LOAD,         // 因Wi-Fi队列溢出迁出
    LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN,    // 因Wi-Fi硬故障迁出
    LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN       // 已驻留Cellular，但迁移原因未知
} linkg_switch_policy_migration_reason_t;

/****************************** Wi-Fi恢复基线 ******************************/

typedef struct
{
    int32_t  rssi_dbm;          // 离开Wi-Fi时记录的RSSI
    int32_t  snr_db;            // 离开Wi-Fi时记录的SNR
    uint64_t migrated_us;       // 最近一次确认迁移至Cellular的时间
    uint64_t recovery_after_us; // 下一次允许尝试Wi-Fi恢复的时间
    bool     rssi_valid;        // RSSI迁移基线是否有效
    bool     snr_valid;         // SNR迁移基线是否有效
} linkg_switch_policy_recovery_t;

/****************************** 单业务事务与恢复上下文 ******************************/

/**
 * @brief 每类业务始终拥有一个独立槽位，发送状态由当前Plan决定。
 *
 * phase为IDLE时无活动切换事务，operation_id为0。
 * phase不为IDLE时存在待推进或待协调事务，operation_id必须非0。
 * 事务结束时清除临时事务字段，保留迁移历史与Wi-Fi恢复基线。
 * PlanSync的稳定逻辑sync_id由Peer级policy.plan_sync_id关联。
 */
typedef struct
{
    linkg_switch_policy_phase_t            phase;            // 当前业务唯一切换事务阶段
    uint32_t                               operation_id;     // 当前切换事务编号，0表示没有活动事务
    linkg_switch_policy_reason_t           reason;           // 当前事务的触发原因
    linkg_switch_policy_target_t           exit_target;      // 本轮事务的最终单发目标
    linkg_switch_policy_migration_reason_t migration_reason; // 最近一次完成迁出Wi-Fi的原因

    uint64_t                               phase_entered_us; // 当前事务阶段开始时间
    uint64_t                               started_us;       // 冗余获得ACK并开始验证/计时的时间
    uint64_t                               deadline_us;      // 当前验证或计时的截止时间，0表示未设置

    linkg_switch_policy_recovery_t         recovery;         // 驻留Cellular期间保留的Wi-Fi恢复基线
} linkg_switch_policy_operation_t;

/****************************** Peer级策略运行状态 ******************************/

typedef struct
{
    linkg_switch_policy_operation_t operations[LINKG_SWITCH_POLICY_OPERATION_MAX]; // REALTIME/VIDEO/DATA各自独立的切换事务
    uint32_t                        next_operation_id;                             // 当前Peer内事务编号分配基线
    uint32_t                        plan_sync_id;                                  // 当前完整三业务PlanSync的稳定逻辑编号，0表示无关联
} linkg_switch_policy_runtime_t;

#endif // SWITCH_POLICY_STATE_H
