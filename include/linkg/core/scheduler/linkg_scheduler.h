/**
 * @file linkg_scheduler.h
 * @brief LinkG发送调度接口
 */

#ifndef LINKG_SCHEDULER_H
#define LINKG_SCHEDULER_H

#include <stdint.h>

#include "linkg_transport_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 模块常量 ******************************/

#define LINKG_SCHEDULER_TX_BATCH_MAX 32U // 单次Scheduler调用最大Packet数量

/****************************** 前置声明 ******************************/

typedef struct linkg_packet linkg_packet_t;

/****************************** 调度策略 ******************************/

typedef enum
{
    LINKG_SCHEDULER_POLICY_DEFAULT = 0, // 使用当前发送计划
    LINKG_SCHEDULER_POLICY_REDUNDANT,   // 尽可能通过当前主备链路同时发送
    LINKG_SCHEDULER_POLICY_SPECIFIED    // 仅通过指定链路发送
} linkg_scheduler_policy_t;

/****************************** 发送上下文 ******************************/

typedef struct
{
    linkg_transport_class_t   traffic_class;     // 当前batch统一业务类别，由上层确定
    linkg_scheduler_policy_t  policy;            // 当前batch调度策略
    uint32_t                  specified_link_id; // SPECIFIED策略使用的指定链路
} linkg_scheduler_tx_context_t;

/****************************** 发送元素 ******************************/

typedef struct
{
    linkg_packet_t         *packet;              // 待发送Transport载荷Packet，仅借用引用
    linkg_transport_type_t  type;                // Transport帧类型
    uint8_t                 destination_node_id; // 最终目标节点编号
    int                     result;              // 单包调度结果，由Scheduler填写
} linkg_scheduler_tx_item_t;

/****************************** 生命周期 ******************************/

int linkg_scheduler_init(void);
int linkg_scheduler_deinit(void);

/****************************** 数据发送 ******************************/

int linkg_scheduler_submit(const linkg_scheduler_tx_context_t *context, linkg_scheduler_tx_item_t *item);
int linkg_scheduler_submit_batch(const linkg_scheduler_tx_context_t *context, linkg_scheduler_tx_item_t *items, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
