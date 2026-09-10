/**
 * @file linkg_transport_stats.h
 * @brief LinkG传输层累计统计定义
 */

#ifndef LINKG_TRANSPORT_STATS_H
#define LINKG_TRANSPORT_STATS_H

#include <stdint.h>

#include "linkg_transport_types.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 类别统计 ******************************/

typedef struct
{
    uint64_t tx_bytes;                // 下层Link成功接管的Transport载荷字节数
    uint64_t tx_packets;              // 下层Link成功接管的Transport帧数量
    uint64_t tx_failed_bytes;         // 下层Link拒绝接管的Transport载荷字节数
    uint64_t tx_failed_packets;       // 下层Link拒绝接管的Transport帧数量

    uint64_t rx_bytes;                // 去重后有效Transport载荷字节数
    uint64_t rx_packets;              // 去重后有效Transport帧数量
    uint64_t rx_lost_packets;         // RX Window最终确认丢失的Transport帧数量
    uint64_t rx_duplicate_packets;    // 重复到达Transport帧数量
    uint64_t rx_out_of_order_packets; // 乱序到达Transport帧数量

    uint64_t last_tx_ms;              // 最近成功提交Transport帧时间
    uint64_t last_rx_ms;              // 最近收到有效Transport帧时间
} linkg_transport_class_stats_t;

/****************************** Peer统计 ******************************/

typedef struct
{
    linkg_transport_class_stats_t classes[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务类别独立累计统计
    uint8_t                       peer_node_id;                          // 当前直接Peer节点编号
} linkg_transport_peer_stats_t;

/****************************** 全局统计 ******************************/

typedef struct
{
    uint64_t rx_invalid_frames;         // 格式、长度或协议字段非法的Transport帧数量
    uint64_t rx_unattributed_frames;    // 无法归属到直接Peer的Transport帧数量
    uint64_t forward_incomplete_bytes;  // 中继不完整分片组丢弃的Transport载荷字节数
    uint64_t forward_incomplete_frames; // 中继不完整分片组丢弃的Transport帧数量
} linkg_transport_global_stats_t;

#ifdef __cplusplus
}
#endif

#endif
