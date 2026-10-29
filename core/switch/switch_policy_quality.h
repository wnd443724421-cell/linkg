/**
 * @file switch_policy_quality.h
 * @brief LinkG Wi-Fi负载压力与无线丢包统一质量检测器内部状态
 */

#ifndef SWITCH_POLICY_QUALITY_H
#define SWITCH_POLICY_QUALITY_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_switch.h"

/****************************** 单方向Wire低样本累计 ******************************/

typedef struct
{
    uint64_t first_updated_us; // 本批首个有效Wire窗口的源时间
    uint64_t sample_packets;   // 本批累计有效业务样本数
    uint64_t lost_packets;     // 本批累计确认丢包数
} linkg_switch_quality_accumulator_t;

/****************************** 单方向HCC差分采样 ******************************/

typedef struct
{
    uint64_t last_updated_us; // 最近消费的HCC源观测时间
    uint32_t last_off_count;  // 最近消费的FLOWCTRL_OFF累计值
    bool     initialized;     // 是否已建立HCC差分基线
} linkg_switch_quality_hcc_state_t;

/****************************** Wire样本判决归属 ******************************/

typedef enum
{
    LINKG_SWITCH_QUALITY_WIRE_SKIP = 0, // 暂停积分，丢弃当前Wire窗口
    LINKG_SWITCH_QUALITY_WIRE_LOAD,     // HCC本轮新增OFF，Wire积分归LOAD
    LINKG_SWITCH_QUALITY_WIRE_LOSS      // 无HCC压力，Wire积分归LOSS
} linkg_switch_quality_wire_route_t;

/****************************** 统一质量检测器运行状态 ******************************/

typedef struct
{
    // 检测器归属
    linkg_send_plan_t                   plan;                              // 当前完整三业务发送计划
    uint32_t                            wifi_link_id;                      // 当前Wi-Fi Link运行实例

    // 上下行Wire低样本累计：两类判决共享输入，不重复消费
    linkg_switch_quality_accumulator_t  uplink_accumulator;                // 上行不足20包的样本批次
    linkg_switch_quality_accumulator_t  downlink_accumulator;              // 下行不足20包的样本批次

    // Wire观测去重及业务活动时间
    uint64_t                            last_wire_uplink_updated_us;       // 最近消费的上行Wire窗口时间
    uint64_t                            last_wire_downlink_updated_us;     // 最近消费的下行Wire窗口时间
    uint64_t                            last_wire_sample_us;               // 最近有效非零Wire业务样本时间
    uint64_t                            last_loss_valid_us;                // 最近完成LOSS有效评分的时间
    uint64_t                            last_load_valid_us;                // 最近完成LOAD有效评分的时间

    // LinkG队列溢出观测去重
    uint64_t                            last_overflow_uplink_updated_us;   // 最近消费的上行OVF窗口时间
    uint64_t                            last_overflow_downlink_updated_us; // 最近消费的下行OVF窗口时间

    // 上下行HCC累计OFF差分基线与压力状态
    linkg_switch_quality_hcc_state_t    uplink_hcc;                        // STA本地TX流控观测
    linkg_switch_quality_hcc_state_t    downlink_hcc;                      // AP远端TX流控观测
    uint64_t                            last_hcc_window_us;                // 最近参与HCC判决的Observation时间
    uint64_t                            last_hcc_pressure_us;              // 最近确认新增OFF或流控持续的时间
    uint8_t                             hcc_pressure_score;                // HCC候选压力积分，范围0～3
    bool                                hcc_pressure_active;               // 是否处于已确认压力保护阶段

    // Wire质量积分与状态隔离
    uint8_t                             loss_score;                        // 无背压证据时的Wire异常积分，范围0～5
    uint8_t                             load_score;                        // 新增OFF伴随Wire损失的积分，范围0～5
    uint64_t                            fast_window_started_us;            // 当前250ms合并窗口起始时刻
    uint64_t                            fast_wire_uplink_us;               // 快速LOAD独立Wire上行去重时间
    uint64_t                            fast_wire_downlink_us;             // 快速LOAD独立Wire下行去重时间
    uint32_t                            fast_window_active_mask;           // 当前Wi-Fi活跃业务掩码
    uint32_t                            fast_window_ack_mask;              // 当前窗口合格Probe ACK掩码
    uint8_t                             fast_window_evidence;              // 当前窗口HCC/Wire/Probe BAD证据位
    uint8_t                             fast_window_probe_points;          // 当前窗口Probe证据最高权重，0/1/2
    uint8_t                             fast_probe_timeout_streak[LINKG_TRANSPORT_CLASS_COUNT];  // 各业务连续TIMEOUT状态（饱和2）
    uint64_t                            fast_probe_last_timeout_us[LINKG_TRANSPORT_CLASS_COUNT]; // 各业务上次TIMEOUT时间
    linkg_switch_quality_wire_route_t   wire_route;                        // 当前Wire累计所属判决路径

    // 生命周期及PlanSync保护
    bool                                initialized;                       // 检测器是否已经初始化
    bool                                waiting_sync;                      // 是否等待同步后的新窗口隔离
} linkg_switch_quality_policy_runtime_t;

#endif // SWITCH_POLICY_QUALITY_H
