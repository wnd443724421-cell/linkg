/**
 * @file linkg_transport_stats.h
 * @brief LinkG传输层累计统计定义
 */

#ifndef LINKG_TRANSPORT_STATS_H
#define LINKG_TRANSPORT_STATS_H

#include <stdint.h>

#include "linkg_transport_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型统计 ******************************/

/**
 * @brief 单类Transport逻辑数据累计统计。
 *
 * bytes均表示Transport载荷长度，不包含Transport头部。
 * 冗余发送只统计一个逻辑TX，重复接收去重后只统计一个逻辑RX。
 */
typedef struct
{
    uint64_t tx_bytes;             // 成功发送Transport载荷字节数
    uint64_t tx_packets;           // 成功发送逻辑包数量
    uint64_t tx_failed_bytes;      // 发送失败Transport载荷字节数
    uint64_t tx_failed_packets;    // 发送失败逻辑包数量
    uint64_t rx_bytes;             // 有效接收Transport载荷字节数
    uint64_t rx_packets;           // 去重后有效接收逻辑包数量
    uint64_t rx_duplicate_packets; // 重复接收逻辑包数量
} linkg_transport_type_stats_t;

/****************************** Peer统计 ******************************/

/**
 * @brief 直接Peer的Transport累计统计。
 */
typedef struct
{
    linkg_transport_type_stats_t types[LINKG_TRANSPORT_TYPE_COUNT]; // 各Transport类型累计统计
    uint64_t                     last_tx_ms;                        // 最近成功发送时间
    uint64_t                     last_rx_ms;                        // 最近有效接收时间
    uint8_t                      peer_node_id;                      // 直接Peer节点编号
} linkg_transport_peer_stats_t;

/****************************** 全局统计 ******************************/

/**
 * @brief Transport全局异常和转发队列累计统计。
 */
typedef struct
{
    uint64_t rx_invalid_frames;             // 非法Transport帧数量
    uint64_t rx_unattributed_frames;        // 无法归属直接Peer的Transport帧数量
} linkg_transport_global_stats_t;

#ifdef __cplusplus
}
#endif

#endif
