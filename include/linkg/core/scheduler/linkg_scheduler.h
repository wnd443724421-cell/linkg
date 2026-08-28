/**
 * @file linkg_scheduler.h
 * @brief LinkG发送调度接口
 */

#ifndef LINKG_SCHEDULER_H
#define LINKG_SCHEDULER_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_packet linkg_packet_t;

/****************************** 单包策略 ******************************/

typedef enum
{
    LINKG_SCHEDULER_POLICY_DEFAULT = 0, // 使用当前发送计划
    LINKG_SCHEDULER_POLICY_REDUNDANT,   // 尽可能通过当前主备链路同时发送
    LINKG_SCHEDULER_POLICY_SPECIFIED    // 仅通过指定链路发送
} linkg_scheduler_policy_t;

/****************************** 发送元素 ******************************/

typedef struct
{
    linkg_packet_t *packet;           // 待发送数据包
    uint8_t         next_hop_node_id; // 当前物理下一跳节点编号
    int             result;           // 单包调度结果，由Scheduler填写
} linkg_scheduler_tx_item_t;

/****************************** 生命周期 ******************************/

int linkg_scheduler_init(void);
int linkg_scheduler_deinit(void);

/****************************** 数据发送 ******************************/

int linkg_scheduler_submit(uint8_t next_hop_node_id, linkg_packet_t *packet, linkg_scheduler_policy_t policy, uint32_t specified_link_id);
int linkg_scheduler_submit_batch(linkg_scheduler_tx_item_t *items, uint32_t count, linkg_scheduler_policy_t policy, uint32_t specified_link_id);

#ifdef __cplusplus
}
#endif

#endif
