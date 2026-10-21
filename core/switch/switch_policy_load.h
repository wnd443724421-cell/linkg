/**
 * @file switch_policy_load.h
 * @brief LinkG Wi-Fi队列溢出检测器内部状态
 */

#ifndef SWITCH_POLICY_LOAD_H
#define SWITCH_POLICY_LOAD_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_switch.h"

/****************************** Load检测器运行状态 ******************************/

typedef struct
{
    linkg_send_plan_t plan;                     // 上一次判决使用的完整发送计划
    uint64_t          last_uplink_updated_us;   // 最近处理的上行观测时间
    uint64_t          last_downlink_updated_us; // 最近处理的下行观测时间
    uint64_t          last_valid_us;            // 最近一次有效样本时间
    uint32_t          wifi_link_id;             // 当前积分归属的Wi-Fi Link
    uint8_t           score;                    // 队列溢出异常积分
    bool              initialized;              // 检测器是否已初始化
    bool              waiting_sync;             // 是否处于Plan同步过渡期
} linkg_switch_load_policy_runtime_t;

#endif
