/**
 * @file linkg_wifi_ops.h
 * @brief LinkG Wi-Fi通用校验接口
 */

#ifndef LINKG_WIFI_OPS_H
#define LINKG_WIFI_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 基础校验 ******************************/

bool linkg_wifi_ssid_valid(const char *ssid);
bool linkg_wifi_password_valid(linkg_wifi_security_t security, const char *password);
bool linkg_wifi_channel_valid(linkg_wifi_work_mode_t work_mode, uint16_t channel);
bool linkg_wifi_security_valid(linkg_wifi_security_t security);

/****************************** 窄带校验 ******************************/

bool linkg_wifi_narrow_mode_valid(linkg_wifi_narrow_mode_t mode);
bool linkg_wifi_narrow_bandwidth_valid(uint16_t bandwidth);
bool linkg_wifi_narrow_rate_valid(uint16_t rate);

/****************************** 宽带校验 ******************************/

bool linkg_wifi_wide_bandwidth_valid(linkg_wifi_wide_bandwidth_t bandwidth);

#ifdef __cplusplus
}
#endif

#endif
