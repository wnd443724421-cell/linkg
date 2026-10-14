/**
 * @file main.c
 * @brief HAL、系统时钟与透明桥接应用入口
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "main.h"

#include "bsp_gpio.h"
#include "bsp_uart.h"
#include "sbus_bridge.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 使用内部 HSI 经 PLL 产生 32MHz，并给两个 UART 提供 PCLK1。
 */
static void system_clock_configure(void)
{
    RCC_OscInitTypeDef oscillator = {0};
    RCC_ClkInitTypeDef clock = {0};
    RCC_PeriphCLKInitTypeDef peripheral = {0};

    __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);
    oscillator.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_LSI;
    oscillator.HSIState = RCC_HSI_ON;
    oscillator.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
    oscillator.LSIState = RCC_LSI_ON;
    oscillator.PLL.PLLState = RCC_PLL_ON;
    oscillator.PLL.PLLSource = RCC_PLLSOURCE_HSI;
    oscillator.PLL.PLLMUL = RCC_PLLMUL_4;
    oscillator.PLL.PLLDIV = RCC_PLLDIV_2;
    if (HAL_RCC_OscConfig(&oscillator) != HAL_OK)
    {
        Error_Handler();
    }

    clock.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2;
    clock.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
    clock.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clock.APB1CLKDivider = RCC_HCLK_DIV1;
    clock.APB2CLKDivider = RCC_HCLK_DIV1;
    if (HAL_RCC_ClockConfig(&clock, FLASH_LATENCY_1) != HAL_OK)
    {
        Error_Handler();
    }

    peripheral.PeriphClockSelection = RCC_PERIPHCLK_USART2 | RCC_PERIPHCLK_LPUART1;
    peripheral.Usart2ClockSelection = RCC_USART2CLKSOURCE_PCLK1;
    peripheral.Lpuart1ClockSelection = RCC_LPUART1CLKSOURCE_PCLK1;
    if (HAL_RCCEx_PeriphCLKConfig(&peripheral) != HAL_OK)
    {
        Error_Handler();
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 尽早隔离通路，启动桥接后在中断之间休眠。
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
 * @brief 致命错误时释放 MCU 数据线并保持隔离，等待已启动的 IWDG 复位。
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
