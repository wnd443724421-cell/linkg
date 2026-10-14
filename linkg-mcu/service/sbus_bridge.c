/**
 * @file sbus_bridge.c
 * @brief 按字节转发、双向 FIFO、模式切换及错误统计
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

#define SBUS_BRIDGE_RETRY_MS      100U                                                                  // UART 初始化失败的重试间隔
#define SBUS_BRIDGE_IWDG_RELOAD   249U                                                                  // 37kHz、256 分频下约 1.73 秒
#define SBUS_BRIDGE_IWDG_FEED_MS  100U                                                                  // 仅主循环定期喂狗
#define SBUS_BRIDGE_BAD_RX_ERRORS (BSP_UART_ERROR_PARITY | BSP_UART_ERROR_FRAME | BSP_UART_ERROR_NOISE) // 不可信字节标记

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t  data[SBUS_BRIDGE_FIFO_CAPACITY]; // 本方向独占的待发送字节
    uint16_t head;                            // 下一个写入位置
    uint16_t tail;                            // 下一个发送位置
} sbus_bridge_fifo_t;

typedef struct
{
    sbus_bridge_fifo_t  fifo[BSP_UART_COUNT]; // 按源端口索引，UART 同级中断内访问
    sbus_bridge_stats_t stats;                // 主循环读取时短暂屏蔽中断
    IWDG_HandleTypeDef  watchdog;             // 独立看门狗实例
    uint32_t            retry_tick;           // 最近一次启动尝试时刻
    uint32_t            feed_tick;            // 最近一次喂狗时刻
    volatile bool       recover_requested;    // ISR 请求主循环恢复外设
} sbus_bridge_context_t;

/****************************** 全局上下文 ******************************/

static sbus_bridge_context_t g_bridge; // 由同级 UART ISR 和受保护的主循环共同持有

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取另一个 UART 端口。
 */
static bsp_uart_port_t sbus_bridge_peer(bsp_uart_port_t port)
{
    return port == BSP_UART_SBUS ? BSP_UART_HOST : BSP_UART_SBUS;
}

/**
 * @brief 推进环形队列位置，不改变字节顺序。
 */
static uint16_t sbus_bridge_next(uint16_t position)
{
    return (uint16_t)((position + 1U) % SBUS_BRIDGE_FIFO_CAPACITY);
}

/**
 * @brief 尝试发送队首一个字节，只在 TDR 接受后推进队列。
 *
 * 调用者须处于同级 UART ISR 或已经屏蔽中断。
 */
static void sbus_bridge_kick(bsp_uart_port_t source)
{
    sbus_bridge_fifo_t *fifo = &g_bridge.fifo[source];
    sbus_bridge_direction_stats_t *stats = &g_bridge.stats.direction[source];
    bsp_uart_port_t target = sbus_bridge_peer(source);
    int result;

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
 * @brief 记录 UART 错误，接收溢出不推算未知的丢失字节数。
 */
static void sbus_bridge_record_errors(sbus_bridge_direction_stats_t *stats, uint32_t errors)
{
    stats->parity_errors += (errors & BSP_UART_ERROR_PARITY) != 0U;
    stats->frame_errors += (errors & BSP_UART_ERROR_FRAME) != 0U;
    stats->noise_errors += (errors & BSP_UART_ERROR_NOISE) != 0U;
    stats->overrun_errors += (errors & BSP_UART_ERROR_OVERRUN) != 0U;
}

/**
 * @brief 停止 MCU 数据驱动并清理两个方向，调用者须屏蔽中断。
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
        stats->queued_bytes = 0U;
        g_bridge.fifo[source].head = 0U;
        g_bridge.fifo[source].tail = 0U;
    }
    bsp_uart_stop();
    g_bridge.recover_requested = false;
    g_bridge.stats.state = SBUS_BRIDGE_ISOLATED;
}

/**
 * @brief 在主循环中选择通路，HAL 初始化期间保留 SysTick 中断。
 */
static void sbus_bridge_update_mode(uint32_t now)
{
    uint32_t saved_mask;
    int result;

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
    if (g_bridge.stats.state == SBUS_BRIDGE_FAULT && (uint32_t)(now - g_bridge.retry_tick) < SBUS_BRIDGE_RETRY_MS)
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
        g_bridge.stats.state = SBUS_BRIDGE_FAULT;
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
 * @brief 初始化桥接上下文和独立看门狗，调用前须已初始化 GPIO。
 */
int sbus_bridge_init(void)
{
    bsp_uart_stop();
    bsp_gpio_set_bypass(false);
    memset(&g_bridge, 0, sizeof(g_bridge));
    g_bridge.watchdog.Instance = IWDG;
    g_bridge.watchdog.Init.Prescaler = IWDG_PRESCALER_256;
    g_bridge.watchdog.Init.Reload = SBUS_BRIDGE_IWDG_RELOAD;
    g_bridge.watchdog.Init.Window = IWDG_WINDOW_DISABLE;
    if (HAL_IWDG_Init(&g_bridge.watchdog) != HAL_OK)
    {
        g_bridge.stats.state = SBUS_BRIDGE_FAULT;
        g_bridge.stats.last_error = -ETIMEDOUT;
        return -ETIMEDOUT;
    }
    g_bridge.feed_tick = HAL_GetTick();
    sbus_bridge_update_mode(g_bridge.feed_tick);
    return 0;
}

/**
 * @brief 处理模式变化及异常重启，并且仅在主循环喂狗。
 */
void sbus_bridge_process(void)
{
    uint32_t now = HAL_GetTick();
    uint32_t saved_mask = __get_PRIMASK();

    __disable_irq();
    if (g_bridge.recover_requested)
    {
        sbus_bridge_stop_locked();
        g_bridge.stats.state = SBUS_BRIDGE_FAULT;
        g_bridge.retry_tick = now;
    }
    __set_PRIMASK(saved_mask);
    sbus_bridge_update_mode(now);
    if ((uint32_t)(now - g_bridge.feed_tick) >= SBUS_BRIDGE_IWDG_FEED_MS)
    {
        (void)HAL_IWDG_Refresh(&g_bridge.watchdog);
        g_bridge.feed_tick = now;
    }
}

/**
 * @brief 每次中断接收至多一字节并推进两个方向，禁止等待和协议处理。
 */
void sbus_bridge_uart_irq(bsp_uart_port_t port)
{
    sbus_bridge_direction_stats_t *stats;
    sbus_bridge_fifo_t *fifo;
    uint32_t errors = 0U;
    uint8_t byte = 0U;
    int received;

    if (port != BSP_UART_SBUS && port != BSP_UART_HOST)
    {
        return;
    }
    if (g_bridge.stats.state != SBUS_BRIDGE_RUNNING)
    {
        return;
    }
    stats = &g_bridge.stats.direction[port];
    fifo = &g_bridge.fifo[port];
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
            // 先尝试腾出一个位置，避免目标刚恢复 TXE 时误报队列满。
            sbus_bridge_kick(port);
            if (stats->queued_bytes == SBUS_BRIDGE_FIFO_CAPACITY)
            {
                stats->overflow_bytes++;
            }
            else
            {
                fifo->data[fifo->head] = byte;
                fifo->head = sbus_bridge_next(fifo->head);
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
 * @brief 获取两个方向的一致快照，不清零累计计数。
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
