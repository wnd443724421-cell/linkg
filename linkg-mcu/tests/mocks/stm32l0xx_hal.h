/**
 * @file stm32l0xx_hal.h
 * @brief 主机测试所需的最小 HAL 时基与看门狗替身
 */

#ifndef LINKG_TEST_HAL_H
#define LINKG_TEST_HAL_H                // 头文件保护

#include <stdint.h>

/****************************** 模块常量 ******************************/

#define IWDG                ((void *)1) // 测试看门狗标识
#define IWDG_PRESCALER_256  256U        // 测试分频常量
#define IWDG_WINDOW_DISABLE 4095U       // 无窗口限制
#define HAL_OK              0           // 测试成功状态

/****************************** 类型定义 ******************************/

typedef struct
{
    uint32_t Prescaler; // 看门狗分频
    uint32_t Reload;    // 看门狗装载值
    uint32_t Window;    // 看门狗窗口值
} IWDG_InitTypeDef;

typedef struct
{
    void             *Instance; // 替身实例标识
    IWDG_InitTypeDef Init;      // 被测配置
} IWDG_HandleTypeDef;

/****************************** 测试接口 ******************************/

uint32_t HAL_GetTick(void);
uint32_t __get_PRIMASK(void);
void     __disable_irq(void);
void     __set_PRIMASK(uint32_t mask);
int      HAL_IWDG_Init(IWDG_HandleTypeDef *watchdog);
int      HAL_IWDG_Refresh(IWDG_HandleTypeDef *watchdog);

#endif
