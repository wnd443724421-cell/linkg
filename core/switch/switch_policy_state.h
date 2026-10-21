
/**
 * @file switch_policy_state.h
 * @brief LinkG切换策略事务运行状态
 */

#ifndef SWITCH_POLICY_STATE_H
#define SWITCH_POLICY_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_transport_types.h"

/****************************** 事务常量 ******************************/

#define LINKG_SWITCH_POLICY_OPERATION_MAX LINKG_TRANSPORT_CLASS_COUNT

/****************************** 当前事务原因 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_REASON_NONE = 0, // 无事务
    LINKG_SWITCH_POLICY_REASON_LOSS,     // 丢包异常，进入冗余质量验证
    LINKG_SWITCH_POLICY_REASON_LOAD,     // 负载溢出，进入冗余定时迁移
    LINKG_SWITCH_POLICY_REASON_RECOVERY, // 5G业务尝试恢复Wi-Fi
    LINKG_SWITCH_POLICY_REASON_WIFI_DOWN // Wi-Fi硬故障，直接迁移到5G
} linkg_switch_policy_reason_t;

/****************************** 事务阶段 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_PHASE_IDLE = 0,       // Wi-Fi正常单发，不存在事务
    LINKG_SWITCH_POLICY_PHASE_PENDING_ENTER,  // 等待提交进入冗余的Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK, // 已提交冗余Plan，等待ACK
    LINKG_SWITCH_POLICY_PHASE_ACTIVE,         // 冗余已确认，等待计时或验证结果
    LINKG_SWITCH_POLICY_PHASE_EXIT_READY,     // 等待提交最终单链路Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK,  // 已提交最终Plan，等待ACK
    LINKG_SWITCH_POLICY_PHASE_WAIT_RECOVERY,  // 已驻留5G，等待恢复条件
    LINKG_SWITCH_POLICY_PHASE_UNCERTAIN       // Plan同步不确定，等待协调恢复
} linkg_switch_policy_phase_t;

/****************************** 事务最终目标 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_TARGET_NONE = 0, // 尚未确定最终链路
    LINKG_SWITCH_POLICY_TARGET_WIFI,     // 最终使用Wi-Fi单发
    LINKG_SWITCH_POLICY_TARGET_CELLULAR  // 最终使用5G单发
} linkg_switch_policy_target_t;

/****************************** 历史迁移原因 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_MIGRATION_NONE = 0,     // 尚无5G迁移历史
    LINKG_SWITCH_POLICY_MIGRATION_LOSS,         // 因Wi-Fi丢包迁移
    LINKG_SWITCH_POLICY_MIGRATION_LOAD,         // 因Wi-Fi负载溢出迁移
    LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN,    // 因Wi-Fi硬故障迁移
    LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN       // 已在5G，但迁移原因未知
} linkg_switch_policy_migration_reason_t;

/****************************** Wi-Fi恢复基线 ******************************/

typedef struct
{
    int32_t  rssi_dbm;          // 离开Wi-Fi时保存的RSSI
    int32_t  snr_db;            // 离开Wi-Fi时保存的SNR

    uint64_t migrated_us;       // 确认迁移到5G的时间
    uint64_t recovery_after_us; // 下一次允许尝试恢复的时间

    bool     rssi_valid;        // RSSI基线有效
    bool     snr_valid;         // SNR基线有效
} linkg_switch_policy_recovery_t;

/****************************** 单业务逻辑事务 ******************************/

typedef struct
{
    uint32_t                               operation_id;     // 当前事务编号，0表示空闲
    uint32_t                               plan_message_id;  // 当前等待确认的Plan消息编号，0表示无

    linkg_switch_policy_reason_t           reason;           // 当前事务执行原因
    linkg_switch_policy_phase_t            phase;            // 当前事务阶段
    linkg_switch_policy_target_t           exit_target;      // 本次操作的最终单链路目标

    linkg_switch_policy_migration_reason_t migration_reason; // 原始迁出Wi-Fi的原因

    uint64_t                               phase_entered_us; // 当前阶段进入时间
    uint64_t                               started_us;       // 本次进入冗余获得ACK的时间
    uint64_t                               deadline_us;      // 本次计时或验证的截止时间

    linkg_switch_policy_recovery_t         recovery;         // 当前业务的Wi-Fi恢复基线
} linkg_switch_policy_operation_t;

/****************************** Peer策略运行状态 ******************************/

typedef struct
{
    linkg_switch_policy_operation_t operations[LINKG_SWITCH_POLICY_OPERATION_MAX]; // 三类业务各自的事务槽位
    uint32_t                        next_operation_id;                             // 下一事务编号分配基线
    uint32_t                        plan_sync_id;                                  // 当前Operation等待的完整Plan逻辑同步编号
} linkg_switch_policy_runtime_t;

#endif
