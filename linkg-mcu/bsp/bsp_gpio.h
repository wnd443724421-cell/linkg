/**
 * @file bsp_gpio.h
 * @brief 模式输入与直通开关接口
 */

#ifndef LINKG_BSP_GPIO_H
#define LINKG_BSP_GPIO_H  // 头文件保护

#include <stdbool.h>

/****************************** 生命周期 ******************************/

void bsp_gpio_init(void);

/****************************** 配置接口 ******************************/

void bsp_gpio_set_bypass(bool enabled);

/****************************** 状态查询 ******************************/

bool bsp_gpio_sbus_requested(void);

#endif
