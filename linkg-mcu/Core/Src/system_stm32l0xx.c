/**
 * @file system_stm32l0xx.c
 * @brief STM32L031 CMSIS 系统启动与时钟频率计算
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "stm32l0xx.h"

/**
 * Derived from STMicroelectronics system_stm32l0xx.c (2016).
 * Copyright (c) 2016 STMicroelectronics. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0
 */

/****************************** 模块常量 ******************************/

#define SYSTEM_HSI_HZ 16000000U // 内部高速时钟频率
#define SYSTEM_HSE_HZ 8000000U  // 保留 CMSIS 外部时钟计算默认值

/****************************** 全局变量 ******************************/

uint32_t SystemCoreClock = 2097152U; // CMSIS 规定的导出符号，复位后的 MSI 时钟
const uint8_t AHBPrescTable[16] = {0U, 0U, 0U, 0U, 0U, 0U, 0U, 0U, 1U, 2U, 3U, 4U, 6U, 7U, 8U, 9U}; // HAL 分频表
const uint8_t APBPrescTable[8] = {0U, 0U, 0U, 0U, 1U, 2U, 3U, 4U}; // HAL 分频表
const uint8_t PLLMulTable[9] = {3U, 4U, 6U, 8U, 12U, 16U, 24U, 32U, 48U}; // HAL 倍频表

/****************************** 生命周期 ******************************/

/**
 * @brief 设置无引导程序固件的 Flash 向量表地址。
 */
void SystemInit(void)
{
    SCB->VTOR = FLASH_BASE;
}

/**
 * @brief 根据 RCC 实际时钟源与分频更新 CMSIS 核心频率。
 */
void SystemCoreClockUpdate(void)
{
    uint32_t source = RCC->CFGR & RCC_CFGR_SWS;
    uint32_t hsi_hz = (RCC->CR & RCC_CR_HSIDIVF) != 0U ? SYSTEM_HSI_HZ / 4U : SYSTEM_HSI_HZ;
    uint32_t multiplier;
    uint32_t divider;
    uint32_t range;

    switch (source)
    {
        case RCC_CFGR_SWS_MSI:
            range = (RCC->ICSCR & RCC_ICSCR_MSIRANGE) >> RCC_ICSCR_MSIRANGE_Pos;
            SystemCoreClock = 32768U << (range + 1U);
            break;

        case RCC_CFGR_SWS_HSI:
            SystemCoreClock = hsi_hz;
            break;

        case RCC_CFGR_SWS_HSE:
            SystemCoreClock = SYSTEM_HSE_HZ;
            break;

        default:
            multiplier = PLLMulTable[(RCC->CFGR & RCC_CFGR_PLLMUL) >> RCC_CFGR_PLLMUL_Pos];
            divider = ((RCC->CFGR & RCC_CFGR_PLLDIV) >> RCC_CFGR_PLLDIV_Pos) + 1U;
            SystemCoreClock = (RCC->CFGR & RCC_CFGR_PLLSRC) != 0U ? SYSTEM_HSE_HZ : hsi_hz;
            SystemCoreClock = SystemCoreClock * multiplier / divider;
            break;
    }
    SystemCoreClock >>= AHBPrescTable[(RCC->CFGR & RCC_CFGR_HPRE) >> RCC_CFGR_HPRE_Pos];
}
