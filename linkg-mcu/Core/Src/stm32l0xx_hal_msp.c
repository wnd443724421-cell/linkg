/**
 * @file stm32l0xx_hal_msp.c
 * @brief HAL 公共系统时钟支持
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "main.h"

/****************************** 生命周期 ******************************/

/**
 * @brief 开启电源控制与系统配置时钟，UART 资源由 BSP 管理。
 */
void HAL_MspInit(void)
{
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    __HAL_RCC_PWR_CLK_ENABLE();
}
