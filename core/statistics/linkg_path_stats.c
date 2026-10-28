
/**
 * @file linkg_path_stats.c
 * @brief LinkG路径累计统计实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-10-28
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
 *
 * @note 保留旧接口，仅更新全局统计。
 */
void linkg_path_stats_record_tx_success(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->tx_bytes   += bytes;
    stats->tx_packets += packets;
}

/**
 * @brief 记录指定业务发送成功，同时更新全局与分类统计。
 *
 * @note class_index必须对应REALTIME、VIDEO或DATA；无效索引不写入统计。
 *       调用方不得再单独调用无类别版本，避免全局计数重复累加。
 */
void linkg_path_stats_record_tx_success_class(linkg_path_stats_t *stats, uint32_t class_index, uint64_t bytes, uint64_t packets)
{
    linkg_path_class_tx_stats_t *class_stats;

    if (stats == NULL || class_index >= LINKG_PATH_STATS_CLASS_COUNT)
    {
        return;
    }

    class_stats = &stats->tx_classes[class_index];

    linkg_path_stats_record_tx_success(stats, bytes, packets);

    class_stats->tx_bytes   += bytes;
    class_stats->tx_packets += packets;
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

    stats->tx_failed_bytes   += bytes;
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

    stats->tx_dropped_bytes   += bytes;
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

    stats->tx_expired_bytes   += bytes;
    stats->tx_expired_packets += packets;
}

/**
 * @brief 精确记录队列容量不足导致的淘汰，同时归入主动丢弃总计。
 *
 * @note 保留旧接口，仅更新全局统计。
 */
void linkg_path_stats_record_tx_queue_overflow(linkg_path_stats_t *stats, uint64_t bytes, uint64_t packets)
{
    if (stats == NULL)
    {
        return;
    }

    stats->tx_dropped_bytes          += bytes;
    stats->tx_dropped_packets        += packets;
    stats->tx_queue_overflow_bytes   += bytes;
    stats->tx_queue_overflow_packets += packets;
}

/**
 * @brief 记录指定业务队列溢出，同时更新全局丢弃、全局OVF与分类OVF。
 *
 * @note OVF已经计入全局tx_dropped，不得再额外调用record_tx_dropped。
 *       class_index无效时不写入统计。
 */
void linkg_path_stats_record_tx_queue_overflow_class(linkg_path_stats_t *stats, uint32_t class_index, uint64_t bytes, uint64_t packets)
{
    linkg_path_class_tx_stats_t *class_stats;

    if (stats == NULL || class_index >= LINKG_PATH_STATS_CLASS_COUNT)
    {
        return;
    }

    class_stats = &stats->tx_classes[class_index];

    linkg_path_stats_record_tx_queue_overflow(stats, bytes, packets);

    class_stats->tx_queue_overflow_bytes   += bytes;
    class_stats->tx_queue_overflow_packets += packets;
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

    stats->rx_bytes   += bytes;
    stats->rx_packets += packets;
}
