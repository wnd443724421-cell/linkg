/**
 * @file linkg_device_config.h
 * @brief LinkG设备基础配置定义及处理接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#ifndef LINKG_DEVICE_CONFIG_H
#define LINKG_DEVICE_CONFIG_H

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_DEVICE_ROLE_UNKNOWN = 0, // 未知设备角色
    LINKG_DEVICE_ROLE_AP,          // AP管理节点
    LINKG_DEVICE_ROLE_STA          // STA接入节点
} linkg_device_role_t;

typedef struct
{
    linkg_device_role_t role; // 设备运行角色
} linkg_device_config_t;

/****************************** 配置处理 ******************************/

void linkg_device_config_set_default(linkg_device_config_t *out);
int  linkg_device_config_parse(const cJSON *node, linkg_device_config_t *out);
int  linkg_device_config_validate(const linkg_device_config_t *config);
int  linkg_device_config_to_json(cJSON *parent, const char *key, const linkg_device_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
