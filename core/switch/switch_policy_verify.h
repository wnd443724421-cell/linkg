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

#define LINKG_SWITCH_POLICY_VERIFY_DURATION_US      3000000ULL // ACK后固定验证3秒
#define LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US       250000U  // 5G有效Probe RTT必须严格小于250ms
#define LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES       20U  // 单方向单轮积分的最小样本数
#define LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES   100U  // 参与业务累计比较的最小样本数/每条链路
#define LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE             5U  // 进入LOSS验证时已有5分异常证据
#define LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE           3U  // 仅积分小于3才确认Wi-Fi恢复

/****************************** 窗口方向 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_UPLINK = 0,
    LINKG_SWITCH_POLICY_VERIFY_DOWNLINK,
    LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT
} linkg_switch_policy_verify_direction_t;

/****************************** 单链路验证样本 ******************************/

typedef struct
{
    uint64_t lost_packets[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];   // 各方向参与业务Wire确认丢失总数
    uint64_t sample_packets[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT]; // 各方向参与业务Wire样本总数
    uint64_t class_samples[LINKG_TRANSPORT_CLASS_COUNT];               // 各参与业务Wire样本，防止不同业务之间误配

    uint64_t last_loss_us[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT][LINKG_TRANSPORT_CLASS_COUNT]; // 去重时间戳
    uint64_t last_load_us[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];                              // 去重时间戳
    uint64_t last_probe_us[LINKG_TRANSPORT_CLASS_COUNT];                                           // 去重时间戳

    uint32_t probe_seen_mask; // 本次窗口各业务已收到的有效可达Probe集合
    uint32_t max_rtt_us;      // 窗口内新鲜Probe最大RTT
    bool     probe_failed;    // Probe超时、不可达或RTT达到上限
    bool     queue_overflow;  // 链路本窗口出现整链路发送队列丢弃
} linkg_switch_policy_verify_path_t;

/****************************** Peer级验证会话 ******************************/

/**
 * @brief 同一Peer的一次整组冗余验证；不创建额外线程或计时器。
 *
 * Operation负责管理参与本组的所有业务Operation ID。
 * verify只保存一个代表性operation_id，用于生成VERIFY_DONE事件。
 * 调用方持有Switch锁或保证仅Switch Worker访问。
 */
typedef struct
{
    uint8_t                            peer_node_id;    // 本次验证所属Peer
    uint32_t                           peer_generation; // Peer运行代际
    uint32_t                           operation_id;    // 本组验证代表事务ID（非零）
    uint32_t                           class_mask;      // 本轮参与验证的业务集合，排除已单发5G业务
    linkg_switch_policy_reason_t       reason;          // LOSS或RECOVERY
    uint32_t                           wifi_link_id;    // 开始验证时Wi-Fi链路实例
    uint32_t                           cellular_link_id;// 开始验证时5G链路实例
    uint64_t                           started_us;      // 收到冗余ACK的单调时刻
    uint64_t                           deadline_us;     // started_us + 3秒
    uint8_t                            wifi_loss_score; // Wi-Fi无线Wire丢包恢复积分（初始5）
    bool                               active;          // 有活动验证会话
    bool                               reported;        // VERIFY_DONE已经输出，防止重复处理
    bool                               path_interrupted;// 验证期间任一Path失效或Link实例变化
    linkg_switch_policy_verify_path_t  wifi;            // Wi-Fi本次参与业务的新样本
    linkg_switch_policy_verify_path_t  cellular;        // 5G本次参与业务的新样本
} linkg_switch_policy_verify_runtime_t;

/****************************** 验证生命周期 ******************************/

void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime);

int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime,
                                     uint8_t peer_node_id,
                                     uint32_t peer_generation,
                                     uint32_t operation_id,
                                     uint32_t class_mask,
                                     linkg_switch_policy_reason_t reason,
                                     const linkg_switch_observation_t *observation,
                                     uint64_t now_us);

/**
 * @brief 消费本轮Observation并在固定3秒截止时输出一次VERIFY_DONE。
 *
 * 返回0表示正常处理（包括尚未到期）；输出事件时设置*event_ready=true。
 * 返回负值表示调用参数错误或会话不匹配，不隐式创建新事务。
 * 事件成功提交给Operation前不要reset本会话，以便读取class_mask。
 */
int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime,
                                       const linkg_switch_observation_t *observation,
                                       uint64_t now_us,
                                       linkg_switch_policy_event_t *event,
                                       bool *event_ready);

#endif // SWITCH_POLICY_VERIFY_H
