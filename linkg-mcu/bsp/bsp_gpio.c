/**
 * @file bsp_gpio.c
 * @brief 模式输入与 U16 直通开关控制
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "bsp_gpio.h"

#include "stm32l0xx_hal.h"

/****************************** 模块常量 ******************************/

#define BSP_GPIO_MODEL_PIN GPIO_PIN_1 // PA1 模式输入
#define BSP_GPIO_CTL_PIN   GPIO_PIN_1 // PB1 直通开关

/****************************** 生命周期 ******************************/

/**
 * @brief 在使能 PB1 输出前预置低电平，隔离直通路径。
 */
void bsp_gpio_init(void)
{
    GPIO_InitTypeDef config = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    HAL_GPIO_WritePin(GPIOB, BSP_GPIO_CTL_PIN, GPIO_PIN_RESET);
    config.Pin   = BSP_GPIO_CTL_PIN;
    config.Mode  = GPIO_MODE_OUTPUT_PP;
    config.Pull  = GPIO_NOPULL;
    config.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GPIOB, &config);

    config.Pin = BSP_GPIO_MODEL_PIN;
    config.Mode = GPIO_MODE_INPUT;
    config.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(GPIOA, &config);
}

/****************************** 配置接口 ******************************/

/**
 * @brief 控制 U16，高电平导通；调用者须先释放 MCU 数据引脚。
 */
void bsp_gpio_set_bypass(bool enabled)
{
    HAL_GPIO_WritePin(GPIOB, BSP_GPIO_CTL_PIN, enabled ? GPIO_PIN_SET : GPIO_PIN_RESET);
    __DSB();
}

/****************************** 状态查询 ******************************/

/**
 * @brief 读取 PA1，高电平表示请求 MCU 桥接。
 */
bool bsp_gpio_sbus_requested(void)
{
    return HAL_GPIO_ReadPin(GPIOA, BSP_GPIO_MODEL_PIN) == GPIO_PIN_SET;
}
