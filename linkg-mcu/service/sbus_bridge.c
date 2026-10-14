
/**
 * @file sbus_bridge.c
 * @brief 双向UART透明转发、FIFO管理、模式切换及错误统计
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "sbus_bridge.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "bsp_gpio.h"
#include "stm32l0xx_hal.h"

/****************************** 模块常量 ******************************/

#define SBUS_BRIDGE_RETRY_MS       100U                                                                  // UART初始化失败后的重试间隔
#define SBUS_BRIDGE_IWDG_RELOAD    249U                                                                  // 37kHz、256分频下约1.73秒
#define SBUS_BRIDGE_IWDG_FEED_MS   100U                                                                  // 主循环看门狗刷新间隔
#define SBUS_BRIDGE_BAD_RX_ERRORS (BSP_UART_ERROR_PARITY | BSP_UART_ERROR_FRAME | BSP_UART_ERROR_NOISE) // 不可信接收字节的错误标记

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t  data[SBUS_BRIDGE_FIFO_CAPACITY]; // 当前方向的待发送字节
    uint16_t head;                            // 下一个写入位置
    uint16_t tail;                            // 下一个发送位置
} sbus_bridge_fifo_t;

typedef struct
{
    sbus_bridge_fifo_t  fifo[BSP_UART_COUNT]; // 按源UART索引的双向发送队列
    sbus_bridge_stats_t stats;                // 桥接运行状态及累计统计
    IWDG_HandleTypeDef  watchdog;             // 独立看门狗句柄
    uint32_t            retry_tick;           // 最近一次UART启动尝试时间
    uint32_t            feed_tick;            // 最近一次看门狗刷新时间
    volatile bool       recover_requested;    // UART中断请求主循环恢复外设
} sbus_bridge_context_t;

/****************************** 全局上下文 ******************************/

static sbus_bridge_context_t g_bridge; // UART中断与主循环共享的桥接上下文

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取当前UART对应的另一侧端口。
 */
static bsp_uart_port_t sbus_bridge_peer(bsp_uart_port_t port)
{
    return port == BSP_UART_SBUS ? BSP_UART_HOST : BSP_UART_SBUS;
}

/**
 * @brief 计算环形队列的下一个索引。
 */
static uint16_t sbus_bridge_next(uint16_t position)
{
    return (uint16_t)((position + 1U) % SBUS_BRIDGE_FIFO_CAPACITY);
}

/**
 * @brief 尝试发送队首字节，成功写入TDR后推进队列。
 *
 * 调用方必须处于同优先级UART中断，或已经屏蔽相关中断。
 */
static void sbus_bridge_kick(bsp_uart_port_t source)
{
    sbus_bridge_fifo_t            *fifo;
    sbus_bridge_direction_stats_t *stats;
    bsp_uart_port_t                target;
    int                            result;

    fifo   = &g_bridge.fifo[source];
    stats  = &g_bridge.stats.direction[source];
    target = sbus_bridge_peer(source);

    if (stats->queued_bytes != 0U)
    {
        result = bsp_uart_try_write(target, fifo->data[fifo->tail]);

        if (result == 0)
        {
            fifo->tail = sbus_bridge_next(fifo->tail);

            stats->queued_bytes--;
            stats->tx_bytes++;
        }
        else if (result != -EAGAIN)
        {
            stats->tx_failures++;
            g_bridge.stats.last_error = result;
            g_bridge.recover_requested = true;
        }
    }

    bsp_uart_set_tx_irq(target, stats->queued_bytes != 0U && !g_bridge.recover_requested);
}

/**
 * @brief 记录UART接收错误，不推算硬件溢出的未知丢失字节数。
 */
static void sbus_bridge_record_errors(sbus_bridge_direction_stats_t *stats, uint32_t errors)
{
    stats->parity_errors  += (errors & BSP_UART_ERROR_PARITY) != 0U;
    stats->frame_errors   += (errors & BSP_UART_ERROR_FRAME) != 0U;
    stats->noise_errors   += (errors & BSP_UART_ERROR_NOISE) != 0U;
    stats->overrun_errors += (errors & BSP_UART_ERROR_OVERRUN) != 0U;
}

/**
 * @brief 停止UART数据驱动并清空两个方向的待发队列。
 *
 * 调用方必须已经屏蔽相关中断。
 */
static void sbus_bridge_stop_locked(void)
{
    uint32_t source;

    for (source = 0U; source < BSP_UART_COUNT; source++)
    {
        sbus_bridge_direction_stats_t *stats = &g_bridge.stats.direction[source];

        if (!bsp_uart_tx_complete(sbus_bridge_peer((bsp_uart_port_t)source)))
        {
            stats->tx_abort_events++;
        }

        stats->discarded_bytes += stats->queued_bytes;
        stats->queued_bytes     = 0U;

        g_bridge.fifo[source].head = 0U;
        g_bridge.fifo[source].tail = 0U;
    }

    bsp_uart_stop();

    g_bridge.recover_requested = false;
    g_bridge.stats.state       = SBUS_BRIDGE_ISOLATED;
}

/**
 * @brief 根据MCU_MODEL切换UART直通和SBUS透明桥接模式。
 *
 * HAL初始化期间保持SysTick中断可用。
 */
static void sbus_bridge_update_mode(uint32_t now)
{
    uint32_t saved_mask;
    int      result;

    if (!bsp_gpio_sbus_requested())
    {
        if (g_bridge.stats.state != SBUS_BRIDGE_BYPASS)
        {
            saved_mask = __get_PRIMASK();

            __disable_irq();

            sbus_bridge_stop_locked();
            bsp_gpio_set_bypass(true);

            g_bridge.stats.state = SBUS_BRIDGE_BYPASS;
            g_bridge.stats.mode_changes++;

            __set_PRIMASK(saved_mask);
        }

        return;
    }

    if (g_bridge.stats.state == SBUS_BRIDGE_RUNNING)
    {
        return;
    }

    if (g_bridge.stats.state == SBUS_BRIDGE_FAULT &&
        (uint32_t)(now - g_bridge.retry_tick) < SBUS_BRIDGE_RETRY_MS)
    {
        return;
    }

    bsp_gpio_set_bypass(false);

    g_bridge.retry_tick = now;

    result = bsp_uart_start();

    if (result != 0)
    {
        g_bridge.stats.start_failures++;
        g_bridge.stats.last_error = result;
        g_bridge.stats.state      = SBUS_BRIDGE_FAULT;

        return;
    }

    saved_mask = __get_PRIMASK();

    __disable_irq();

    if (bsp_gpio_sbus_requested())
    {
        g_bridge.stats.state = SBUS_BRIDGE_RUNNING;
        g_bridge.stats.mode_changes++;

        bsp_uart_enable_rx_irq();
    }
    else
    {
        sbus_bridge_stop_locked();
        bsp_gpio_set_bypass(true);

        g_bridge.stats.state = SBUS_BRIDGE_BYPASS;
        g_bridge.stats.mode_changes++;
    }

    __set_PRIMASK(saved_mask);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化双UART桥接上下文和独立看门狗。
 *
 * 调用前必须完成GPIO初始化。
 */
int sbus_bridge_init(void)
{
    bsp_uart_stop();

    bsp_gpio_set_bypass(false);

    memset(&g_bridge, 0, sizeof(g_bridge));

    g_bridge.watchdog.Instance       = IWDG;
    g_bridge.watchdog.Init.Prescaler = IWDG_PRESCALER_256;
    g_bridge.watchdog.Init.Reload    = SBUS_BRIDGE_IWDG_RELOAD;
    g_bridge.watchdog.Init.Window    = IWDG_WINDOW_DISABLE;

    if (HAL_IWDG_Init(&g_bridge.watchdog) != HAL_OK)
    {
        g_bridge.stats.state      = SBUS_BRIDGE_FAULT;
        g_bridge.stats.last_error = -ETIMEDOUT;

        return -ETIMEDOUT;
    }

    g_bridge.feed_tick = HAL_GetTick();

    sbus_bridge_update_mode(g_bridge.feed_tick);

    return 0;
}

/**
 * @brief 处理模式变化、UART故障恢复和看门狗刷新。
 */
void sbus_bridge_process(void)
{
    uint32_t now;
    uint32_t saved_mask;

    now        = HAL_GetTick();
    saved_mask = __get_PRIMASK();

    __disable_irq();

    if (g_bridge.recover_requested)
    {
        sbus_bridge_stop_locked();

        g_bridge.stats.state = SBUS_BRIDGE_FAULT;
        g_bridge.retry_tick  = now;
    }

    __set_PRIMASK(saved_mask);

    sbus_bridge_update_mode(now);

    if ((uint32_t)(now - g_bridge.feed_tick) >= SBUS_BRIDGE_IWDG_FEED_MS)
    {
        (void)HAL_IWDG_Refresh(&g_bridge.watchdog);

        g_bridge.feed_tick = now;
    }
}

/****************************** UART中断处理 ******************************/

/**
 * @brief 处理单次UART中断，按字节执行双向透明转发。
 *
 * 不等待完整SBUS帧，不执行协议校验或定时输出。
 */
void sbus_bridge_uart_irq(bsp_uart_port_t port)
{
    sbus_bridge_direction_stats_t *stats;
    sbus_bridge_fifo_t            *fifo;
    uint32_t                       errors = 0U;
    uint8_t                        byte   = 0U;
    int                            received;

    if (port != BSP_UART_SBUS && port != BSP_UART_HOST)
    {
        return;
    }

    if (g_bridge.stats.state != SBUS_BRIDGE_RUNNING)
    {
        return;
    }

    stats = &g_bridge.stats.direction[port];
    fifo  = &g_bridge.fifo[port];

    received = bsp_uart_receive(port, &byte, &errors);

    sbus_bridge_record_errors(stats, errors);

    if (received > 0)
    {
        stats->rx_bytes++;

        if ((errors & SBUS_BRIDGE_BAD_RX_ERRORS) != 0U)
        {
            stats->bad_rx_bytes++;
        }
        else
        {
            // 先尝试发送队首字节，减少目标UART暂时繁忙导致的队列占用。
            sbus_bridge_kick(port);

            if (stats->queued_bytes == SBUS_BRIDGE_FIFO_CAPACITY)
            {
                stats->overflow_bytes++;
            }
            else
            {
                fifo->data[fifo->head] = byte;
                fifo->head            = sbus_bridge_next(fifo->head);

                stats->queued_bytes++;

                if (stats->queued_bytes > stats->high_water_bytes)
                {
                    stats->high_water_bytes = stats->queued_bytes;
                }
            }
        }
    }
    else if (received < 0)
    {
        g_bridge.stats.last_error = received;
        g_bridge.recover_requested = true;
    }

    sbus_bridge_kick(port);
    sbus_bridge_kick(sbus_bridge_peer(port));
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取包含两个方向统计数据的运行状态快照。
 *
 * 获取快照期间屏蔽中断，保持统计字段一致。
 */
int sbus_bridge_get_stats(sbus_bridge_stats_t *stats)
{
    uint32_t saved_mask;

    if (stats == NULL)
    {
        return -EINVAL;
    }

    saved_mask = __get_PRIMASK();

    __disable_irq();

    *stats = g_bridge.stats;

    __set_PRIMASK(saved_mask);

    return 0;
}
