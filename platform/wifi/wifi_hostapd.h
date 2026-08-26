/**
 * @file wifi_hostapd.h
 * @brief LinkG hostapd服务内部接口
 */

#ifndef WIFI_HOSTAPD_H
#define WIFI_HOSTAPD_H

#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置同步 ******************************/

int wifi_hostapd_update_config(const linkg_wifi_config_t *config);

/****************************** 服务控制 ******************************/

int wifi_hostapd_start(const linkg_wifi_config_t *config);
int wifi_hostapd_stop(void);

#ifdef __cplusplus
}
#endif

#endif
