/**
 * @file stm32l0xx_hal_conf.h
 * @brief 精简 HAL 驱动选择与时钟常量
 */

#ifndef LINKG_STM32L0XX_HAL_CONF_H
#define LINKG_STM32L0XX_HAL_CONF_H                 // 头文件保护

/****************************** 模块配置 ******************************/

#define HAL_MODULE_ENABLED              1U         // HAL 公共层
#define HAL_CORTEX_MODULE_ENABLED       1U         // NVIC 与 SysTick
#define HAL_DMA_MODULE_ENABLED          1U         // HAL UART 的公共链接依赖，不开启 DMA
#define HAL_FLASH_MODULE_ENABLED        1U         // 时钟切换所需 Flash 配置
#define HAL_GPIO_MODULE_ENABLED         1U         // 数据引脚和模式控制
#define HAL_IWDG_MODULE_ENABLED         1U         // 独立看门狗
#define HAL_PWR_MODULE_ENABLED          1U         // 稳压器配置
#define HAL_RCC_MODULE_ENABLED          1U         // 系统及外设时钟
#define HAL_UART_MODULE_ENABLED         1U         // UART 初始化与停止
#define USE_HAL_UART_REGISTER_CALLBACKS 0U         // 使用静态 MSP 回调
#define HSE_VALUE                       8000000U   // 外部时钟默认频率，单位 Hz
#define HSE_STARTUP_TIMEOUT             100U       // 外部高速时钟启动超时，单位 ms
#define MSI_VALUE                       2097152U   // 复位 MSI 频率，单位 Hz
#define HSI_VALUE                       16000000U  // HSI 频率，单位 Hz
#define HSI48_VALUE                     48000000U  // HAL 公共定义，单位 Hz
#define LSI_VALUE                       37000U     // LSI 标称频率，单位 Hz
#define LSE_VALUE                       32768U     // 外部低速时钟频率，单位 Hz
#define LSE_STARTUP_TIMEOUT             5000U      // 外部低速时钟启动超时，单位 ms
#define VDD_VALUE                       3300U      // 供电电压，单位 mV
#define TICK_INT_PRIORITY               3U         // UART 中断优先于系统时基
#define USE_RTOS                        0U         // 裸机运行
#define PREFETCH_ENABLE                 0U         // 沿用旧工程 Flash 预取配置
#define PREREAD_ENABLE                  1U         // 开启 Flash 预读
#define BUFFER_CACHE_DISABLE            0U         // 保留 Flash 缓冲
#define assert_param(expr)              ((void)0U) // HAL 参数已由固定配置约束

/****************************** 驱动依赖 ******************************/

#include "stm32l0xx_hal_rcc.h"
#include "stm32l0xx_hal_gpio.h"
#include "stm32l0xx_hal_dma.h"
#include "stm32l0xx_hal_cortex.h"
#include "stm32l0xx_hal_flash.h"
#include "stm32l0xx_hal_pwr.h"
#include "stm32l0xx_hal_iwdg.h"
#include "stm32l0xx_hal_uart.h"

#endif
