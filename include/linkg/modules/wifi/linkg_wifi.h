/**
 * @file linkg_wifi.h
 * @brief LinkG Wi-Fi运行控制接口
 */

#ifndef LINKG_WIFI_H
#define LINKG_WIFI_H

#include "linkg_device_config.h"
#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_wifi_init(linkg_device_role_t role, const linkg_wifi_config_t *config);
int linkg_wifi_start(void);
int linkg_wifi_stop(void);
int linkg_wifi_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
