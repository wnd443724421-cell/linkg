/**
 * @file linkg_path_stats.h
 * @brief LinkG路径累计统计定义
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-25
 */

#ifndef LINKG_PATH_STATS_H
#define LINKG_PATH_STATS_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 路径统计 ******************************/

typedef struct
{
    uint64_t tx_bytes;                  // 已成功提交底层发送接口的LinkG帧字节数
    uint64_t tx_packets;                // 已成功提交底层发送接口的LinkG帧数量

    uint64_t tx_failed_bytes;           // 底层最终发送失败的LinkG帧字节数
    uint64_t tx_failed_packets;         // 底层最终发送失败的LinkG帧数量

    uint64_t tx_dropped_bytes;          // 本地主动丢弃的LinkG帧字节数
    uint64_t tx_dropped_packets;        // 本地主动丢弃的LinkG帧数量

    uint64_t tx_queue_overflow_bytes;   // TX等待队列溢出淘汰字节数，属于tx_dropped子集
    uint64_t tx_queue_overflow_packets; // TX等待队列溢出淘汰帧数，属于tx_dropped子集

    uint64_t tx_expired_bytes;          // 本地等待超时失效字节数，独立于tx_dropped
    uint64_t tx_expired_packets;        // 本地等待超时失效帧数，独立于tx_dropped

    uint64_t rx_bytes;                  // 从该Path成功接收的LinkG帧字节数
    uint64_t rx_packets;                // 从该Path成功接收的LinkG帧数量
} linkg_path_stats_t;

/****************************** 统计操作 ******************************/

void linkg_path_stats_reset(linkg_path_stats_t *stats);
void linkg_path_stats_record_tx_success(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets);
void linkg_path_stats_record_tx_failed(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets);
void linkg_path_stats_record_tx_dropped(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets);
void linkg_path_stats_record_tx_expired(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets);
void linkg_path_stats_record_tx_queue_overflow(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets);
void linkg_path_stats_record_rx(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets);

#ifdef __cplusplus
}
#endif

#endif
