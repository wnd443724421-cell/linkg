/**
 * @file stm32l0xx_it.c
 * @brief 异常、系统时基和 UART 中断转发
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "stm32l0xx_it.h"

#include "main.h"
#include "sbus_bridge.h"

/****************************** 中断入口 ******************************/

/**
 * @brief 处理不可屏蔽异常并进入安全故障状态。
 */
void NMI_Handler(void)
{
    Error_Handler();
}

/**
 * @brief 处理硬件异常并进入安全故障状态。
 */
void HardFault_Handler(void)
{
    Error_Handler();
}

/**
 * @brief 保留裸机环境的系统服务入口。
 */
void SVC_Handler(void)
{
}

/**
 * @brief 保留裸机环境的挂起服务入口。
 */
void PendSV_Handler(void)
{
}

/**
 * @brief 更新 HAL 毫秒时基。
 */
void SysTick_Handler(void)
{
    HAL_IncTick();
}

/**
 * @brief 推进 Host UART 的字节收发。
 */
void USART2_IRQHandler(void)
{
    sbus_bridge_uart_irq(BSP_UART_HOST);
}

/**
 * @brief 推进外部 SBUS UART 的字节收发。
 */
void LPUART1_IRQHandler(void)
{
    sbus_bridge_uart_irq(BSP_UART_SBUS);
}
