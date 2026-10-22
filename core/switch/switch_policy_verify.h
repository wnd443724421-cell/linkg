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

/****************************** 验证参数 ******************************/

#define LINKG_SWITCH_POLICY_VERIFY_DURATION_US         3000000ULL // ACK后固定验证3秒
#define LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US        250000U    // 5G有效Probe RTT必须严格小于250ms
#define LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES    20U        // 单方向单轮积分的最小样本数
#define LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES 100U       // 参与业务累计比较的最小样本数/每条链路
#define LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE         5U         // 进入LOSS验证时已有5分异常证据
#define LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE       3U         // 仅积分小于3才确认Wi-Fi恢复

/****************************** 窗口方向 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_UPLINK          = 0, // STA到AP方向
    LINKG_SWITCH_POLICY_VERIFY_DOWNLINK,            // AP到STA方向
    LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT      // 统计方向数量
} linkg_switch_policy_verify_direction_t;

/****************************** 单链路验证样本 ******************************/

typedef struct
{
    uint64_t lost_packets[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                              // 各方向参与业务Wire确认丢失总数
    uint64_t sample_packets[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                            // 各方向参与业务Wire样本总数
    uint64_t class_samples[LINKG_TRANSPORT_CLASS_COUNT];                                            // 各参与业务Wire样本，防止不同业务之间误配
    uint64_t last_loss_us[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT][LINKG_TRANSPORT_CLASS_COUNT]; // 各方向、各业务Wire窗口去重时间戳
    uint64_t last_load_us[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                              // 各方向Load窗口去重时间戳
    uint64_t last_probe_us[LINKG_TRANSPORT_CLASS_COUNT];                                            // 各业务Probe去重时间戳
    uint32_t probe_seen_mask;                                                                       // 本次窗口各业务已收到的有效可达Probe集合
    uint32_t max_rtt_us;                                                                            // 窗口内新鲜Probe最大RTT
    bool     probe_failed;                                                                          // Probe超时、不可达或RTT达到上限
    bool     queue_overflow;                                                                        // 链路本窗口出现整链路发送队列丢弃
} linkg_switch_policy_verify_path_t;

/****************************** Peer级验证会话 ******************************/

/**
 * @brief 同一Peer的一次整组冗余验证运行状态。
 *
 * Operation管理本组全部业务Operation ID，Verify只保存代表性ID用于输出VERIFY_DONE。
 * 调用方持有Switch锁，或保证仅由Switch Worker访问该运行状态。
 */
typedef struct
{
    uint8_t                           peer_node_id;     // 本次验证所属Peer
    uint32_t                          peer_generation;  // Peer运行代际
    uint32_t                          operation_id;     // 本组验证代表事务ID（非零）
    uint32_t                          class_mask;       // 本轮参与验证的业务集合，排除已单发5G业务
    linkg_switch_policy_reason_t      reason;           // LOSS或RECOVERY
    uint32_t                          wifi_link_id;     // 开始验证时Wi-Fi链路实例
    uint32_t                          cellular_link_id; // 开始验证时5G链路实例
    uint64_t                          started_us;       // 收到冗余ACK的单调时刻
    uint64_t                          deadline_us;      // started_us + 3秒
    uint8_t                           wifi_loss_score;  // Wi-Fi无线Wire丢包恢复积分（初始5）
    bool                              active;           // 有活动验证会话
    bool                              reported;         // VERIFY_DONE已经输出，防止重复处理
    bool                              path_interrupted; // 验证期间任一Path失效或Link实例变化
    linkg_switch_policy_verify_path_t wifi;             // Wi-Fi本次参与业务的新样本
    linkg_switch_policy_verify_path_t cellular;         // 5G本次参与业务的新样本
} linkg_switch_policy_verify_runtime_t;

/****************************** 验证生命周期 ******************************/

void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime);
int  linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime, uint8_t peer_node_id, uint32_t peer_generation, uint32_t operation_id, uint32_t class_mask, linkg_switch_policy_reason_t reason, const linkg_switch_observation_t *observation, uint64_t now_us);
int  linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_observation_t *observation, uint64_t now_us, linkg_switch_policy_event_t *event, bool *event_ready);

#endif
