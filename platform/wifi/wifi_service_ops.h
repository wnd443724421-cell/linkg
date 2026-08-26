/**
 * @file wifi_service_ops.h
 * @brief LinkG Wi-Fi系统服务统一操作接口
 */

#ifndef WIFI_SERVICE_OPS_H
#define WIFI_SERVICE_OPS_H

#include "linkg_device_config.h"
#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置同步 ******************************/

int wifi_service_ops_update_config(linkg_device_role_t role, const linkg_wifi_config_t *config);

/****************************** 服务控制 ******************************/

int wifi_service_ops_start(linkg_device_role_t role, const linkg_wifi_config_t *config);
int wifi_service_ops_stop(linkg_device_role_t role);
int wifi_service_ops_restart(linkg_device_role_t role, const linkg_wifi_config_t *config);

/****************************** STA连接控制 ******************************/

int wifi_service_ops_disconnect_sta(void);
int wifi_service_ops_reconnect_sta(void);

#ifdef __cplusplus
}
#endif

#endif
