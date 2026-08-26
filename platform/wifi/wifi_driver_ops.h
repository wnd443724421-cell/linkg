/**
 * @file wifi_driver_ops.h
 * @brief LinkG HI1105 Wi-Fi驱动运行控制接口
 */

#ifndef WIFI_DRIVER_OPS_H
#define WIFI_DRIVER_OPS_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"

#include "wal_radio_status_ioctl.h"
#include "wal_tx_flowctrl_ioctl.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 日志等级 ******************************/

typedef enum
{
    WIFI_DRIVER_LOG_LEVEL_ERROR = 1, // 仅错误
    WIFI_DRIVER_LOG_LEVEL_WARN,      // 错误和警告
    WIFI_DRIVER_LOG_LEVEL_INFO       // 错误、警告和信息
} wifi_driver_log_level_t;

/****************************** 生命周期 ******************************/

int  wifi_driver_ops_init(void);
void wifi_driver_ops_deinit(void);

/****************************** 状态查询 ******************************/

int wifi_driver_get_radio_status(wal_radio_status_stru *status);
int wifi_driver_get_tx_flowctrl_status(wal_tx_flowctrl_status_stru *status);

/****************************** 接收模式 ******************************/

int wifi_driver_set_napi_state(bool enable);
int wifi_driver_set_low_latency(bool enable);

/****************************** 电源管理 ******************************/

int wifi_driver_set_power_management(bool enable);
int wifi_driver_set_sta_power_save(bool enable);

/****************************** 驱动控制 ******************************/

int wifi_driver_reload_ini(linkg_device_role_t role);

/****************************** 窄带控制 ******************************/

int wifi_driver_set_narrow_bandwidth(bool enable, uint16_t bandwidth_mhz);
int wifi_driver_set_narrow_auto_rate(bool enable);
int wifi_driver_set_narrow_rate_level(uint8_t rate_level);
int wifi_driver_refresh_narrow_power_table(void);

/****************************** 日志控制 ******************************/

int wifi_driver_set_log_level(wifi_driver_log_level_t level);

#ifdef __cplusplus
}
#endif

#endif
