
/**
 * @file main.c
 * @brief STM32系统时钟初始化与SBUS透明桥接入口
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "main.h"

#include "bsp_gpio.h"
#include "bsp_uart.h"
#include "sbus_bridge.h"

/****************************** 系统时钟 ******************************/

/**
 * @brief 使用内部HSI经PLL产生32MHz系统时钟，并为双UART提供PCLK1。
 */
static void system_clock_configure(void)
{
    RCC_OscInitTypeDef       oscillator = {0};
    RCC_ClkInitTypeDef       clock      = {0};
    RCC_PeriphCLKInitTypeDef peripheral = {0};

    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

    oscillator.OscillatorType       = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
    oscillator.HSIState             = RCC_HSI_ON;
    oscillator.HSICalibrationValue  = RCC_HSICALIBRATION_DEFAULT;
    oscillator.LSIState             = RCC_LSI_ON;
    oscillator.PLL.PLLState         = RCC_PLL_ON;
    oscillator.PLL.PLLSource        = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLMUL           = RCC_PLLMUL_4;
    oscillator.PLL.PLLDIV           = RCC_PLLDIV_2;

    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
    {
        Error_Handler();
    }

    clock.ClockType       = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource    = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider   = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider  = RCC_HCLK_DIV1;
    clock.APB2CLKDivider  = RCC_HCLK_DIV1;

    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_1) != HAL_OK)
    {
        Error_Handler();
    }

    peripheral.PeriphClockSelection  = RCC_PERIPHCLK_USART2 | RCC_PERIPHCLK_LPUART1;
    peripheral.Usart2ClockSelection  = RCC_USART2CLKSOURCE_PCLK1;
    peripheral.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;

    if (HAL_RCCEx_PeriphCLKConfig(&peripheral) != HAL_OK)
    {
        Error_Handler();
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化HAL、GPIO和SBUS桥接，并在主循环中处理运行状态。
 */
int main(void)
{
    HAL_Init();

    bsp_gpio_init();

    system_clock_configure();

    if (sbus_bridge_init() != 0)
    {
        Error_Handler();
    }

    while (1)
    {
        sbus_bridge_process();

        __WFI();
    }
}

/**
 * @brief 发生致命错误时停止UART并隔离硬件通路。
 *
 * 保持中断关闭，等待已启动的IWDG执行复位。
 */
void Error_Handler(void)
{
    __disable_irq();

    bsp_uart_stop();

    bsp_gpio_set_bypass(false);

    while (1)
    {
    }
}
