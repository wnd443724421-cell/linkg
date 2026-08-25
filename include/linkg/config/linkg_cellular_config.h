/**
 * @file linkg_cellular_config.h
 * @brief LinkG蜂窝链路配置定义及处理接口
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-24
 */

#ifndef LINKG_CELLULAR_CONFIG_H
#define LINKG_CELLULAR_CONFIG_H

#include <stdbool.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置常量 ******************************/

#define LINKG_CELLULAR_APN_MAX 100U // APN最大长度
#define LINKG_CELLULAR_PIN_MIN   4U // SIM PIN最小长度
#define LINKG_CELLULAR_PIN_MAX   8U // SIM PIN最大长度

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_CELLULAR_NETWORK_MODE_UNKNOWN = 0, // 未知网络模式
    LINKG_CELLULAR_NETWORK_MODE_AUTO,        // 4G/5G自动选择
    LINKG_CELLULAR_NETWORK_MODE_4G,          // 仅4G LTE
    LINKG_CELLULAR_NETWORK_MODE_5G           // 仅5G NR SA
} linkg_cellular_network_mode_t;

typedef struct
{
    bool                          enabled;                          // 是否初始化蜂窝接入模块
    linkg_cellular_network_mode_t network_mode;                     // 蜂窝网络工作模式
    char                          apn[LINKG_CELLULAR_APN_MAX + 1U]; // APN，空字符串表示自动选择
    char                          pin[LINKG_CELLULAR_PIN_MAX + 1U]; // SIM PIN，空字符串表示未配置
} linkg_cellular_config_t;

/****************************** 配置处理 ******************************/

void linkg_cellular_config_set_default(linkg_cellular_config_t *out);
int  linkg_cellular_config_parse(const cJSON *node, linkg_cellular_config_t *out);
int  linkg_cellular_config_validate(const linkg_cellular_config_t *config);
int  linkg_cellular_config_to_json(cJSON *parent, const char *key, const linkg_cellular_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
