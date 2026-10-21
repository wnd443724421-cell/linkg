/**
 * @file switch_policy_state.h
 * @brief LinkG业务链路分配策略运行状态定义
 */

#ifndef SWITCH_POLICY_STATE_H
#define SWITCH_POLICY_STATE_H

#include <stdbool.h>
#include <stdint.h>

#include "switch_policy_event.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_POLICY_OPERATION_MAX LINKG_TRANSPORT_CLASS_COUNT // 单Peer最大活动事务数量
#define LINKG_SWITCH_POLICY_MIGRATION_ID_INVALID 0U                   // 无效迁移批次编号

/****************************** 事务原因 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_REASON_NONE = 0, // 无策略事务
    LINKG_SWITCH_POLICY_REASON_LOSS,     // 无线丢包触发的质量验证
    LINKG_SWITCH_POLICY_REASON_LOAD,     // 队列溢出触发的定时迁移
    LINKG_SWITCH_POLICY_REASON_RECOVERY  // Wi-Fi恢复触发的质量验证
} linkg_switch_policy_reason_t;

/****************************** 事务阶段 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_PHASE_IDLE = 0,       // 空闲事务槽位
    LINKG_SWITCH_POLICY_PHASE_PENDING_ENTER,  // 等待提交进入冗余的Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK, // 等待进入冗余的Plan确认
    LINKG_SWITCH_POLICY_PHASE_ACTIVE,         // 正在计时或进行质量验证
    LINKG_SWITCH_POLICY_PHASE_EXIT_READY,     // 已得到结果，等待提交退出冗余的Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK,  // 等待最终Plan确认
    LINKG_SWITCH_POLICY_PHASE_UNCERTAIN       // 远端同步结果不确定，需要协调恢复
} linkg_switch_policy_phase_t;

/****************************** 事务最终目标 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_TARGET_NONE = 0,     // 尚未产生最终目标
    LINKG_SWITCH_POLICY_TARGET_WIFI,         // 最终使用Wi-Fi单发
    LINKG_SWITCH_POLICY_TARGET_CELLULAR      // 最终使用Cellular单发
} linkg_switch_policy_target_t;

/****************************** 活动逻辑事务 ******************************/

typedef struct
{
    uint32_t                                operation_id;    // 当前逻辑事务唯一编号，0表示空闲
    uint32_t                                class_mask;      // 本次事务绑定的业务集合
    linkg_switch_policy_reason_t            reason;          // LOSS、LOAD或RECOVERY
    linkg_switch_policy_phase_t             phase;           // 当前事务阶段
    linkg_switch_policy_target_t            exit_target;     // 事务结束时的目标单链路

    uint64_t                                phase_entered_us;// 当前阶段进入时间
    uint64_t                                started_us;      // 进入冗余获得ACK的时间
    uint64_t                                deadline_us;     // 当前事务下一截止时间
} linkg_switch_policy_operation_t;

/****************************** 5G驻留原因 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_MIGRATION_NONE = 0,     // 无迁移记录
    LINKG_SWITCH_POLICY_MIGRATION_LOSS,         // 因无线丢包离开Wi-Fi
    LINKG_SWITCH_POLICY_MIGRATION_LOAD,         // 因负载溢出离开Wi-Fi
    LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN,    // 因Wi-Fi硬故障离开
    LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN       // 由外部Plan或资源修复形成的5G驻留
} linkg_switch_policy_migration_reason_t;

/****************************** 业务5G驻留记录 ******************************/

typedef struct
{
    linkg_switch_policy_migration_reason_t reason;           // 本次离开Wi-Fi的原因

    uint32_t                               migration_id;     // 本次迁移批次编号
    uint64_t                               migrated_us;      // 迁移发生时间
    uint64_t                               recovery_after_us;// 下次允许恢复尝试的时间

    int32_t                                rssi_dbm;         // 迁移时Wi-Fi RSSI
    int32_t                                snr_db;           // 迁移时Wi-Fi SNR
    bool                                   rssi_valid;       // RSSI基线有效
    bool                                   snr_valid;        // SNR基线有效
    bool                                   valid;            // 当前驻留记录有效
} linkg_switch_policy_residency_t;

/****************************** Peer策略运行状态 ******************************/

typedef struct
{
    linkg_switch_policy_operation_t operations[LINKG_SWITCH_POLICY_OPERATION_MAX]; // 当前活动逻辑事务

    uint32_t class_operation_id[LINKG_TRANSPORT_CLASS_COUNT]; 					   // 每类业务当前所属事务编号
    uint32_t next_operation_id;                               					   // 下一逻辑事务编号分配基线

    linkg_switch_policy_residency_t residency[LINKG_TRANSPORT_CLASS_COUNT]; 	   // 三类业务5G驻留记录
    uint32_t next_migration_id;                                             	   // 下一迁移批次编号分配基线
} linkg_switch_policy_runtime_t;

#endif
