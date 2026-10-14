/**
 * @file test_bridge.c
 * @brief 透明转发、背压、错误恢复及模式切换主机测试
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "sbus_bridge.h"

#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "bsp_gpio.h"
#include "stm32l0xx_hal.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    bool     requested;                   // 模式输入电平
    bool     bypass;                      // U16 是否导通
    bool     active;                      // MCU 数据引脚是否正在驱动
    bool     flip_during_start;           // 在启动中模拟输入变化
    bool     ready[BSP_UART_COUNT];       // 目标 TXE 状态
    bool     complete[BSP_UART_COUNT];    // 目标 TC 状态
    bool     tx_irq[BSP_UART_COUNT];      // TXE 中断使能状态
    bool     rx_pending[BSP_UART_COUNT];  // RDR 待接收状态
    uint8_t  rx_byte[BSP_UART_COUNT];     // RDR 字节
    uint32_t rx_errors[BSP_UART_COUNT];   // ISR 硬件错误
    uint8_t  sent[BSP_UART_COUNT][16384]; // TDR 写入历史
    uint32_t sent_count[BSP_UART_COUNT];  // TDR 写入次数
    uint32_t tick;                        // 模拟毫秒时基
    uint32_t mask;                        // 模拟 PRIMASK
    uint32_t starts;                      // 初始化尝试次数
    uint32_t feeds;                       // 主循环喂狗次数
    int      start_result;                // 下一次初始化结果
    int      write_result;                // 非忙发送错误注入
} test_context_t;

/****************************** 全局上下文 ******************************/

static test_context_t g_test; // 每个测试独占并重置的硬件替身

/****************************** 硬件替身 ******************************/

/**
 * @brief 返回模拟毫秒时基。
 */
uint32_t HAL_GetTick(void)
{
    return g_test.tick;
}

/**
 * @brief 返回模拟中断屏蔽状态。
 */
uint32_t __get_PRIMASK(void)
{
    return g_test.mask;
}

/**
 * @brief 屏蔽模拟中断。
 */
void __disable_irq(void)
{
    g_test.mask = 1U;
}

/**
 * @brief 恢复模拟中断状态。
 */
void __set_PRIMASK(uint32_t mask)
{
    g_test.mask = mask;
}

/**
 * @brief 检查独立看门狗的实际配置。
 */
int HAL_IWDG_Init(IWDG_HandleTypeDef *watchdog)
{
    assert(watchdog->Instance == IWDG);
    assert(watchdog->Init.Prescaler == 256U);
    assert(watchdog->Init.Reload == 249U);
    assert(watchdog->Init.Window == 4095U);
    return HAL_OK;
}

/**
 * @brief 记录主循环喂狗行为。
 */
int HAL_IWDG_Refresh(IWDG_HandleTypeDef *watchdog)
{
    assert(watchdog->Instance == IWDG);
    g_test.feeds++;
    return HAL_OK;
}

/**
 * @brief 断言接通 U16 前 MCU 已经停止驱动。
 */
void bsp_gpio_set_bypass(bool enabled)
{
    assert(!enabled || !g_test.active);
    g_test.bypass = enabled;
}

/**
 * @brief 返回模拟模式电平。
 */
bool bsp_gpio_sbus_requested(void)
{
    return g_test.requested;
}

/**
 * @brief 断言 MCU 驱动前 U16 已经断开，并注入启动结果。
 */
int bsp_uart_start(void)
{
    assert(!g_test.bypass);
    assert(g_test.mask == 0U);
    g_test.starts++;
    if (g_test.flip_during_start)
    {
        g_test.requested = false;
    }
    g_test.active = g_test.start_result == 0;
    return g_test.start_result;
}

/**
 * @brief 模拟关闭 UART 并释放引脚。
 */
void bsp_uart_stop(void)
{
    g_test.active = false;
    memset(g_test.tx_irq, 0, sizeof(g_test.tx_irq));
    memset(g_test.rx_pending, 0, sizeof(g_test.rx_pending));
    memset(g_test.rx_errors, 0, sizeof(g_test.rx_errors));
    g_test.complete[0] = true;
    g_test.complete[1] = true;
}

/**
 * @brief 验证接收中断只在两路外设就绪后开启。
 */
void bsp_uart_enable_rx_irq(void)
{
    assert(g_test.active);
    assert(!g_test.bypass);
    assert(g_test.mask == 1U);
}

/**
 * @brief 消费一次模拟 RDR 与硬件错误。
 */
int bsp_uart_receive(bsp_uart_port_t port, uint8_t *byte, uint32_t *errors)
{
    int received = g_test.rx_pending[port] ? 1 : 0;

    assert(g_test.active);
    *byte = g_test.rx_byte[port];
    *errors = g_test.rx_errors[port];
    g_test.rx_pending[port] = false;
    g_test.rx_errors[port] = 0U;
    return received;
}

/**
 * @brief 模拟 TDR 只能在 TXE 有效时接受一个字节。
 */
int bsp_uart_try_write(bsp_uart_port_t port, uint8_t byte)
{
    assert(g_test.active);
    if (g_test.write_result != 0)
    {
        return g_test.write_result;
    }
    if (!g_test.ready[port])
    {
        return -EAGAIN;
    }
    assert(g_test.sent_count[port] < sizeof(g_test.sent[port]));
    g_test.sent[port][g_test.sent_count[port]++] = byte;
    g_test.ready[port] = false;
    g_test.complete[port] = false;
    return 0;
}

/**
 * @brief 保存目标 TXE 中断状态。
 */
void bsp_uart_set_tx_irq(bsp_uart_port_t port, bool enabled)
{
    g_test.tx_irq[port] = enabled;
}

/**
 * @brief 返回模拟停止位发送完成状态。
 */
bool bsp_uart_tx_complete(bsp_uart_port_t port)
{
    return !g_test.active || g_test.complete[port];
}

/****************************** 内部辅助 ******************************/

/**
 * @brief 使用新的硬件状态启动一个测试。
 */
static void test_reset(bool requested)
{
    memset(&g_test, 0, sizeof(g_test));
    g_test.requested = requested;
    g_test.ready[0] = true;
    g_test.ready[1] = true;
    assert(sbus_bridge_init() == 0);
    assert(g_test.mask == 0U);
}

/**
 * @brief 注入一个实际接收字节并执行对应 IRQ。
 */
static void test_receive(bsp_uart_port_t source, uint8_t byte, uint32_t errors)
{
    g_test.rx_pending[source] = true;
    g_test.rx_byte[source] = byte;
    g_test.rx_errors[source] = errors;
    sbus_bridge_uart_irq(source);
}

/**
 * @brief 模拟目标 TDR 腾空并触发中断。
 */
static void test_tx_ready(bsp_uart_port_t target)
{
    g_test.ready[target] = true;
    sbus_bridge_uart_irq(target);
}

/****************************** 测试用例 ******************************/

/**
 * @brief 双向逐字节验证全部字节值及多次队列回绕，不使用任何帧结构。
 */
static void test_transparency(void)
{
    uint32_t index;
    sbus_bridge_stats_t stats;

    test_reset(true);
    for (index = 0U; index < 10000U; index++)
    {
        g_test.ready[BSP_UART_HOST] = true;
        test_receive(BSP_UART_SBUS, (uint8_t)index, 0U);
        g_test.ready[BSP_UART_SBUS] = true;
        test_receive(BSP_UART_HOST, (uint8_t)(index ^ 0xA5U), 0U);
        assert(g_test.sent[BSP_UART_HOST][index] == (uint8_t)index);
        assert(g_test.sent[BSP_UART_SBUS][index] == (uint8_t)(index ^ 0xA5U));
    }
    assert(g_test.sent_count[0] == 10000U);
    assert(g_test.sent_count[1] == 10000U);
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(stats.direction[0].queued_bytes == 0U);
    assert(stats.direction[1].queued_bytes == 0U);
    assert(stats.direction[0].high_water_bytes == 1U);
    assert(g_test.feeds == 0U);
}

/**
 * @brief 验证双向独立 FIFO、忙状态、溢出丢新字节和逐字节进度。
 */
static void test_overflow_and_progress(void)
{
    uint32_t index;
    sbus_bridge_stats_t stats;

    test_reset(true);
    g_test.ready[0] = false;
    g_test.ready[1] = false;
    for (index = 0U; index < SBUS_BRIDGE_FIFO_CAPACITY + 9U; index++)
    {
        test_receive(BSP_UART_SBUS, (uint8_t)index, 0U);
        test_receive(BSP_UART_HOST, (uint8_t)(index + 64U), 0U);
    }
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(stats.direction[0].overflow_bytes == 9U);
    assert(stats.direction[1].overflow_bytes == 9U);
    assert(g_test.sent_count[0] == 0U);
    assert(g_test.sent_count[1] == 0U);
    for (index = 0U; index < SBUS_BRIDGE_FIFO_CAPACITY; index++)
    {
        test_tx_ready(BSP_UART_HOST);
        assert(g_test.sent_count[BSP_UART_HOST] == index + 1U);
        assert(g_test.sent[BSP_UART_HOST][index] == index);
    }
    assert(g_test.sent_count[BSP_UART_SBUS] == 0U);
    assert(!g_test.tx_irq[BSP_UART_HOST]);
    assert(g_test.tx_irq[BSP_UART_SBUS]);
    for (index = 0U; index < SBUS_BRIDGE_FIFO_CAPACITY; index++)
    {
        test_tx_ready(BSP_UART_SBUS);
        assert(g_test.sent[BSP_UART_SBUS][index] == index + 64U);
    }
    assert(!g_test.tx_irq[BSP_UART_SBUS]);
    g_test.ready[BSP_UART_HOST] = true;
    test_receive(BSP_UART_SBUS, 0xFEU, 0U);
    assert(g_test.sent[BSP_UART_HOST][SBUS_BRIDGE_FIFO_CAPACITY] == 0xFEU);
}

/**
 * @brief 验证 PE、FE、NE 丢弃不可信字节，ORE 保留当前真实数据。
 */
static void test_uart_errors(void)
{
    sbus_bridge_stats_t stats;

    test_reset(true);
    test_receive(BSP_UART_SBUS, 0x99U, BSP_UART_ERROR_PARITY | BSP_UART_ERROR_FRAME | BSP_UART_ERROR_NOISE);
    assert(g_test.sent_count[BSP_UART_HOST] == 0U);
    test_receive(BSP_UART_SBUS, 0x42U, BSP_UART_ERROR_OVERRUN);
    assert(g_test.sent[BSP_UART_HOST][0] == 0x42U);
    g_test.rx_errors[BSP_UART_SBUS] = BSP_UART_ERROR_OVERRUN;
    sbus_bridge_uart_irq(BSP_UART_SBUS);
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(stats.direction[0].rx_bytes == 2U);
    assert(stats.direction[0].bad_rx_bytes == 1U);
    assert(stats.direction[0].parity_errors == 1U);
    assert(stats.direction[0].frame_errors == 1U);
    assert(stats.direction[0].noise_errors == 1U);
    assert(stats.direction[0].overrun_errors == 2U);
    g_test.ready[BSP_UART_HOST] = true;
    test_receive(BSP_UART_SBUS, 0x24U, 0U);
    assert(g_test.sent[BSP_UART_HOST][1] == 0x24U);
}

/**
 * @brief 验证停止先释放引脚、清理残留数据并记录在途截断。
 */
static void test_mode_switch(void)
{
    sbus_bridge_stats_t stats;

    test_reset(false);
    assert(g_test.bypass);
    assert(!g_test.active);
    g_test.requested = true;
    sbus_bridge_process();
    test_receive(BSP_UART_SBUS, 1U, 0U);
    test_receive(BSP_UART_SBUS, 2U, 0U);
    test_receive(BSP_UART_SBUS, 3U, 0U);
    g_test.requested = false;
    sbus_bridge_process();
    assert(g_test.bypass);
    assert(!g_test.active);
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(stats.direction[0].discarded_bytes == 2U);
    assert(stats.direction[0].tx_abort_events == 1U);
    assert(stats.state == SBUS_BRIDGE_BYPASS);
    g_test.requested = true;
    sbus_bridge_process();
    test_tx_ready(BSP_UART_HOST);
    assert(g_test.sent_count[BSP_UART_HOST] == 1U);
    test_receive(BSP_UART_SBUS, 4U, 0U);
    assert(g_test.sent[BSP_UART_HOST][1] == 4U);
}

/**
 * @brief 验证启动失败限频重试、启动中模式变化及计时回绕。
 */
static void test_start_recovery(void)
{
    sbus_bridge_stats_t stats;

    test_reset(false);
    g_test.tick = UINT32_MAX - 50U;
    g_test.start_result = -ETIMEDOUT;
    g_test.requested = true;
    sbus_bridge_process();
    assert(g_test.starts == 1U);
    assert(!g_test.bypass);
    assert(!g_test.active);
    g_test.tick += 99U;
    sbus_bridge_process();
    assert(g_test.starts == 1U);
    g_test.tick++;
    g_test.start_result = 0;
    sbus_bridge_process();
    assert(g_test.starts == 2U);
    assert(g_test.active);
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(stats.start_failures == 1U);
    assert(stats.last_error == -ETIMEDOUT);

    test_reset(false);
    g_test.flip_during_start = true;
    g_test.requested = true;
    sbus_bridge_process();
    assert(g_test.bypass);
    assert(!g_test.active);
}

/**
 * @brief 验证发送硬故障不消费队首且由主循环重新初始化。
 */
static void test_tx_recovery(void)
{
    sbus_bridge_stats_t stats;

    test_reset(true);
    g_test.write_result = -EIO;
    test_receive(BSP_UART_SBUS, 0xAAU, 0U);
    assert(g_test.sent_count[1] == 0U);
    assert(!g_test.tx_irq[1]);
    sbus_bridge_process();
    assert(!g_test.active);
    assert(!g_test.bypass);
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(stats.direction[0].discarded_bytes == 1U);
    assert(stats.direction[0].tx_failures == 1U);
    g_test.write_result = 0;
    g_test.tick += 100U;
    sbus_bridge_process();
    test_receive(BSP_UART_SBUS, 0xBBU, 0U);
    assert(g_test.sent[1][0] == 0xBBU);
    assert(g_test.feeds == 1U);
}

/**
 * @brief 验证无效参数和统计快照保留调用者的中断屏蔽状态。
 */
static void test_status(void)
{
    sbus_bridge_stats_t stats;

    test_reset(true);
    assert(sbus_bridge_get_stats(NULL) == -EINVAL);
    sbus_bridge_uart_irq(BSP_UART_COUNT);
    sbus_bridge_uart_irq((bsp_uart_port_t)-1);
    g_test.mask = 1U;
    assert(sbus_bridge_get_stats(&stats) == 0);
    assert(g_test.mask == 1U);
    g_test.mask = 0U;
    g_test.tick = 99U;
    sbus_bridge_process();
    assert(g_test.feeds == 0U);
    g_test.tick = 100U;
    sbus_bridge_process();
    assert(g_test.feeds == 1U);
}

/****************************** 测试入口 ******************************/

/**
 * @brief 运行透明转发与异常路径测试。
 */
int main(void)
{
    test_transparency();
    test_overflow_and_progress();
    test_uart_errors();
    test_mode_switch();
    test_start_recovery();
    test_tx_recovery();
    test_status();
    puts("PASS: 7 bridge tests; 20000 ordered bytes; overflow, errors, switching, retries and watchdog");
    return 0;
}
