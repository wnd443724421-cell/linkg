/**
 * @file linkg_wifi.h
 * @brief LinkG Wi-Fi运行控制接口
 */

#ifndef LINKG_WIFI_H
#define LINKG_WIFI_H

#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_thread.h"
#include "linkg_wifi_config.h"
#include "linkg_wifi_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_wifi_init(linkg_device_role_t role, uint8_t node_id, const linkg_wifi_config_t *config);
int linkg_wifi_start(void);
int linkg_wifi_run(linkg_thread_t *owner_thread);
int linkg_wifi_stop(void);
int linkg_wifi_deinit(void);

/****************************** 状态读取 ******************************/

int linkg_wifi_get_status(linkg_wifi_status_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
