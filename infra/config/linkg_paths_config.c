/**
 * @file linkg_paths_config.c
 * @brief LinkG逻辑路径配置处理实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-08-28
 */

#include "linkg_paths_config.h"

#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"

/****************************** 模块常量 ******************************/

#define LINKG_PATH_WIFI_DEFAULT_PRIORITY     0U // Wi-Fi路径默认优先级
#define LINKG_PATH_CELLULAR_DEFAULT_PRIORITY 1U // 蜂窝路径默认优先级
#define LINKG_PATH_MODE_STRING_MAX           16U // 路径模式字符串缓冲区

/****************************** 模式转换 ******************************/

/**
 * @brief 将路径模式文本转换为枚举值。
 */
static int _paths_mode_from_string(const char *text, linkg_path_mode_t *mode)
{
    if (text == NULL || mode == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(text, "direct") == 0)
    {
        *mode = LINKG_PATH_MODE_DIRECT;
        return CONFIG_OK;
    }

    if (strcmp(text, "relay") == 0)
    {
        *mode = LINKG_PATH_MODE_RELAY;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将路径模式枚举转换为文本。
 */
static const char *_paths_mode_to_string(linkg_path_mode_t mode)
{
    switch (mode)
    {
        case LINKG_PATH_MODE_DIRECT:
            return "direct";

        case LINKG_PATH_MODE_RELAY:
            return "relay";

        default:
            return NULL;
    }
}

/****************************** 子配置处理 ******************************/

/**
 * @brief 解析单条逻辑路径配置。
 */
static int _paths_path_parse(const cJSON *node, linkg_path_config_t *out)
{
    char mode_text[LINKG_PATH_MODE_STRING_MAX];
    int  ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_bool(node, "enabled", &out->enabled);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_json_get_string(node, "mode", mode_text, sizeof(mode_text));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _paths_mode_from_string(mode_text, &out->mode);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_get_uint16(node, "priority", &out->priority);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    return CONFIG_OK;
}

/**
 * @brief 校验单条逻辑路径配置。
 */
static int _paths_path_validate(const linkg_path_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->mode != LINKG_PATH_MODE_DIRECT &&
        config->mode != LINKG_PATH_MODE_RELAY)
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 判断两条启用路径是否存在优先级冲突。
 */
static bool _paths_priority_conflicts(const linkg_path_config_t *first, const linkg_path_config_t *second)
{
    if (first == NULL || second == NULL)
    {
        return false;
    }

    if (!first->enabled || !second->enabled)
    {
        return false;
    }

    return first->priority == second->priority;
}

/**
 * @brief 将单条逻辑路径配置写入JSON对象。
 */
static int _paths_path_to_json(cJSON *parent, const char *key, const linkg_path_config_t *config)
{
    const char *mode_name;
    cJSON      *node;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _paths_path_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    mode_name = _paths_mode_to_string(config->mode);
    if (mode_name == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    node = cJSON_CreateObject();
    if (node == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(node, "enabled", config->enabled);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(node, "mode", mode_name);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(node, "priority", config->priority);
    }

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(node);
        return config_json_write_error(ret);
    }

    ret = linkg_json_add_object(parent, key, node);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(node);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/****************************** 配置处理 ******************************/

/**
 * @brief 设置逻辑路径默认配置。
 */
void linkg_paths_config_set_default(linkg_paths_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->wifi.enabled  = true;
    out->wifi.mode     = LINKG_PATH_MODE_DIRECT;
    out->wifi.priority = LINKG_PATH_WIFI_DEFAULT_PRIORITY;

    out->cellular.enabled  = false;
    out->cellular.mode     = LINKG_PATH_MODE_DIRECT;
    out->cellular.priority = LINKG_PATH_CELLULAR_DEFAULT_PRIORITY;
}

/**
 * @brief 解析逻辑路径配置。
 */
int linkg_paths_config_parse(const cJSON *node, linkg_paths_config_t *out)
{
    linkg_paths_config_t temp;
    const cJSON         *path_node;
    int                  ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    linkg_paths_config_set_default(&temp);

    ret = linkg_json_get_object(node, "wifi", &path_node);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _paths_path_parse(path_node, &temp.wifi);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_get_object(node, "cellular", &path_node);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _paths_path_parse(path_node, &temp.cellular);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_paths_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 校验逻辑路径配置。
 */
int linkg_paths_config_validate(const linkg_paths_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _paths_path_validate(&config->wifi);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _paths_path_validate(&config->cellular);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (!config->wifi.enabled && !config->cellular.enabled)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (_paths_priority_conflicts(&config->wifi, &config->cellular))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 将逻辑路径配置转换为JSON对象。
 */
int linkg_paths_config_to_json(cJSON *parent, const char *key, const linkg_paths_config_t *config)
{
    cJSON *node;
    int    ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_paths_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    node = cJSON_CreateObject();
    if (node == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = _paths_path_to_json(node, "wifi", &config->wifi);
    if (ret == CONFIG_OK)
    {
        ret = _paths_path_to_json(node, "cellular", &config->cellular);
    }

    if (ret != CONFIG_OK)
    {
        cJSON_Delete(node);
        return ret;
    }

    ret = linkg_json_add_object(parent, key, node);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(node);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}
