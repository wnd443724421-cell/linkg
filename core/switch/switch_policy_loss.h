/**
 * @file switch_policy_loss.h
 * @brief LinkG Wi-Fi丢包异常检测器内部状态
 */

#ifndef SWITCH_POLICY_LOSS_H
#define SWITCH_POLICY_LOSS_H

#include <stdbool.h>
#include <stdint.h>

/****************************** 单方向低样本累计状态 ******************************/

typedef struct
{
    uint64_t first_updated_us; // 本批累计样本首个窗口的时间
    uint64_t sample_packets;   // 本批累计的有效样本数
    uint64_t lost_packets;     // 本批累计的确认丢包数
} linkg_switch_loss_accumulator_t;

/****************************** Loss检测器运行状态 ******************************/

typedef struct
{
    // 上下行独立累计状态
    linkg_switch_loss_accumulator_t uplink_accumulator;       // 上行不足20包的累计样本
    linkg_switch_loss_accumulator_t downlink_accumulator;     // 下行不足20包的累计样本

    // 观测窗口去重
    uint64_t                        last_uplink_updated_us;   // 最近处理的上行观测时间
    uint64_t                        last_downlink_updated_us; // 最近处理的下行观测时间

    // 样本活动与质量判决时间
    uint64_t                        last_sample_us;           // 最近一次收到有效非零新样本的时间
    uint64_t                        last_valid_us;            // 最近一次完成有效质量判决的时间

    // Wi-Fi实例及共享积分
    uint32_t                        wifi_link_id;             // 当前积分归属的Wi-Fi Link
    uint8_t                         score;                    // 上下行融合后的异常积分
    bool                            initialized;              // 检测器是否已初始化
} linkg_switch_loss_policy_runtime_t;

#endif // SWITCH_POLICY_LOSS_H
