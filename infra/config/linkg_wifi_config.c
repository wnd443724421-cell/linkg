/**
 * @file linkg_wifi_config.c
 * @brief LinkG Wi-Fi 配置处理接口实现
 * @author Dawn
 * @version 1.0.1
 * @date 2026-07-22
 */

#include "linkg_wifi_config.h"

#include <stddef.h>
#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"
#include "linkg_wifi_ops.h"

/****************************** 模块常量 ******************************/

#define WIFI_DEFAULT_SSID                "LinkG" // 默认Wi-Fi名称
#define WIFI_DEFAULT_AP_CHANNEL          149U // 默认AP信道
#define WIFI_DEFAULT_NARROW_MODE         LINKG_WIFI_NARROW_MODE_FIXED // 默认窄带速率模式
#define WIFI_DEFAULT_MANUAL_RATE         1U // 默认固定速率档位
#define WIFI_DEFAULT_WIDE_BANDWIDTH      LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ // 默认宽带带宽
#define WIFI_ENUM_STRING_MAX             16U // 枚举字符串缓冲区

/****************************** 基础辅助函数 ******************************/

/**
 * @brief 设置Wi-Fi字符串。
 */
static void _wifi_string_set(char *out, size_t out_size, const char *value)
{
    size_t length;

    if (out == NULL || out_size == 0U || value == NULL)
    {
        return;
    }

    length = strlen(value);
    if (length >= out_size)
    {
        length = out_size - 1U;
    }

    memcpy(out, value, length);
    out[length] = '\0';
}

/**
 * @brief 将Wi-Fi安全模式转换为字符串。
 */
static const char *_wifi_security_to_string(linkg_wifi_security_t security)
{
    switch (security)
    {
        case LINKG_WIFI_SECURITY_OPEN:
            return "open";

        case LINKG_WIFI_SECURITY_WPA2_PSK:
            return "wpa2-psk";

        default:
            return NULL;
    }
}

/**
 * @brief 将字符串转换为Wi-Fi安全模式。
 */
static int _wifi_security_from_string(const char *string, linkg_wifi_security_t *out)
{
    if (string == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(string, "open") == 0)
    {
        *out = LINKG_WIFI_SECURITY_OPEN;
        return CONFIG_OK;
    }

    if (strcmp(string, "wpa2-psk") == 0)
    {
        *out = LINKG_WIFI_SECURITY_WPA2_PSK;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将Wi-Fi工作模式转换为字符串。
 */
static const char *_wifi_work_mode_to_string(linkg_wifi_work_mode_t mode)
{
    switch (mode)
    {
        case LINKG_WIFI_WORK_MODE_NARROW:
            return "narrow";

        case LINKG_WIFI_WORK_MODE_WIDE:
            return "wide";

        default:
            return NULL;
    }
}

/**
 * @brief 将字符串转换为Wi-Fi工作模式。
 */
static int _wifi_work_mode_from_string(const char *string, linkg_wifi_work_mode_t *out)
{
    if (string == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(string, "narrow") == 0)
    {
        *out = LINKG_WIFI_WORK_MODE_NARROW;
        return CONFIG_OK;
    }

    if (strcmp(string, "wide") == 0)
    {
        *out = LINKG_WIFI_WORK_MODE_WIDE;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将窄带速率模式转换为字符串。
 */
static const char *_wifi_narrow_mode_to_string(linkg_wifi_narrow_mode_t mode)
{
    switch (mode)
    {
        case LINKG_WIFI_NARROW_MODE_FIXED:
            return "fixed";

        case LINKG_WIFI_NARROW_MODE_ADAPTIVE:
            return "adaptive";

        default:
            return NULL;
    }
}

/**
 * @brief 将字符串转换为窄带速率模式。
 */
static int _wifi_narrow_mode_from_string(const char *string, linkg_wifi_narrow_mode_t *out)
{
    if (string == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(string, "fixed") == 0)
    {
        *out = LINKG_WIFI_NARROW_MODE_FIXED;
        return CONFIG_OK;
    }

    if (strcmp(string, "adaptive") == 0)
    {
        *out = LINKG_WIFI_NARROW_MODE_ADAPTIVE;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/****************************** 默认配置 ******************************/

/**
 * @brief 设置Wi-FiAP配置默认值。
 */
static void _wifi_ap_config_set_default(linkg_wifi_ap_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    _wifi_string_set(out->ssid, sizeof(out->ssid), WIFI_DEFAULT_SSID);

    out->channel  = WIFI_DEFAULT_AP_CHANNEL;
    out->security = LINKG_WIFI_SECURITY_OPEN;
}

/**
 * @brief 设置Wi-FiSTA配置默认值。
 */
static void _wifi_sta_config_set_default(linkg_wifi_sta_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    _wifi_string_set(out->ssid, sizeof(out->ssid), WIFI_DEFAULT_SSID);

    out->security = LINKG_WIFI_SECURITY_OPEN;
}

/**
 * @brief 设置Wi-Fi窄带参数默认值。
 */
static void _wifi_narrow_params_set_default(linkg_wifi_narrow_params_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->mode        = WIFI_DEFAULT_NARROW_MODE;
    out->bandwidth   = LINKG_WIFI_NARROW_BANDWIDTH_MHZ;
    out->manual_rate = WIFI_DEFAULT_MANUAL_RATE;
}

/**
 * @brief 设置Wi-Fi宽带参数默认值。
 */
static void _wifi_wide_params_set_default(linkg_wifi_wide_params_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->ap_bandwidth = WIFI_DEFAULT_WIDE_BANDWIDTH;
}

/**
 * @brief 设置Wi-Fi宽带模式配置默认值。
 */
static void _wifi_wideband_config_set_default(linkg_wifi_wideband_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->work_mode = LINKG_WIFI_WORK_MODE_NARROW;

    _wifi_narrow_params_set_default(&out->narrow_params);
    _wifi_wide_params_set_default(&out->wide_params);
}

/****************************** JSON字段解析 ******************************/

/**
 * @brief 获取安全模式。
 */
static int _wifi_get_security(const cJSON *node, const char *key, linkg_wifi_security_t *out)
{
    char value[WIFI_ENUM_STRING_MAX];
    int  ret;

    if (node == NULL || key == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_json_get_string(node, key, value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    return _wifi_security_from_string(value, out);
}

/**
 * @brief 读取Wi-Fi工作模式。
 */
static int _wifi_get_work_mode(const cJSON *node, const char *key, linkg_wifi_work_mode_t *out)
{
    char value[WIFI_ENUM_STRING_MAX];
    int  ret;

    if (node == NULL || key == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_json_get_string(node, key, value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    return _wifi_work_mode_from_string(value, out);
}

/**
 * @brief 获取窄带模式。
 */
static int _wifi_get_narrow_mode(const cJSON *node, const char *key, linkg_wifi_narrow_mode_t *out)
{
    char value[WIFI_ENUM_STRING_MAX];
    int  ret;

    if (node == NULL || key == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_json_get_string(node, key, value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    return _wifi_narrow_mode_from_string(value, out);
}

/**
 * @brief 获取宽带带宽。
 */
static int _wifi_get_wide_bandwidth(const cJSON *node, const char *key, linkg_wifi_wide_bandwidth_t *out)
{
    uint16_t value;
    int      ret;

    if (node == NULL || key == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_json_get_uint16(node, key, &value);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (!linkg_wifi_wide_bandwidth_valid((linkg_wifi_wide_bandwidth_t)value))
    {
        return CONFIG_ERR_VALIDATE;
    }

    *out = (linkg_wifi_wide_bandwidth_t)value;

    return CONFIG_OK;
}

/****************************** 子配置解析 ******************************/

/**
 * @brief 解析Wi-FiAP配置。
 */
static int _wifi_ap_config_parse(const cJSON *node, linkg_wifi_ap_config_t *out)
{
    linkg_wifi_ap_config_t temp = {0};
    int                    ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_string(node, "ssid", temp.ssid, sizeof(temp.ssid));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_json_get_string(node, "password", temp.password, sizeof(temp.password));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_json_get_uint16(node, "channel", &temp.channel);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _wifi_get_security(node, "security", &temp.security);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 解析Wi-FiSTA配置。
 */
static int _wifi_sta_config_parse(const cJSON *node, linkg_wifi_sta_config_t *out)
{
    linkg_wifi_sta_config_t temp = {0};
    int                     ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_string(node, "ssid", temp.ssid, sizeof(temp.ssid));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_json_get_string(node, "password", temp.password, sizeof(temp.password));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _wifi_get_security(node, "security", &temp.security);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 解析Wi-Fi窄带参数。
 */
static int _wifi_narrow_params_parse(const cJSON *node, linkg_wifi_narrow_params_t *out)
{
    linkg_wifi_narrow_params_t temp;
    bool                       found;
    int                        ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    // 自适应模式字段缺失时保留默认固定参数，便于后续切回固定模式。
    temp = *out;

    ret = _wifi_get_narrow_mode(node, "mode", &temp.mode);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    switch (temp.mode)
    {
        case LINKG_WIFI_NARROW_MODE_FIXED:
            ret = linkg_json_get_uint16(node, "bandwidth", &temp.bandwidth);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            ret = linkg_json_get_uint16(node, "manual_rate", &temp.manual_rate);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }
            break;

        case LINKG_WIFI_NARROW_MODE_ADAPTIVE:
            found = false;
            ret = linkg_json_try_get_uint16(node, "bandwidth", &temp.bandwidth, &found);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            found = false;
            ret = linkg_json_try_get_uint16(node, "manual_rate", &temp.manual_rate, &found);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }
            break;

        default:
            return CONFIG_ERR_VALIDATE;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 解析Wi-Fi宽带参数。
 */
static int _wifi_wide_params_parse(const cJSON *node, linkg_wifi_wide_params_t *out)
{
    linkg_wifi_wide_params_t temp = {0};
    int                      ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = _wifi_get_wide_bandwidth(node, "ap_bandwidth", &temp.ap_bandwidth);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 解析Wi-Fi宽带模式配置。
 */
static int _wifi_wideband_config_parse(const cJSON *node, linkg_wifi_wideband_config_t *out)
{
    linkg_wifi_wideband_config_t temp;
    const cJSON                 *child;
    bool                         found;
    int                          ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    temp = *out;

    ret = _wifi_get_work_mode(node, "work_mode", &temp.work_mode);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    switch (temp.work_mode)
    {
        case LINKG_WIFI_WORK_MODE_NARROW:
            child = NULL;
            ret = linkg_json_get_object(node, "narrow_params", &child);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            ret = _wifi_narrow_params_parse(child, &temp.narrow_params);
            if (ret != CONFIG_OK)
            {
                return ret;
            }

            child = NULL;
            found = false;
            ret = linkg_json_try_get_object(node, "wide_params", &child, &found);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            if (found)
            {
                ret = _wifi_wide_params_parse(child, &temp.wide_params);
                if (ret != CONFIG_OK)
                {
                    return ret;
                }
            }
            break;

        case LINKG_WIFI_WORK_MODE_WIDE:
            child = NULL;
            ret = linkg_json_get_object(node, "wide_params", &child);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            ret = _wifi_wide_params_parse(child, &temp.wide_params);
            if (ret != CONFIG_OK)
            {
                return ret;
            }

            child = NULL;
            found = false;
            ret = linkg_json_try_get_object(node, "narrow_params", &child, &found);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            if (found)
            {
                ret = _wifi_narrow_params_parse(child, &temp.narrow_params);
                if (ret != CONFIG_OK)
                {
                    return ret;
                }
            }
            break;

        default:
            return CONFIG_ERR_VALIDATE;
    }

    *out = temp;

    return CONFIG_OK;
}

/****************************** 配置校验 ******************************/

/**
 * @brief 校验Wi-FiAP配置。
 */
static int _wifi_ap_config_validate(const linkg_wifi_ap_config_t *config, linkg_wifi_work_mode_t work_mode)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!linkg_wifi_ssid_valid(config->ssid))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_wifi_security_valid(config->security))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_wifi_password_valid(config->security, config->password))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_wifi_channel_valid(work_mode, config->channel))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验Wi-FiSTA配置。
 */
static int _wifi_sta_config_validate(const linkg_wifi_sta_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!linkg_wifi_ssid_valid(config->ssid))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_wifi_security_valid(config->security))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_wifi_password_valid(config->security, config->password))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验Wi-Fi窄带参数。
 */
static int _wifi_narrow_params_validate(const linkg_wifi_narrow_params_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!linkg_wifi_narrow_mode_valid(config->mode))
    {
        return CONFIG_ERR_VALIDATE;
    }

    // 自适应模式仍保留并校验固定模式参数，避免切换后使用非法旧值。
    if (!linkg_wifi_narrow_bandwidth_valid(config->bandwidth))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_wifi_narrow_rate_valid(config->manual_rate))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验Wi-Fi宽带参数。
 */
static int _wifi_wide_params_validate(const linkg_wifi_wide_params_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!linkg_wifi_wide_bandwidth_valid(config->ap_bandwidth))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验Wi-Fi宽带模式配置。
 */
static int _wifi_wideband_config_validate(const linkg_wifi_wideband_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->work_mode != LINKG_WIFI_WORK_MODE_NARROW &&
        config->work_mode != LINKG_WIFI_WORK_MODE_WIDE)
    {
        return CONFIG_ERR_VALIDATE;
    }

    ret = _wifi_narrow_params_validate(&config->narrow_params);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    return _wifi_wide_params_validate(&config->wide_params);
}

/****************************** JSON序列化 ******************************/

/**
 * @brief 将Wi-FiAP配置转换为JSON对象。
 */
static int _wifi_ap_config_to_json(cJSON *parent, const char *key, const linkg_wifi_ap_config_t *config, linkg_wifi_work_mode_t work_mode)
{
    const char *security;
    cJSON      *object;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _wifi_ap_config_validate(config, work_mode);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    security = _wifi_security_to_string(config->security);
    if (security == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "ssid", config->ssid);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "password", config->password);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(object, "channel", config->channel);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "security", security);
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

/**
 * @brief 将Wi-FiSTA配置转换为JSON对象。
 */
static int _wifi_sta_config_to_json(cJSON *parent, const char *key, const linkg_wifi_sta_config_t *config)
{
    const char *security;
    cJSON      *object;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _wifi_sta_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    security = _wifi_security_to_string(config->security);
    if (security == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "ssid", config->ssid);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "password", config->password);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "security", security);
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

/**
 * @brief 将Wi-Fi窄带参数转换为JSON对象。
 */
static int _wifi_narrow_params_to_json(cJSON *parent, const char *key, const linkg_wifi_narrow_params_t *config)
{
    const char *mode;
    cJSON      *object;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _wifi_narrow_params_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    mode = _wifi_narrow_mode_to_string(config->mode);
    if (mode == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "mode", mode);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(object, "bandwidth", config->bandwidth);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(object, "manual_rate", config->manual_rate);
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

/**
 * @brief 将Wi-Fi宽带参数转换为JSON对象。
 */
static int _wifi_wide_params_to_json(cJSON *parent, const char *key, const linkg_wifi_wide_params_t *config)
{
    cJSON *object;
    int    ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _wifi_wide_params_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_uint16(object, "ap_bandwidth", (uint16_t)config->ap_bandwidth);
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

/**
 * @brief 将Wi-Fi宽带模式配置转换为JSON对象。
 */
static int _wifi_wideband_config_to_json(cJSON *parent, const char *key, const linkg_wifi_wideband_config_t *config)
{
    const char *work_mode;
    cJSON      *object;
    int         ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _wifi_wideband_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    work_mode = _wifi_work_mode_to_string(config->work_mode);
    if (work_mode == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "work_mode", work_mode);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = _wifi_narrow_params_to_json(object, "narrow_params", &config->narrow_params);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = _wifi_wide_params_to_json(object, "wide_params", &config->wide_params);
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

/****************************** 配置处理 ******************************/

/**
 * @brief 设置Wi-Fi配置默认值。
 */
void linkg_wifi_config_set_default(linkg_wifi_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));

    out->enabled = false;

    _wifi_ap_config_set_default(&out->ap);
    _wifi_sta_config_set_default(&out->sta);
    _wifi_wideband_config_set_default(&out->wideband);
}

/**
 * @brief 校验Wi-Fi配置。
 */
int linkg_wifi_config_validate(const linkg_wifi_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _wifi_wideband_config_validate(&config->wideband);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _wifi_ap_config_validate(&config->ap, config->wideband.work_mode);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    return _wifi_sta_config_validate(&config->sta);
}

/**
 * @brief 解析Wi-Fi配置。
 */
int linkg_wifi_config_parse(linkg_device_role_t role, const cJSON *node, linkg_wifi_config_t *out)
{
    linkg_wifi_config_t temp;
    const cJSON        *child;
    bool                found;
    int                 ret;

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

    // 默认值负责补全非当前角色和非当前工作模式配置。
    linkg_wifi_config_set_default(&temp);

    ret = linkg_json_get_bool(node, "enabled", &temp.enabled);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    switch (role)
    {
        case LINKG_DEVICE_ROLE_AP:
            child = NULL;
            ret = linkg_json_get_object(node, "ap", &child);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            ret = _wifi_ap_config_parse(child, &temp.ap);
            if (ret != CONFIG_OK)
            {
                return ret;
            }

            child = NULL;
            found = false;
            ret = linkg_json_try_get_object(node, "sta", &child, &found);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            if (found)
            {
                ret = _wifi_sta_config_parse(child, &temp.sta);
                if (ret != CONFIG_OK)
                {
                    return ret;
                }
            }
            break;

        case LINKG_DEVICE_ROLE_STA:
            child = NULL;
            ret = linkg_json_get_object(node, "sta", &child);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            ret = _wifi_sta_config_parse(child, &temp.sta);
            if (ret != CONFIG_OK)
            {
                return ret;
            }

            child = NULL;
            found = false;
            ret = linkg_json_try_get_object(node, "ap", &child, &found);
            if (ret != LINKG_JSON_OK)
            {
                return config_json_parse_error(ret);
            }

            if (found)
            {
                ret = _wifi_ap_config_parse(child, &temp.ap);
                if (ret != CONFIG_OK)
                {
                    return ret;
                }
            }
            break;

        default:
            return CONFIG_ERR_VALIDATE;
    }

    child = NULL;
    ret = linkg_json_get_object(node, "wideband", &child);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _wifi_wideband_config_parse(child, &temp.wideband);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_wifi_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 将Wi-Fi配置转换为JSON对象。
 */
int linkg_wifi_config_to_json(cJSON *parent, const char *key, const linkg_wifi_config_t *config)
{
    cJSON *object;
    int    ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_wifi_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_bool(object, "enabled", config->enabled);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = _wifi_ap_config_to_json(object, "ap", &config->ap, config->wideband.work_mode);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = _wifi_sta_config_to_json(object, "sta", &config->sta);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = _wifi_wideband_config_to_json(object, "wideband", &config->wideband);
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
