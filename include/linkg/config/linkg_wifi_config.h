/**
 * @file linkg_wifi_config.h
 * @brief LinkG Wi-Fi配置定义及处理接口
 * @author Dawn
 * @version 1.0.1
 * @date 2026-07-22
 */

#ifndef LINKG_WIFI_CONFIG_H
#define LINKG_WIFI_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "linkg_device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置常量 ******************************/

#define LINKG_WIFI_SSID_MAX             32U // Wi-Fi名称最大长度
#define LINKG_WIFI_PASSWORD_MAX         63U // Wi-Fi密码最大长度
#define LINKG_WIFI_WPA2_PASSWORD_MIN     8U // WPA2密码最小长度
#define LINKG_WIFI_NARROW_BANDWIDTH_MHZ 10U // 窄带固定带宽
#define LINKG_WIFI_NARROW_RATE_MIN       0U // 窄带最低速率档位
#define LINKG_WIFI_NARROW_RATE_MAX       5U // 窄带最高速率档位

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_WIFI_SECURITY_UNKNOWN = 0, // 未知安全模式
    LINKG_WIFI_SECURITY_OPEN,        // 开放网络
    LINKG_WIFI_SECURITY_WPA2_PSK     // WPA2-Personal
} linkg_wifi_security_t;

typedef enum
{
    LINKG_WIFI_WORK_MODE_UNKNOWN = 0, // 未知工作模式
    LINKG_WIFI_WORK_MODE_NARROW,      // 窄带模式
    LINKG_WIFI_WORK_MODE_WIDE         // 宽带模式
} linkg_wifi_work_mode_t;

typedef enum
{
    LINKG_WIFI_NARROW_MODE_UNKNOWN = 0, // 未知速率控制模式
    LINKG_WIFI_NARROW_MODE_FIXED,       // 固定速率模式
    LINKG_WIFI_NARROW_MODE_ADAPTIVE     // 驱动自适应速率模式
} linkg_wifi_narrow_mode_t;

typedef enum
{
    LINKG_WIFI_WIDE_BANDWIDTH_UNKNOWN = 0,  // 未知宽带带宽
    LINKG_WIFI_WIDE_BANDWIDTH_20_MHZ  = 20, // 20MHz
    LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ  = 40, // 40MHz
    LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ  = 80  // 80MHz
} linkg_wifi_wide_bandwidth_t;

typedef struct
{
    char                  ssid[LINKG_WIFI_SSID_MAX + 1U];         // AP热点名称
    char                  password[LINKG_WIFI_PASSWORD_MAX + 1U]; // AP认证密码
    uint16_t              channel;                                // AP工作信道
    linkg_wifi_security_t security;                               // AP安全模式
} linkg_wifi_ap_config_t;

typedef struct
{
    char                  ssid[LINKG_WIFI_SSID_MAX + 1U];         // 目标AP名称
    char                  password[LINKG_WIFI_PASSWORD_MAX + 1U]; // AP认证密码
    linkg_wifi_security_t security;                               // STA安全模式
} linkg_wifi_sta_config_t;

typedef struct
{
    linkg_wifi_narrow_mode_t mode;        // 窄带速率控制模式
    uint16_t                 bandwidth;   // 窄带带宽，固定为10MHz
    uint16_t                 manual_rate; // 固定速率档位，范围0～5
} linkg_wifi_narrow_params_t;

typedef struct
{
    linkg_wifi_wide_bandwidth_t ap_bandwidth; // AP宽带带宽
} linkg_wifi_wide_params_t;

typedef struct
{
    linkg_wifi_work_mode_t     work_mode;     // 宽窄带工作模式
    linkg_wifi_narrow_params_t narrow_params; // 窄带参数
    linkg_wifi_wide_params_t   wide_params;   // 宽带参数
} linkg_wifi_wideband_config_t;

typedef struct
{
    bool                         enabled;  // 是否初始化Wi-Fi接入模块
    linkg_wifi_ap_config_t       ap;       // AP配置
    linkg_wifi_sta_config_t      sta;      // STA配置
    linkg_wifi_wideband_config_t wideband; // 宽窄带配置
} linkg_wifi_config_t;

/****************************** 配置处理 ******************************/

void linkg_wifi_config_set_default(linkg_wifi_config_t *out);
int  linkg_wifi_config_parse(linkg_device_role_t role, const cJSON *node, linkg_wifi_config_t *out);
int  linkg_wifi_config_validate(const linkg_wifi_config_t *config);
int  linkg_wifi_config_to_json(cJSON *parent, const char *key, const linkg_wifi_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
