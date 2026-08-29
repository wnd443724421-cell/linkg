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
 * @brief 单类Transport帧累计统计。
 *
 * bytes均表示Transport载荷长度，不包含Transport基础头和分片扩展头。
 * 冗余发送只统计一个Transport TX Frame，重复接收Frame单独累计duplicate。
 */
typedef struct
{
    uint64_t tx_bytes;             // 成功提交Transport帧载荷字节数
    uint64_t tx_packets;           // 成功提交Transport帧数量
    uint64_t tx_failed_bytes;      // 提交失败Transport帧载荷字节数
    uint64_t tx_failed_packets;    // 提交失败Transport帧数量
    uint64_t rx_bytes;             // 去重后有效Transport帧载荷字节数
    uint64_t rx_packets;           // 去重后有效Transport帧数量
    uint64_t rx_duplicate_packets; // 重复Transport帧数量
} linkg_transport_type_stats_t;

/****************************** Peer统计 ******************************/

/**
 * @brief 直接Peer的Transport累计统计。
 */
typedef struct
{
    linkg_transport_type_stats_t types[LINKG_TRANSPORT_TYPE_COUNT]; // 各Transport类型累计统计
    uint64_t                     last_tx_ms;                        // 最近成功提交Transport帧时间
    uint64_t                     last_rx_ms;                        // 最近有效接收Transport帧时间
    uint8_t                      peer_node_id;                      // 直接Peer节点编号
} linkg_transport_peer_stats_t;

/****************************** 全局统计 ******************************/

/**
 * @brief Transport全局异常和转发累计统计。
 */
typedef struct
{
    uint64_t rx_invalid_frames;          // 非法Transport帧数量
    uint64_t rx_unattributed_frames;     // 无法归属直接Peer的Transport帧数量
    uint64_t forward_incomplete_bytes;   // AP不完整分片组丢弃载荷字节数
    uint64_t forward_incomplete_frames;  // AP不完整分片组丢弃Transport帧数量
} linkg_transport_global_stats_t;

#ifdef __cplusplus
}
#endif

#endif
