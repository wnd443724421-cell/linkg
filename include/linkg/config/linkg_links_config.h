/**
 * @file linkg_links_config.h
 * @brief LinkG链路聚合配置定义及处理接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#ifndef LINKG_LINKS_CONFIG_H
#define LINKG_LINKS_CONFIG_H

#include "cJSON.h"
#include "linkg_cellular_config.h"
#include "linkg_device_config.h"
#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef struct
{
    linkg_wifi_config_t     wifi;     // Wi-Fi链路配置
    linkg_cellular_config_t cellular; // 蜂窝链路配置
} linkg_links_config_t;

/****************************** 配置处理 ******************************/

void linkg_links_config_set_default(linkg_links_config_t *out);
int  linkg_links_config_parse(linkg_device_role_t role, const cJSON *node, linkg_links_config_t *out);
int  linkg_links_config_validate(const linkg_links_config_t *config);
int  linkg_links_config_to_json(cJSON *parent, const char *key, const linkg_links_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
