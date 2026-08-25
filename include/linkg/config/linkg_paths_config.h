/**
 * @file linkg_paths_config.h
 * @brief LinkG逻辑路径配置定义及处理接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-03
 */

#ifndef LINKG_PATHS_CONFIG_H
#define LINKG_PATHS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "linkg_link.h"
#include "linkg_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef struct
{
    bool              enabled;  // 路径是否参与业务调度
    linkg_link_mode_t mode;     // 路径传输模式
    uint16_t          priority; // 路径优先级，数值越小优先级越高
} linkg_path_config_t;

typedef struct
{
    linkg_path_config_t wifi;     // Wi-Fi接入路径配置
    linkg_path_config_t cellular; // 蜂窝接入路径配置
} linkg_paths_config_t;

/****************************** 配置处理 ******************************/

void linkg_paths_config_set_default(linkg_paths_config_t *out);
int  linkg_paths_config_parse(const cJSON *node, linkg_paths_config_t *out);
int  linkg_paths_config_validate(const linkg_paths_config_t *config);
int  linkg_paths_config_to_json(cJSON *parent, const char *key, const linkg_paths_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
