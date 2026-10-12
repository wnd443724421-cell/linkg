/**
 * @file linkg_gpio_manager.h
 * @brief LinkG GPIO硬件控制及指示灯管理接口
 */

#ifndef LINKG_GPIO_MANAGER_H
#define LINKG_GPIO_MANAGER_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_gpio_manager_init(void);
int linkg_gpio_manager_start(void);
int linkg_gpio_manager_stop(void);
int linkg_gpio_manager_deinit(void);

/****************************** 运行配置 ******************************/

int linkg_gpio_manager_set_role(linkg_device_role_t role);

/****************************** 硬件控制 ******************************/

int linkg_gpio_manager_set_sbus_enabled(bool enabled);
int linkg_gpio_manager_set_uart_rx_enabled(bool enabled);

/****************************** 硬件版本 ******************************/

int linkg_gpio_manager_get_hardware_bits(bool *hwid1, bool *hwid2);
int linkg_gpio_manager_get_hardware_id(uint8_t *hardware_id);

#ifdef __cplusplus
}
#endif

#endif // LINKG_GPIO_MANAGER_H
