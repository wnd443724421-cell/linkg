/**
 * @file switch_policy_internal.h
 * @brief LinkG链路切换策略内部定义
 */

#ifndef SWITCH_POLICY_INTERNAL_H
#define SWITCH_POLICY_INTERNAL_H

#include <stdint.h>

#include "linkg_switch.h"

#include "switch_observation.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t                    peer_node_id;    // 当前直接AP节点编号
    uint32_t                   peer_generation; // 当前Peer运行代际
    linkg_send_plan_t          plan;            // 当前生效发送计划
    linkg_switch_observation_t observation;     // 当前最新观测快照
    bool                       wifi_blocked;    // 当前Wi-Fi是否被Maintenance禁止使用
    bool                       cellular_blocked;// 当前Cellular是否被Maintenance禁止使用
} linkg_switch_policy_input_t;

/****************************** 主链路策略 ******************************/

int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us);
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us);

#endif
