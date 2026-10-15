/**
 * @file uart_debug.c
 * @brief LinkG串口收发、异常与SBUS水位调试统计
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#include "uart_internal.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "linkg_log.h"

/****************************** 时间获取 ******************************/

/**
 * @brief 获取系统启动后的单调毫秒时间。
 */
uint64_t uart_now_ms(void)
{
    struct timespec now;

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return 0U;
    }

    return (uint64_t)now.tv_sec * 1000U + (uint64_t)now.tv_nsec / 1000000U;
}

/****************************** 统计管理 ******************************/

/**
 * @brief 清空端口收发统计并建立下一次统计期限。
 */
void uart_stats_reset(linkg_uart_port_t *port)
{
    if (port == NULL)
    {
        return;
    }

    memset(&port->stats, 0, sizeof(port->stats));
    atomic_store(&port->stats.fifo_min, UINT32_MAX);
    port->last_debug_ms       = uart_now_ms();
    port->last_sbus_rx_frames = 0U;
    port->last_sbus_tx_frames = 0U;
    port->last_uart_rx_bytes  = 0U;
    port->last_uart_tx_bytes  = 0U;
    port->last_net_rx_bytes   = 0U;
    port->last_net_tx_bytes   = 0U;
    port->last_drops          = 0U;
    port->last_errors         = 0U;
    port->last_fifo_empty     = 0U;
    atomic_store(&port->debug_preview_budget, 3U);
}

/****************************** 字节预览 ******************************/

/**
 * @brief 打开Debug时每个统计周期最多预览3次物理UART真实收发字节。
 */
void uart_debug_data(linkg_uart_port_t *port, const char *direction, const uint8_t *data, uint32_t length)
{
    char     hexadecimal[LINKG_UART_DEBUG_PREVIEW_BYTES * 3U + 1U];
    uint32_t preview;
    uint32_t index;
    unsigned budget;
    int      position;

    if (port == NULL || direction == NULL || data == NULL || length == 0U || !atomic_load(&g_uart_manager.debug_enabled))
    {
        return;
    }

    budget = atomic_load(&port->debug_preview_budget);
    while (budget > 0U)
    {
        if (atomic_compare_exchange_weak(&port->debug_preview_budget, &budget, budget - 1U))
        {
            break;
        }
    }

    if (budget == 0U)
    {
        return;
    }

    preview  = length < LINKG_UART_DEBUG_PREVIEW_BYTES ? length : LINKG_UART_DEBUG_PREVIEW_BYTES;
    position = 0;
    for (index = 0U; index < preview; index++)
    {
        position += snprintf(hexadecimal + position, sizeof(hexadecimal) - (size_t)position, "%02X ", data[index]);
    }
    hexadecimal[position] = '\0';

    LINKG_LOG_DEBUG("UART: %s logical=%u bytes=%u data=%s", direction, port->config->uart_num, length, hexadecimal);
}

/****************************** FIFO采样 ******************************/

/**
 * @brief 原子记录FIFO水位样本，Debug关闭时不增加调度负担。
 */
void uart_stats_fifo_sample(linkg_uart_port_t *port, uint32_t depth)
{
    unsigned expected;

    if (port == NULL || !atomic_load(&g_uart_manager.debug_enabled))
    {
        return;
    }

    atomic_fetch_add(&port->stats.fifo_samples, 1U);
    atomic_fetch_add(&port->stats.fifo_sum, depth);

    expected = atomic_load(&port->stats.fifo_min);
    while (depth < expected && !atomic_compare_exchange_weak(&port->stats.fifo_min, &expected, depth))
    {
    }

    expected = atomic_load(&port->stats.fifo_max);
    while (depth > expected && !atomic_compare_exchange_weak(&port->stats.fifo_max, &expected, depth))
    {
    }
}

/****************************** 周期诊断 ******************************/

/**
 * @brief 每5s统计串口和网络实际收发差异，帮助定位无数据问题。
 */
void uart_stats_report(linkg_uart_manager_context_t *manager)
{
    linkg_uart_port_t *port;
    uint64_t           now_ms;
    uint64_t           urx;
    uint64_t           utx;
    uint64_t           nrx;
    uint64_t           ntx;
    uint64_t           drop;
    uint64_t           errors;
    uint64_t           empty;
    uint64_t           frames_rx;
    uint64_t           frames_tx;
    uint64_t           samples;
    uint64_t           sum;
    uint32_t           low;
    uint32_t           high;
    uint32_t           depth;
    uint32_t           index;

    if (manager == NULL || !atomic_load(&manager->debug_enabled))
    {
        return;
    }

    now_ms = uart_now_ms();

    for (index = 0U; index < manager->port_count; index++)
    {
        port = &manager->ports[index];
        if (now_ms - port->last_debug_ms < LINKG_UART_DEBUG_PERIOD_MS)
        {
            continue;
        }

        urx       = atomic_load(&port->stats.uart_rx_bytes);
        utx       = atomic_load(&port->stats.uart_tx_bytes);
        nrx       = atomic_load(&port->stats.net_rx_bytes);
        ntx       = atomic_load(&port->stats.net_tx_bytes);
        drop      = atomic_load(&port->stats.drops);
        errors    = atomic_load(&port->stats.errors);
        empty     = atomic_load(&port->stats.fifo_empty);
        frames_rx = atomic_load(&port->stats.sbus_rx_frames);
        frames_tx = atomic_load(&port->stats.sbus_tx_frames);
        samples   = atomic_exchange(&port->stats.fifo_samples, 0U);
        sum       = atomic_exchange(&port->stats.fifo_sum, 0U);
        low       = atomic_exchange(&port->stats.fifo_min, UINT32_MAX);
        high      = atomic_exchange(&port->stats.fifo_max, 0U);

        depth = 0U;
        if (port->config->port_config.type == LINKG_UART_PORT_TYPE_SBUS)
        {
            pthread_mutex_lock(&port->sbus_lock);
            depth = port->sbus_fifo.count;
            pthread_mutex_unlock(&port->sbus_lock);
        }

        if (samples == 0U)
        {
            low     = depth;
            high    = depth;
            sum     = depth;
            samples = 1U;
        }

        LINKG_LOG_DEBUG("UART: stats logical=%u fd=%d uart_rx=%" PRIu64 " uart_tx=%" PRIu64
                       " net_rx=%" PRIu64 " net_tx=%" PRIu64 " drops=%" PRIu64 " errors=%" PRIu64
                       " sbus_rx=%" PRIu64 " sbus_tx=%" PRIu64
                       " fifo=%u min=%u max=%u avg_x100=%" PRIu64 " target=%u interval=%uus empty_ticks=%" PRIu64,
                       port->config->uart_num, port->fd,
                       urx - port->last_uart_rx_bytes, utx - port->last_uart_tx_bytes,
                       nrx - port->last_net_rx_bytes, ntx - port->last_net_tx_bytes,
                       drop - port->last_drops, errors - port->last_errors,
                       frames_rx - port->last_sbus_rx_frames, frames_tx - port->last_sbus_tx_frames,
                       depth, low, high, (sum * 100U) / samples,
                       atomic_load(&manager->sbus_target_level), atomic_load(&port->sbus_interval_us),
                       empty - port->last_fifo_empty);

        port->last_uart_rx_bytes  = urx;
        port->last_uart_tx_bytes  = utx;
        port->last_net_rx_bytes   = nrx;
        port->last_net_tx_bytes   = ntx;
        port->last_drops          = drop;
        port->last_errors         = errors;
        port->last_fifo_empty     = empty;
        port->last_sbus_rx_frames = frames_rx;
        port->last_sbus_tx_frames = frames_tx;
        port->last_debug_ms       = now_ms;
        atomic_store(&port->debug_preview_budget, 3U);
    }
}
