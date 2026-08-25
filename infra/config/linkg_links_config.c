/**
 * @file linkg_links_config.c
 * @brief LinkG 链路聚合配置处理接口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#include "linkg_links_config.h"

#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"

/****************************** 配置处理 ******************************/

/**
 * @brief 设置链路配置默认值。
 */
void linkg_links_config_set_default(linkg_links_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    linkg_wifi_config_set_default(&out->wifi);
    linkg_cellular_config_set_default(&out->cellular);
}

/**
 * @brief 校验链路配置。
 */
int linkg_links_config_validate(const linkg_links_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_wifi_config_validate(&config->wifi);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    return linkg_cellular_config_validate(&config->cellular);
}

/**
 * @brief 解析链路配置。
 */
int linkg_links_config_parse(linkg_device_role_t role, const cJSON *node, linkg_links_config_t *out)
{
    linkg_links_config_t temp;
    const cJSON         *child;
    int                  ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return CONFIG_ERR_VALIDATE;
    }

    linkg_links_config_set_default(&temp);

    child = NULL;
    ret = linkg_json_get_object(node, "wifi", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_wifi_config_parse(role, child, &temp.wifi);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;
    ret = linkg_json_get_object(node, "cellular", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_cellular_config_parse(child, &temp.cellular);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_links_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 将链路配置转换为JSON对象。
 */
int linkg_links_config_to_json(cJSON *parent, const char *key, const linkg_links_config_t *config)
{
    cJSON *object;
    int    ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_links_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_wifi_config_to_json(object, "wifi", &config->wifi);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_cellular_config_to_json(object, "cellular", &config->cellular);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}
