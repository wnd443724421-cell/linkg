/**
 * @file wifi_driver_loader.h
 * @brief LinkG HI1105 Wi-Fi驱动加载及固件、INI资源选择接口
 */

#ifndef WIFI_DRIVER_LOADER_H
#define WIFI_DRIVER_LOADER_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 驱动加载 ******************************/

bool wifi_driver_is_loaded(void);
int  wifi_driver_load(void);

/****************************** 固件选择 ******************************/

int wifi_driver_prepare_firmware(uint8_t hardware_id);

/****************************** INI配置 ******************************/

int wifi_driver_prepare_ini(uint8_t hardware_id, linkg_wifi_work_mode_t work_mode, bool *changed);
int wifi_driver_update_ini(uint8_t hardware_id, linkg_wifi_work_mode_t work_mode);
int wifi_driver_update_ax_ini(linkg_device_role_t role, linkg_wifi_protocol_t protocol, bool *changed);

#ifdef __cplusplus
}
#endif

#endif // WIFI_DRIVER_LOADER_H
