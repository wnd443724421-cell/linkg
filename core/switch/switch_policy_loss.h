/**
 * @file switch_policy_loss.h
 * @brief LinkG Wi-Fi丢包异常检测器内部状态
 */

#ifndef SWITCH_POLICY_LOSS_H
#define SWITCH_POLICY_LOSS_H

#include <stdbool.h>
#include <stdint.h>

/****************************** Loss检测器运行状态 ******************************/

typedef struct
{
    uint64_t last_uplink_updated_us;   // 最近处理的上行观测时间
    uint64_t last_downlink_updated_us; // 最近处理的下行观测时间
    uint64_t last_valid_us;            // 最近一次有效样本时间
    uint32_t wifi_link_id;             // 当前积分归属的Wi-Fi Link
    uint8_t  score;                    // 上下行融合后的异常积分
    bool     initialized;              // 检测器是否已初始化
} linkg_switch_loss_policy_runtime_t;

#endif
