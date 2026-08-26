/**
 * @file wifi_driver_loader.h
 * @brief LinkG HI1105 Wi-Fi驱动加载及INI配置接口
 */

#ifndef WIFI_DRIVER_LOADER_H
#define WIFI_DRIVER_LOADER_H

#include <stdbool.h>

#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 驱动加载 ******************************/

bool wifi_driver_is_loaded(void);
int  wifi_driver_load(void);

/****************************** INI配置 ******************************/

int wifi_driver_prepare_ini(linkg_wifi_work_mode_t work_mode, bool *changed);
int wifi_driver_update_ini(linkg_wifi_work_mode_t work_mode);

#ifdef __cplusplus
}
#endif

#endif
