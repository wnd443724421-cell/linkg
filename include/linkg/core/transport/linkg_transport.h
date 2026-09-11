/**
 * @file linkg_transport.h
 * @brief LinkG逻辑传输层接口
 */

#ifndef LINKG_TRANSPORT_H
#define LINKG_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"
#include "linkg_path.h"
#include "linkg_transport_stats.h"
#include "linkg_transport_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_TX_TARGET_MAX    2U  // 主备模型单次最大发送Target数量

/****************************** 前置声明 ******************************/

typedef struct linkg_packet linkg_packet_t;

/****************************** 发送类型 ******************************/

typedef struct
{
    linkg_link_t          *link;        // Scheduler确定的具体Link，仅借用
    linkg_path_t          *path;        // Scheduler确定的直接Peer Path，仅借用
    linkg_path_endpoint_t  destination; // Scheduler确定的下一跳Endpoint快照
} linkg_transport_tx_target_t;

typedef struct
{
    const linkg_transport_tx_target_t *targets;       // Scheduler确定的物理发送Target数组，仅借用
    uint32_t                           target_count;  // 当前物理发送Target数量
    linkg_transport_class_t            traffic_class; // 当前batch统一业务类别
    uint8_t                            peer_node_id;  // 当前物理下一跳直接Peer节点编号
} linkg_transport_tx_context_t;

typedef struct
{
    linkg_packet_t         *packet;              // 原始Transport载荷数据包，仅借用引用
    linkg_transport_type_t  type;                // Transport帧类型
    uint8_t                 destination_node_id; // 最终目标节点编号
} linkg_transport_tx_item_t;

/****************************** 本机交付 ******************************/

typedef struct
{
    linkg_packet_t          *packet;         // 已完成Transport处理的载荷Packet，仅借用引用
    linkg_transport_class_t  traffic_class;  // 当前业务类别
    uint8_t                  source_node_id; // 原始发送节点编号
    uint8_t                  peer_node_id;   // 当前物理上一跳直接Peer节点编号
} linkg_transport_delivery_t;

typedef int (*linkg_transport_handler_func_t)(const linkg_transport_delivery_t *items, uint32_t count, void *user_data);

/****************************** 中继交付 ******************************/

typedef struct
{
    linkg_packet_t *packet;              // 完整Transport Wire Frame，仅借用引用
    uint32_t        payload_length;      // 当前Transport Frame实际载荷长度
    uint8_t         destination_node_id; // 最终目标节点编号
    uint8_t         peer_node_id;        // 当前物理上一跳直接Peer节点编号
} linkg_transport_forward_item_t;

typedef int (*linkg_transport_forward_handler_func_t)(linkg_transport_class_t traffic_class, const linkg_transport_forward_item_t *items, uint32_t count, int *results, void *user_data);

/****************************** 生命周期 ******************************/

int linkg_transport_init(void);
int linkg_transport_deinit(void);

/****************************** Peer状态 ******************************/

int linkg_transport_register_peer(uint8_t peer_node_id);
int linkg_transport_reset_peer(uint8_t peer_node_id, bool clear_stats);
int linkg_transport_unregister_peer(uint8_t peer_node_id);

/****************************** 本机交付 ******************************/

int linkg_transport_register_handler(linkg_transport_type_t type, linkg_transport_handler_func_t handler, void *user_data);
int linkg_transport_unregister_handler(linkg_transport_type_t type);

/****************************** 中继调度 ******************************/

int linkg_transport_register_forward_handler(linkg_transport_forward_handler_func_t handler, void *user_data);
int linkg_transport_unregister_forward_handler(void);

/****************************** 数据发送 ******************************/

int linkg_transport_send(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *item);
int linkg_transport_send_batch(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *items, uint32_t count, int *results);

/****************************** 中继发送 ******************************/

int linkg_transport_forward_batch(const linkg_transport_tx_context_t *context, const linkg_transport_forward_item_t *items, uint32_t count, int *results);
/****************************** 统计查询 ******************************/

int linkg_transport_get_peer_stats(uint8_t peer_node_id, linkg_transport_peer_stats_t *stats);
int linkg_transport_get_global_stats(linkg_transport_global_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
