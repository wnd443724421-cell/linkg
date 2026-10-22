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

#define LINKG_SWITCH_POLICY_OPERATION_MAX LINKG_TRANSPORT_CLASS_COUNT // 一个Peer的独立业务事务槽位数量

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
    LINKG_SWITCH_POLICY_PHASE_IDLE = 0,       // 当前业务没有活动事务
    LINKG_SWITCH_POLICY_PHASE_PENDING_ENTER,  // 已创建进入冗余事务，等待提交Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_ENTER_ACK, // 本地冗余生效，等待AP确认
    LINKG_SWITCH_POLICY_PHASE_WAIT_VERIFY,    // 冗余已确认，等待质量验证结果
    LINKG_SWITCH_POLICY_PHASE_WAIT_TIMER,     // 冗余已确认，等待负载迁移计时到期
    LINKG_SWITCH_POLICY_PHASE_PENDING_EXIT,   // 已决定退出冗余，等待提交单发Plan
    LINKG_SWITCH_POLICY_PHASE_WAIT_EXIT_ACK,  // 本地单发生效，等待AP确认
    LINKG_SWITCH_POLICY_PHASE_UNCERTAIN       // 同步终态不确定，等待有限重同步
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
    LINKG_SWITCH_POLICY_MIGRATION_NONE = 0,    // 没有已记录的迁移历史
    LINKG_SWITCH_POLICY_MIGRATION_LOSS,        // 因Wi-Fi无线丢包迁出
    LINKG_SWITCH_POLICY_MIGRATION_LOAD,        // 因Wi-Fi队列溢出迁出
    LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN,   // 因Wi-Fi硬故障迁出
    LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN      // 已驻留Cellular，但迁移原因未知
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
 * @brief 每类业务独立保存逻辑事务标识、当前阶段和迁移恢复历史。
 *
 * phase为IDLE时operation_id为0，其余阶段operation_id必须非0。
 * 事务结束只清除当前事务字段，保留迁移历史与Wi-Fi恢复基线。
 * 单次整组PlanSync使用Peer级plan_sync_id关联，各业务并不独立发送PlanSync。
 */
typedef struct
{
    linkg_switch_policy_phase_t            phase;            // 当前业务的事务阶段
    uint32_t                               operation_id;     // 当前业务事务编号，0表示空闲
    linkg_switch_policy_reason_t           reason;           // 当前事务触发原因
    linkg_switch_policy_target_t           exit_target;      // 当前业务的最终单发目标
    linkg_switch_policy_migration_reason_t migration_reason; // 最近一次完成迁出Wi-Fi的原因

    uint64_t                               phase_entered_us; // 当前阶段开始时间
    uint64_t                               started_us;       // 冗余ACK确认后开始验证或计时的时间
    uint64_t                               deadline_us;      // 当前阶段截止时间，0表示无截止

    linkg_switch_policy_recovery_t         recovery;         // 按业务保存的迁移历史及恢复基线
} linkg_switch_policy_operation_t;

/****************************** Peer级策略运行状态 ******************************/

/**
 * @brief 每个Peer的三个独立业务事务及唯一组级PlanSync事务。
 *
 * group_mask非0表示当前有活动事务组；组内业务使用独立Operation ID。
 * group_phase/group_reason/group_target和plan_sync_id描述同一完整Plan的同步过程。
 * last_wifi_*和observed_*是跨事务的历史信息，清除活动组时不得清空。
 * Verify运行状态独立存放在linkg_switch_sta_peer_runtime_t.verify中。
 */
typedef struct
{
    linkg_switch_policy_operation_t operations[LINKG_SWITCH_POLICY_OPERATION_MAX];  // 三类业务独立事务槽位
    uint32_t                        next_operation_id;                              // 当前Peer的事务编号分配基线
    uint32_t                        plan_sync_id;                                   // 活动组对应的完整PlanSync逻辑编号

    uint32_t                        group_mask;                                     // 当前事务组包含的业务集合，0表示无活动组
    linkg_switch_policy_reason_t    group_reason;                                   // 当前组LOSS/LOAD/RECOVERY/WIFI_DOWN触发原因
    linkg_switch_policy_phase_t     group_phase;                                    // 当前组统一的事务阶段
    linkg_switch_policy_target_t    group_target;                                   // 退出冗余后的最终单发目标
    uint64_t                        next_action_us;                                 // LOAD到期或同步恢复的下一动作时间
    uint8_t                         recovery_count;                                 // 当前组的追加重同步次数

    int32_t                         group_rssi_dbm;                                 // 事务建立时的Wi-Fi RSSI基线
    bool                            group_rssi_valid;                               // 当前组RSSI基线是否有效
    uint32_t                        group_wifi_link_id;                             // 创建事务组时的Wi-Fi Link实例
    uint32_t                        group_cellular_link_id;                         // 创建事务组时的Cellular Link实例

    int32_t                         last_wifi_rssi_dbm;                             // 最近一次有效Wi-Fi RSSI，供物理断开后的恢复判断
    bool                            last_wifi_rssi_valid;                           // 历史Wi-Fi RSSI是否有效
    uint32_t                        last_wifi_link_id;                              // 最近一次有效Wi-Fi Link实例
    uint32_t                        observed_primary_link_ids[LINKG_TRANSPORT_CLASS_COUNT]; // 上轮各业务主链路，用于识别外部迁移
    bool                            observed_plan_valid;                           // 是否已经记录首次Plan基线
} linkg_switch_policy_runtime_t;

#endif // SWITCH_POLICY_STATE_H
