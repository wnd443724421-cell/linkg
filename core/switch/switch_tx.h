/**
 * @file switch_tx.h
 * @brief LinkG链路切换控制消息发送接口
 */

#ifndef SWITCH_TX_H
#define SWITCH_TX_H

#include <stdint.h>

#include "switch_wire.h"

/****************************** 前置声明 ******************************/

typedef struct linkg_packet_pool linkg_packet_pool_t; // LinkG数据包内存池

/****************************** 数据发送 ******************************/

int linkg_switch_tx_send_wifi_quality_report(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_wifi_quality_report_t *report);
int linkg_switch_tx_send_plan_sync(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_plan_sync_t *plan);
int linkg_switch_tx_send_plan_ack(linkg_packet_pool_t *pool, uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_plan_ack_t *ack);

#endif
