/**
 * @file config_codec.c
 * @brief LinkG完整配置解析、校验与序列化
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "linkg_config.h"

#include <stdbool.h>
#include <string.h>

#include "linkg_json.h"

#include "config_internal.h"

/****************************** 默认配置 ******************************/

/**
 * @brief 设置全局配置默认值。
 */
void config_set_default(linkg_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    linkg_device_config_set_default(&out->device);
    linkg_network_config_set_default(&out->network);
    linkg_links_config_set_default(&out->links);
    linkg_paths_config_set_default(&out->paths);
    linkg_uart_config_set_default(&out->uart);
}

/****************************** 配置校验 ******************************/

/**
 * @brief 校验全局配置。
 */
int config_validate(const linkg_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_device_config_validate(&config->device);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_network_config_validate(&config->network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_links_config_validate(&config->links);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_paths_config_validate(&config->paths);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_uart_config_validate(&config->uart);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (config->paths.wifi.enabled && !config->links.wifi.enabled)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->paths.cellular.enabled && !config->links.cellular.enabled)
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/****************************** JSON解析 ******************************/

/**
 * @brief 解析全局JSON配置。
 */
int linkg_config_parse_json(const cJSON *root, linkg_config_t *out)
{
    linkg_config_t temp;
    const cJSON   *child;
    bool           found;
    int            ret;

    if (root == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(root))
    {
        return CONFIG_ERR_PARSE;
    }

    config_set_default(&temp);

    child = NULL;

    ret = linkg_json_get_object(root, "device", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_device_config_parse(child, &temp.device);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;

    ret = linkg_json_get_object(root, "network", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_network_config_parse(child, &temp.network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;

    ret = linkg_json_get_object(root, "links", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_links_config_parse(temp.device.role, child, &temp.links);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;

    ret = linkg_json_get_object(root, "paths", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_paths_config_parse(child, &temp.paths);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    child = NULL;
    found = false;

    ret = linkg_json_try_get_array(root, "uart", &child, &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (found)
    {
        ret = linkg_uart_config_parse(child, &temp.uart);
        if (ret != CONFIG_OK)
        {
            return ret;
        }
    }

    ret = config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/****************************** JSON序列化 ******************************/

/**
 * @brief 校验并序列化完整配置，成功后由调用方释放JSON树。
 */
int linkg_config_to_json(const linkg_config_t *config, cJSON **out)
{
    cJSON *root;
    int    ret;

    if (out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    *out = NULL;
    ret = config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_device_config_to_json(root, "device", &config->device);

    if (ret == CONFIG_OK)
    {
        ret = linkg_network_config_to_json(root, "network", &config->network);
    }

    if (ret == CONFIG_OK)
    {
        ret = linkg_links_config_to_json(root, "links", &config->links);
    }

    if (ret == CONFIG_OK)
    {
        ret = linkg_paths_config_to_json(root, "paths", &config->paths);
    }

    if (ret == CONFIG_OK)
    {
        ret = linkg_uart_config_to_json(root, "uart", &config->uart);
    }

    if (ret != CONFIG_OK)
    {
        cJSON_Delete(root);
        return ret;
    }

    *out = root;

    return CONFIG_OK;
}
