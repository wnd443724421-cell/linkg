/**
 * @file switch_policy_verify.h
 * @brief LinkG冗余阶段Wi-Fi恢复积分及双链路质量验证接口
 */

#ifndef SWITCH_POLICY_VERIFY_H
#define SWITCH_POLICY_VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "switch_observation.h"
#include "switch_policy_event.h"
#include "switch_policy_state.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 验证参数 ******************************/

#define LINKG_SWITCH_POLICY_VERIFY_DURATION_US            3000000ULL // 冗余ACK后固定验证3秒
#define LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US           250000U    // Cellular RTT必须严格小于250ms
#define LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US       1500000ULL // 截止时成功Probe最大年龄
#define LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US 1000000ULL // 截止时观测快照最大年龄
#define LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES       20U        // 单方向单轮积分最小样本数
#define LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES    100U       // 双链路质量比较最小配对样本数
#define LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE            5U         // LOSS验证开始时的Wi-Fi积分
#define LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE          3U         // 积分严格小于3确认Wi-Fi恢复

/****************************** 流量方向 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_UPLINK = 0,     // STA到AP
    LINKG_SWITCH_POLICY_VERIFY_DOWNLINK,       // AP到STA
    LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT // 流量方向数量
} linkg_switch_policy_verify_direction_t;

/****************************** 单链路验证样本 ******************************/

/**
 * @brief 保存单条物理链路在验证期间的Wire与队列溢出统计。
 *
 * Wire按业务类别和上下行独立累计，避免不同业务及方向错配。
 * Queue Overflow为整链路双方向统计，不计入Wire丢包数量。
 * LOSS验证的首个有效窗口仅建立采样边界；LOAD恢复允许消费首个有效Load窗口。
 */
typedef struct
{
    uint64_t lost_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];   // 各业务方向Wire确认丢失数
    uint64_t sample_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT]; // 各业务方向Wire样本总数

    uint64_t last_loss_us[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];   // 各业务方向最后消费的Wire窗口时间
    uint64_t last_load_us[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                                // 各方向最后消费的Load窗口时间

    bool     loss_primed[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];    // 各业务方向是否已建立Wire采样边界
    bool     load_primed[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                                 // 各方向是否已建立Load采样边界
    bool     load_observed[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                               // 各方向是否已取得有效Load窗口
    bool     queue_overflow;                                                                          // 验证窗口内是否出现TX队列溢出
} linkg_switch_policy_verify_path_t;

/****************************** Peer级验证会话 ******************************/

/**
 * @brief 一个Peer同一时刻最多维护一组LOSS或RECOVERY验证会话。
 *
 * 由Operation在冗余PlanSync ACK后创建，并在事务退出或取消时清理。
 * operation_ids保存参与业务的原始事务身份，非参与业务的ID必须为0。
 * 结果形成后通过result_pending锁存，允许重复交付相同VERIFY_DONE事件。
 */
typedef struct
{
    /****************************** 事务身份 ******************************/

    uint8_t                                peer_node_id;                                             // 验证所属Peer节点ID
    uint32_t                               peer_generation;                                          // Peer运行代际
    uint32_t                               operation_id;                                             // VERIFY_DONE使用的代表事务ID
    uint32_t                               operation_ids[LINKG_TRANSPORT_CLASS_COUNT];               // 各参与业务的Operation ID
    uint32_t                               class_mask;                                               // 本次验证参与的业务集合
    linkg_switch_policy_reason_t           reason;                                                   // LOSS或RECOVERY
    linkg_switch_policy_migration_reason_t recovery_reason;                                          // RECOVERY对应的原始迁移原因

    /****************************** 链路与时间 ******************************/

    uint32_t                               wifi_link_id;                                             // 启动时Wi-Fi Link实例
    uint32_t                               cellular_link_id;                                         // 启动时Cellular Link实例
    uint64_t                               started_us;                                               // 冗余PlanSync ACK确认时刻
    uint64_t                               deadline_us;                                              // 固定三秒验证截止时刻
    uint64_t                               finished_us;                                              // 最终结果形成时间

    /****************************** Cellular Probe ******************************/

    uint64_t                               cellular_last_probe_us[LINKG_TRANSPORT_CLASS_COUNT];      // 各业务最后消费的Cellular Probe时间
    uint64_t                               cellular_last_good_probe_us[LINKG_TRANSPORT_CLASS_COUNT]; // 各业务最近成功的Cellular Probe时间
    uint32_t                               cellular_probe_seen_mask;                                 // 验证期内成功Probe的业务集合
    uint32_t                               cellular_max_rtt_us;                                      // 验证期间最大有效Cellular RTT
    bool                                   cellular_probe_failed;                                    // 验证期间发生Probe失败或RTT超限

    /****************************** 质量与负载状态 ******************************/

    uint8_t                                wifi_loss_score;                                          // Wi-Fi恢复积分，LOSS初始为5
    bool                                   path_interrupted;                                         // 物理Path失效或Link实例发生变化
    bool                                   wifi_traffic_seen;                                        // LOAD恢复期间已采集Wi-Fi流量观测
    bool                                   wifi_tx_active;                                           // LOAD恢复期间Wi-Fi存在发送活动
    bool                                   wifi_rx_active;                                           // LOAD恢复期间Wi-Fi存在接收活动

    /****************************** 生命周期与结果 ******************************/

    bool                                   active;                                                   // 验证会话已启动
    bool                                   result_pending;                                           // 已固定的验证结果等待消费
    linkg_switch_policy_verify_result_t    result;                                                   // 已固定的最终验证结果

    /****************************** 双链路采样 ******************************/

    linkg_switch_policy_verify_path_t      wifi;                                                     // Wi-Fi验证样本
    linkg_switch_policy_verify_path_t      cellular;                                                 // Cellular验证样本
} linkg_switch_policy_verify_runtime_t;

/****************************** 验证启动参数 ******************************/

/**
 * @brief Operation在冗余PlanSync ACK后提交的验证启动输入。
 *
 * observation仅在begin调用期间有效，Verify不保留该指针。
 */
typedef struct
{
    uint8_t                                peer_node_id;                                // 当前Peer节点ID
    uint32_t                               peer_generation;                             // Peer运行代际
    uint32_t                               operation_ids[LINKG_TRANSPORT_CLASS_COUNT];  // 各参与业务的Operation ID
    uint32_t                               class_mask;                                  // 本次参与验证的业务集合
    linkg_switch_policy_reason_t           reason;                                      // LOSS或RECOVERY
    linkg_switch_policy_migration_reason_t recovery_reason;                             // RECOVERY原始迁移原因
    const linkg_switch_observation_t      *observation;                                 // 启动时的完整链路观测
    uint64_t                               now_us;                                      // 冗余ACK确认的单调时间
} linkg_switch_policy_verify_begin_params_t;

/****************************** 验证处理参数 ******************************/

/**
 * @brief Switch Worker每轮交付给Verify的观测输入。
 */
typedef struct
{
    const linkg_switch_observation_t *observation; // 本轮最新双链路观测
    uint64_t                          now_us;      // 本轮单调时间
} linkg_switch_policy_verify_process_params_t;

/****************************** 验证处理输出 ******************************/

/**
 * @brief 单轮Verify处理后交付给Operation的事件。
 *
 * event_ready为false时event内容不应被消费。
 * 验证结果一旦形成，应重复返回同一VERIFY_DONE，直至事务确认处理。
 */
typedef struct
{
    linkg_switch_policy_event_t event;       // 固定验证结果对应的事件
    bool                        event_ready; // 是否存在待交付事件
} linkg_switch_policy_verify_process_output_t;

/****************************** 验证生命周期 ******************************/

/**
 * @brief 清理当前验证会话，不负责变更Operation或发送计划。
 */
void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime);

/**
 * @brief 冗余PlanSync ACK确认后创建一次三秒验证会话。
 */
int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_begin_params_t *params);

/**
 * @brief 消费新观测，并在验证结束后输出可重复交付的VERIFY_DONE事件。
 */
int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_process_params_t *params, linkg_switch_policy_verify_process_output_t *output);

#ifdef __cplusplus
}
#endif

#endif // SWITCH_POLICY_VERIFY_H
