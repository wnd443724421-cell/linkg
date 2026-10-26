/**
 * @file linkg_cellular_config.c
 * @brief LinkG 蜂窝配置处理实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-24
 */

#include "linkg_cellular_config.h"

#include <stddef.h>
#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"

/****************************** 模块常量 ******************************/

#define CELLULAR_ENUM_STRING_MAX 16U // 枚举字符串缓冲区

/****************************** 网络模式转换 ******************************/

/**
 * @brief 将蜂窝网络模式转换为字符串。
 */
static const char *_cellular_network_mode_to_string(linkg_cellular_network_mode_t mode)
{
    switch (mode)
    {
        case LINKG_CELLULAR_NETWORK_MODE_AUTO:
            return "auto";

        case LINKG_CELLULAR_NETWORK_MODE_4G:
            return "4g";

        case LINKG_CELLULAR_NETWORK_MODE_5G:
            return "5g";

        default:
            return NULL;
    }
}

/**
 * @brief 将字符串转换为蜂窝网络模式。
 */
static int _cellular_network_mode_from_string(const char *string, linkg_cellular_network_mode_t *out)
{
    if (string == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(string, "auto") == 0)
    {
        *out = LINKG_CELLULAR_NETWORK_MODE_AUTO;
        return CONFIG_OK;
    }

    if (strcmp(string, "4g") == 0)
    {
        *out = LINKG_CELLULAR_NETWORK_MODE_4G;
        return CONFIG_OK;
    }

    if (strcmp(string, "5g") == 0)
    {
        *out = LINKG_CELLULAR_NETWORK_MODE_5G;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/****************************** 字段校验 ******************************/

/**
 * @brief 判断字符是否允许出现在APN中。
 */
static bool _cellular_apn_char_valid(unsigned char value)
{
    if (value >= 'a' && value <= 'z')
    {
        return true;
    }

    if (value >= 'A' && value <= 'Z')
    {
        return true;
    }

    if (value >= '0' && value <= '9')
    {
        return true;
    }

    return value == '-' || value == '.';
}

/**
 * @brief 校验APN。
 */
static bool _cellular_apn_valid(const char *apn)
{
    const unsigned char *cursor;
    size_t               length;

    if (apn == NULL)
    {
        return false;
    }

    length = strlen(apn);
    if (length > LINKG_CELLULAR_APN_MAX)
    {
        return false;
    }

    if (length == 0U)
    {
        return true;
    }

    cursor = (const unsigned char *)apn;
    while (*cursor != '\0')
    {
        if (!_cellular_apn_char_valid(*cursor))
        {
            return false;
        }

        cursor++;
    }

    return true;
}

/**
 * @brief 校验SIM PIN。
 */
static bool _cellular_pin_valid(const char *pin)
{
    size_t index;
    size_t length;

    if (pin == NULL)
    {
        return false;
    }

    length = strlen(pin);
    if (length == 0U)
    {
        return true;
    }

    if (length < LINKG_CELLULAR_PIN_MIN || length > LINKG_CELLULAR_PIN_MAX)
    {
        return false;
    }

    for (index = 0U; index < length; index++)
    {
        if (pin[index] < '0' || pin[index] > '9')
        {
            return false;
        }
    }

    return true;
}

/****************************** 配置处理 ******************************/

/**
 * @brief 设置蜂窝配置默认值。
 */
void linkg_cellular_config_set_default(linkg_cellular_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->enabled = false;
    out->network_mode = LINKG_CELLULAR_NETWORK_MODE_AUTO;
}

/**
 * @brief 校验蜂窝配置。
 */
int linkg_cellular_config_validate(const linkg_cellular_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->network_mode != LINKG_CELLULAR_NETWORK_MODE_AUTO &&
        config->network_mode != LINKG_CELLULAR_NETWORK_MODE_4G &&
        config->network_mode != LINKG_CELLULAR_NETWORK_MODE_5G)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!_cellular_apn_valid(config->apn))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!_cellular_pin_valid(config->pin))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 解析蜂窝配置。
 */
int linkg_cellular_config_parse(const cJSON *node, linkg_cellular_config_t *out)
{
    linkg_cellular_config_t temp;
    char                    mode[CELLULAR_ENUM_STRING_MAX];
    bool                    found;
    int                     ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    linkg_cellular_config_set_default(&temp);

    ret = linkg_json_get_bool(node, "enabled", &temp.enabled);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    found = false;
    ret = linkg_json_try_get_string(node, "network_mode", mode, sizeof(mode), &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (found)
    {
        ret = _cellular_network_mode_from_string(mode, &temp.network_mode);
        if (ret != CONFIG_OK)
        {
            return ret;
        }
    }

    found = false;
    ret = linkg_json_try_get_string(node, "apn", temp.apn, sizeof(temp.apn), &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    found = false;
    ret = linkg_json_try_get_string(node, "pin", temp.pin, sizeof(temp.pin), &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_cellular_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 将蜂窝配置转换为JSON对象。
 */
int linkg_cellular_config_to_json(cJSON *parent, const char *key, const linkg_cellular_config_t *config)
{
    const char *network_mode;
    cJSON      *object;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_cellular_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    network_mode = _cellular_network_mode_to_string(config->network_mode);
    if (network_mode == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "enabled", config->enabled);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "network_mode", network_mode);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "apn", config->apn);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "pin", config->pin);
    }

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
