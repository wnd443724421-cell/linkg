
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


/****************************** Wi-Fi质量判决状态 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_QUALITY_NONE = 0, // 未达到质量异常触发条件
    LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD, // 负载导致业务损失，需要逐类迁移Wi-Fi业务
    LINKG_SWITCH_POLICY_QUALITY_LOSS_BAD  // 无线质量异常，需要启动整组冗余验证
} linkg_switch_policy_quality_state_t;

/****************************** 策略输入快照 ******************************/

typedef struct
{
    uint8_t                    peer_node_id;     // 当前直接AP节点编号
    uint32_t                   peer_generation;  // 当前Peer运行代际
    linkg_send_plan_t          plan;             // 当前三类业务完整发送计划
    linkg_switch_observation_t observation;      // 当前最新观测快照
    bool                       wifi_blocked;     // 当前Wi-Fi是否被Maintenance禁止使用
    bool                       cellular_blocked; // 当前Cellular是否被Maintenance禁止使用
} linkg_switch_policy_input_t;

/****************************** 质量判断 ******************************/

int linkg_switch_policy_quality_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_quality_state_t *quality_state);

/****************************** 主链路策略 ******************************/

int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us);
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us);

#endif
