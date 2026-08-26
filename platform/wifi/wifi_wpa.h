/**
 * @file wifi_wpa.h
 * @brief LinkG wpa_supplicant服务内部接口
 */

#ifndef WIFI_WPA_H
#define WIFI_WPA_H

#include <stdint.h>

#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置同步 ******************************/

int wifi_wpa_update_config(const linkg_wifi_config_t *config);

/****************************** 服务控制 ******************************/

int wifi_wpa_start(const linkg_wifi_config_t *config);
int wifi_wpa_stop(void);
int wifi_wpa_ping(void);

/****************************** 连接控制 ******************************/

int wifi_wpa_disconnect(void);
int wifi_wpa_reconnect(void);
int wifi_wpa_scan(uint32_t frequency_mhz);

#ifdef __cplusplus
}
#endif

#endif
