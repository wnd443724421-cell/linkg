/**
 * @file switch_policy_verify.h
 * @brief LinkG冗余阶段Wi-Fi恢复积分及双链路质量验证
 */

#ifndef SWITCH_POLICY_VERIFY_H
#define SWITCH_POLICY_VERIFY_H

#include <stdbool.h>
#include <stdint.h>

#include "switch_observation.h"
#include "switch_policy_event.h"
#include "switch_policy_state.h"

/****************************** 验证参数 ******************************/

#define LINKG_SWITCH_POLICY_VERIFY_DURATION_US            3000000ULL  // 冗余ACK后固定验证3秒
#define LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US           250000U     // Cellular RTT必须严格小于250ms
#define LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US       1500000ULL  // 截止时成功Probe最大年龄
#define LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US 1000000ULL  // 截止时观测快照最大年龄
#define LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES       20U         // 单方向单轮积分最小样本数
#define LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES    100U        // 每条候选链路最小累计样本数
#define LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE            5U          // 验证开始时积分
#define LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE          3U          // 积分小于3确认Wi-Fi恢复

/****************************** 流量方向 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_UPLINK = 0,      // STA到AP
    LINKG_SWITCH_POLICY_VERIFY_DOWNLINK,        // AP到STA
    LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT  // 流量方向数量
} linkg_switch_policy_verify_direction_t;

/****************************** 单链路样本 ******************************/

/**
 * @brief 按业务和方向分别累计Wire样本，避免不同业务及方向错配。
 *
 * Queue Overflow是整链路统计，不能直接加入各业务Wire丢包数量。
 * 首个ACK后的新差分窗口用于重新建立采样边界，不参与累计。
 */
typedef struct
{
    uint64_t lost_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];    // Wire确认丢失数
    uint64_t sample_packets[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];  // Wire样本总数

    uint64_t last_loss_us[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];    // 各业务及方向已处理的窗口时间戳
    uint64_t last_load_us[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                                 // 各方向整链路Load窗口时间戳

    bool     loss_primed[LINKG_TRANSPORT_CLASS_COUNT][LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];     // 各业务及方向是否跳过首个新窗口
    bool     load_primed[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                                  // 各方向Load是否跳过首个新窗口
    bool     queue_overflow;                                                                           // 窗口内是否出现TX队列溢出
} linkg_switch_policy_verify_path_t;

/****************************** Peer级验证会话 ******************************/

/**
 * @brief 一个Peer同时至多进行一组LOSS或RECOVERY质量验证。
 *
 * Operation负责创建和维护各业务事务，并在冗余PlanSync ACK后启动验证。
 * operation_ids按业务固定保存本组事务身份，非参与业务必须为0。
 * operation_id取参与业务中下标最小的事务ID，用于单个VERIFY_DONE事件。
 *
 * result_pending表示结果已计算完成，但尚未被Operation确认消费。
 * 在调用方明确完成或取消会话前，process会重复提供同一事件，
 * 不重新计算结果；Operation必须按Generation、ID和Phase幂等处理。
 *
 * 结构体由Switch Worker独占，或由调用方在Switch锁下操作。
 */
typedef struct
{
    uint8_t                             peer_node_id;                                              // 验证所属Peer
    uint32_t                            peer_generation;                                           // Peer运行代际
    uint32_t                            operation_id;                                              // VERIFY_DONE代表事务ID
    uint32_t                            operation_ids[LINKG_TRANSPORT_CLASS_COUNT];                // 本组各业务原始事务ID
    uint32_t                            class_mask;                                                // 本次参与验证的业务集合
    linkg_switch_policy_reason_t        reason;                                                    // LOSS或RECOVERY
    uint32_t                            wifi_link_id;                                              // 启动时Wi-Fi Link实例
    uint32_t                            cellular_link_id;                                          // 启动时Cellular Link实例
    uint64_t                            started_us;                                                // 冗余PlanSync ACK时刻
    uint64_t                            deadline_us;                                               // 固定3秒截止时刻
    uint64_t                            finished_us;                                               // 结果形成的时间

    uint64_t                            cellular_last_probe_us[LINKG_TRANSPORT_CLASS_COUNT];       // 最近已消费的Cellular Probe时间
    uint64_t                            cellular_last_good_probe_us[LINKG_TRANSPORT_CLASS_COUNT];  // 最近成功的Cellular Probe时间
    uint32_t                            cellular_probe_seen_mask;                                  // 验证期内有效Cellular Probe集合
    uint32_t                            cellular_max_rtt_us;                                       // 验证期内最大有效Cellular RTT
    uint8_t                             wifi_loss_score;                                           // Wi-Fi恢复积分，初始5分
    bool                                cellular_probe_failed;                                     // 验证期内Cellular Probe失败或RTT超限
    bool                                path_interrupted;                                          // 物理Path失效或Link实例变化
    bool                                active;                                                    // 验证会话已建立
    bool                                result_pending;                                            // 最终结果等待Operation消费
    linkg_switch_policy_verify_result_t result;                                                    // 固定截止时计算的结果

    linkg_switch_policy_verify_path_t   wifi;                                                      // Wi-Fi参与业务Wire统计
    linkg_switch_policy_verify_path_t   cellular;                                                  // Cellular参与业务Wire统计
} linkg_switch_policy_verify_runtime_t;

/****************************** 验证生命周期 ******************************/

void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime);
int  linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime, uint8_t peer_node_id, uint32_t peer_generation, const uint32_t operation_ids[LINKG_TRANSPORT_CLASS_COUNT], uint32_t class_mask, linkg_switch_policy_reason_t reason, const linkg_switch_observation_t *observation, uint64_t now_us);
int  linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_observation_t *observation, uint64_t now_us, linkg_switch_policy_event_t *event, bool *event_ready);

#endif // SWITCH_POLICY_VERIFY_H
