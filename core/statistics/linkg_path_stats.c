/**
 * @file linkg_path_stats.c
 * @brief LinkG路径累计统计实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-25
 */

#include "linkg_path_stats.h"

#include <string.h>

/****************************** 统计操作 ******************************/

/**
 * @brief 清空路径累计统计。
 */
void linkg_path_stats_reset(linkg_path_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }

    memset(stats, 0, sizeof(*stats));
}

/**
 * @brief 记录路径底层发送成功。
 */
void linkg_path_stats_record_tx_success(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->tx_bytes += bytes;
    stats->tx_packets += packets;
}

/**
 * @brief 记录路径底层最终发送失败。
 */
void linkg_path_stats_record_tx_failed(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->tx_failed_bytes += bytes;
    stats->tx_failed_packets += packets;
}

/**
 * @brief 记录路径本地主动丢弃。
 */
void linkg_path_stats_record_tx_dropped(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->tx_dropped_bytes += bytes;
    stats->tx_dropped_packets += packets;
}

/**
 * @brief 记录路径本地等待超时失效。
 */
void linkg_path_stats_record_tx_expired(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->tx_expired_bytes += bytes;
    stats->tx_expired_packets += packets;
}

/**
 * @brief 记录路径实际接收成功。
 */
void linkg_path_stats_record_rx(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->rx_bytes += bytes;
    stats->rx_packets += packets;
}
