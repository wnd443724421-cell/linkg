/**
 * @file linkg_device_config.c
 * @brief LinkG 设备基础配置处理接口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#include "linkg_device_config.h"

#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"

/****************************** 模块常量 ******************************/

#define DEVICE_ENUM_STRING_MAX 16U // 枚举字符串缓冲区

/****************************** 角色转换 ******************************/

/**
 * @brief 将设备角色转换为字符串。
 */
static const char *_device_role_to_string(linkg_device_role_t role)
{
    switch (role)
    {
        case LINKG_DEVICE_ROLE_AP:
            return "ap";

        case LINKG_DEVICE_ROLE_STA:
            return "sta";

        default:
            return NULL;
    }
}

/**
 * @brief 将字符串转换为设备角色。
 */
static int _device_role_from_string(const char *string, linkg_device_role_t *out)
{
    if (string == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(string, "ap") == 0)
    {
        *out = LINKG_DEVICE_ROLE_AP;
        return CONFIG_OK;
    }

    if (strcmp(string, "sta") == 0)
    {
        *out = LINKG_DEVICE_ROLE_STA;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/****************************** 配置处理 ******************************/

/**
 * @brief 设置设备配置默认值。
 */
void linkg_device_config_set_default(linkg_device_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->role = LINKG_DEVICE_ROLE_STA;
}

/**
 * @brief 校验设备配置。
 */
int linkg_device_config_validate(const linkg_device_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->role != LINKG_DEVICE_ROLE_AP &&
        config->role != LINKG_DEVICE_ROLE_STA)
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 解析设备配置。
 */
int linkg_device_config_parse(const cJSON *node, linkg_device_config_t *out)
{
    linkg_device_config_t temp;
    char                  role[DEVICE_ENUM_STRING_MAX];
    int                   ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    linkg_device_config_set_default(&temp);

    ret = linkg_json_get_string(node, "role", role, sizeof(role));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _device_role_from_string(role, &temp.role);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_device_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 将设备配置转换为JSON对象。
 */
int linkg_device_config_to_json(cJSON *parent, const char *key, const linkg_device_config_t *config)
{
    const char *role;
    cJSON      *object;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_device_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    role = _device_role_to_string(config->role);
    if (role == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}
