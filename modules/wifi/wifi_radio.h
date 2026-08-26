/**
 * @file wifi_radio.h
 * @brief LinkG Wi-Fi无线参数运行维护接口
 */

#ifndef WIFI_RADIO_H
#define WIFI_RADIO_H

#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_wifi_config.h"
#include "wifi_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int  wifi_radio_init(linkg_device_role_t role, const linkg_wifi_config_t *config);
int  wifi_radio_start(const wifi_runtime_t *runtime, uint64_t now_ms);
int  wifi_radio_stop(void);
void wifi_radio_deinit(void);

/****************************** 运行状态同步 ******************************/

int  wifi_radio_sync_runtime(const wifi_runtime_t *runtime, uint64_t now_ms);
void wifi_radio_notify_reapplied(const wifi_runtime_t *runtime, uint64_t now_ms);

/****************************** 定时处理 ******************************/

uint64_t wifi_radio_get_deadline(void);
int      wifi_radio_process(const wifi_runtime_t *runtime, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
