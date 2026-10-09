
/**
 * @file switch_policy_internal.h
 * @brief LinkG链路切换策略内部定义
 */

#ifndef SWITCH_POLICY_INTERNAL_H
#define SWITCH_POLICY_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_switch.h"

#include "switch_observation.h"

/****************************** Loss状态 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_LOSS_NONE    = 0, // 当前无明显丢包异常
    LINKG_SWITCH_POLICY_LOSS_SUSPECT,     // 轻微异常，需要RF辅助判断
    LINKG_SWITCH_POLICY_LOSS_BAD          // 明显异常，无需RF辅助判断
} linkg_switch_policy_loss_state_t;

/****************************** RF状态 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_RF_UNKNOWN  = 0, // 无法确定当前RF状态
    LINKG_SWITCH_POLICY_RF_GOOD,         // 当前RF环境良好
    LINKG_SWITCH_POLICY_RF_NEUTRAL,      // 当前RF环境处于灰区
    LINKG_SWITCH_POLICY_RF_DEGRADED      // 当前RF环境明显恶化
} linkg_switch_policy_rf_state_t;

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t                    peer_node_id;     // 当前直接AP节点编号
    uint32_t                   peer_generation;  // 当前Peer运行代际
    linkg_send_plan_t          plan;             // 当前生效发送计划
    linkg_switch_observation_t observation;      // 当前最新观测快照
    bool                       wifi_blocked;     // 当前Wi-Fi是否被Maintenance禁止使用
    bool                       cellular_blocked; // 当前Cellular是否被Maintenance禁止使用
} linkg_switch_policy_input_t;

/****************************** 质量判断 ******************************/

int linkg_switch_policy_loss_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_loss_state_t *loss_state);
int linkg_switch_policy_rf_evaluate(const linkg_switch_wifi_radio_observation_t *radio, linkg_switch_policy_rf_state_t *rf_state);

/****************************** 主链路策略 ******************************/

int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us);
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us);

#endif
