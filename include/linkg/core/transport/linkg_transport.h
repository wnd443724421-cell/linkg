/**
 * @file linkg_transport.h
 * @brief LinkG逻辑传输层接口
 */

#ifndef LINKG_TRANSPORT_H
#define LINKG_TRANSPORT_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"
#include "linkg_scheduler.h"
#include "linkg_transport_stats.h"
#include "linkg_transport_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_packet linkg_packet_t;

/****************************** 数据交付 ******************************/

typedef struct
{
    linkg_transport_header_t header; // Transport协议头
    linkg_packet_t          *packet; // Transport载荷数据包，仅借用引用
} linkg_transport_delivery_t;

typedef struct
{
    linkg_packet_t *packet;              // Transport载荷数据包
    uint8_t         destination_node_id; // 最终目标节点编号
} linkg_transport_tx_item_t;

/**
 * Transport本机批量数据交付回调。
 *
 * items中的Packet在回调期间仅包含Transport载荷，并且只作为借用引用使用。
 * 如果需要在回调返回后继续持有Packet，必须在返回前显式增加Packet引用。
 */
typedef int (*linkg_transport_handler_func_t)(const linkg_transport_delivery_t *items, uint32_t count, void *user_data);

/****************************** 生命周期 ******************************/

int linkg_transport_init(void);
int linkg_transport_deinit(void);

/****************************** Peer状态 ******************************/

int linkg_transport_register_peer(uint8_t peer_node_id);
int linkg_transport_reset_peer(uint8_t peer_node_id, bool clear_stats);
int linkg_transport_unregister_peer(uint8_t peer_node_id);

/****************************** 类型交付 ******************************/

int linkg_transport_register_handler(linkg_transport_type_t type, linkg_transport_handler_func_t handler, void *user_data);
int linkg_transport_unregister_handler(linkg_transport_type_t type);

/****************************** 数据发送 ******************************/

int linkg_transport_send(uint8_t destination_node_id, linkg_transport_type_t type, linkg_packet_t *packet, linkg_scheduler_policy_t policy, uint32_t specified_link_id);
int linkg_transport_send_batch(const linkg_transport_tx_item_t *items, uint32_t count, linkg_transport_type_t type, linkg_scheduler_policy_t policy, uint32_t specified_link_id);

/****************************** 链路接收 ******************************/

void linkg_transport_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t count, void *user_data);

/****************************** 统计查询 ******************************/

int linkg_transport_get_peer_stats(uint8_t peer_node_id, linkg_transport_peer_stats_t *stats);
int linkg_transport_get_global_stats(linkg_transport_global_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
