/**
 * @file linkg_paths_config.h
 * @brief LinkG逻辑路径配置定义及处理接口
 */

#ifndef LINKG_PATHS_CONFIG_H
#define LINKG_PATHS_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 路径模式 ******************************/

typedef enum
{
    LINKG_PATH_MODE_NONE   = 0, // 未指定路径模式
    LINKG_PATH_MODE_DIRECT,     // 对端直连
    LINKG_PATH_MODE_RELAY       // 服务器中继
} linkg_path_mode_t;

/****************************** 类型定义 ******************************/

typedef struct
{
    bool              enabled;  // 路径是否参与业务调度
    linkg_path_mode_t mode;     // 路径建立模式
    uint16_t          priority; // 默认调度优先级，数值越小优先级越高
} linkg_path_config_t;

typedef struct
{
    linkg_path_config_t wifi;     // Wi-Fi路径配置
    linkg_path_config_t cellular; // 蜂窝路径配置
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