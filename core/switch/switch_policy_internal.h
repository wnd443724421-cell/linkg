
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
    LINKG_SWITCH_POLICY_LOSS_NONE = 0, // 尚未达到明显丢包异常触发条件
    LINKG_SWITCH_POLICY_LOSS_BAD       // 明显丢包异常，需要启动链路质量验证
} linkg_switch_policy_loss_state_t;

/****************************** Load状态 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_LOAD_NONE = 0, // 尚未达到队列溢出迁移条件
    LINKG_SWITCH_POLICY_LOAD_BAD       // 需要迁移下一类Wi-Fi业务
} linkg_switch_policy_load_state_t;

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

int linkg_switch_policy_loss_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_loss_state_t *loss_state);
int linkg_switch_policy_load_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_load_state_t *load_state);

/****************************** 主链路策略 ******************************/

int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us);
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us);

#endif
