/**
 * @file web_handler_wifi.c
 * @brief LinkG Web Wi-Fi配置与状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-25
 */
#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_network.h"
#include "linkg_wifi_config.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 将设备角色转换为Web协议字符串。
 */
static const char *_linkg_web_wifi_role_string(linkg_device_role_t role)
{
    if (role == LINKG_DEVICE_ROLE_AP)
    {
        return "ap";
    }

    if (role == LINKG_DEVICE_ROLE_STA)
    {
        return "sta";
    }

    return NULL;
}

/**
 * @brief 将JSON模块错误转换为Web Handler错误。
 */
static int _linkg_web_wifi_json_error(int error)
{
    if (error == LINKG_JSON_ERR_MEMORY)
    {
        return -ENOMEM;
    }

    return -EINVAL;
}

/**
 * @brief 判断Web管理的Wi-Fi配置是否完全相同。
 */
static bool _linkg_web_wifi_config_equal(const linkg_config_t *left, const linkg_config_t *right)
{
    const linkg_wifi_config_t *left_wifi;
    const linkg_wifi_config_t *right_wifi;

    if (left == NULL || right == NULL)
    {
        return false;
    }

    if (left->paths.wifi.enabled != right->paths.wifi.enabled)
    {
        return false;
    }

    left_wifi  = &left->links.wifi;
    right_wifi = &right->links.wifi;

    return left_wifi->enabled == right_wifi->enabled &&
           strcmp(left_wifi->ap.ssid, right_wifi->ap.ssid) == 0 &&
           strcmp(left_wifi->ap.password, right_wifi->ap.password) == 0 &&
           left_wifi->ap.channel == right_wifi->ap.channel &&
           left_wifi->ap.security == right_wifi->ap.security &&
           strcmp(left_wifi->sta.ssid, right_wifi->sta.ssid) == 0 &&
           strcmp(left_wifi->sta.password, right_wifi->sta.password) == 0 &&
           left_wifi->sta.security == right_wifi->sta.security &&
           left_wifi->wideband.work_mode == right_wifi->wideband.work_mode &&
           left_wifi->wideband.narrow_params.mode == right_wifi->wideband.narrow_params.mode &&
           left_wifi->wideband.narrow_params.bandwidth == right_wifi->wideband.narrow_params.bandwidth &&
           left_wifi->wideband.narrow_params.manual_rate == right_wifi->wideband.narrow_params.manual_rate &&
           left_wifi->wideband.wide_params.ap_bandwidth == right_wifi->wideband.wide_params.ap_bandwidth;
}

/**
 * @brief 判断本次修改是否仅涉及窄带速率模式和速率。
 */
static bool _linkg_web_wifi_narrow_only_changed(const linkg_config_t *old_config, const linkg_config_t *new_config)
{
    linkg_config_t temp;

    if (old_config == NULL || new_config == NULL)
    {
        return false;
    }

    temp = *old_config;

    temp.links.wifi.wideband.narrow_params.mode        = new_config->links.wifi.wideband.narrow_params.mode;
    temp.links.wifi.wideband.narrow_params.manual_rate = new_config->links.wifi.wideband.narrow_params.manual_rate;

    return _linkg_web_wifi_config_equal(&temp, new_config);
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理Wi-Fi配置查询。
 */
int _linkg_web_handler_wifi_config_get(const cJSON *param, char **response)
{
    linkg_config_t config;
    const char    *role;
    cJSON         *data;
    int            ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_GET, "获取全局配置失败", response);
    }

    role = _linkg_web_wifi_role_string(config.device.role);
    if (role == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_GET, "设备角色无效", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(data, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(data, "path_enabled", config.paths.wifi.enabled);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_wifi_config_to_json(data, "wifi", &config.links.wifi);
    if (ret != 0)
    {
        cJSON_Delete(data);
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_GET, "构造WiFi配置失败", response);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_CONFIG_GET, data, NULL, response);

error:
    cJSON_Delete(data);

    return _linkg_web_wifi_json_error(ret);
}

/**
 * @brief 处理Wi-Fi配置更新。
 */
int _linkg_web_handler_wifi_config_set(const cJSON *param, char **response)
{
    linkg_config_t old_config;
    linkg_config_t new_config;
    const cJSON   *wifi;
    cJSON         *data;
    bool           dynamic_only;
    int            ret;

    if (param == NULL || response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_config_create_snapshot(&old_config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "获取当前配置失败", response);
    }

    new_config = old_config;

    ret = linkg_json_get_bool(param, "path_enabled", &new_config.paths.wifi.enabled);
    if (ret != LINKG_JSON_OK)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "WiFi路径配置无效", response);
    }

    ret = linkg_json_get_object(param, "wifi", &wifi);
    if (ret != LINKG_JSON_OK)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "WiFi配置无效", response);
    }

    ret = linkg_wifi_config_parse(new_config.device.role, wifi, &new_config.links.wifi);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "WiFi配置校验失败", response);
    }

    if (!new_config.links.wifi.enabled)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "WiFi模块必须保持启用", response);
    }

    if (_linkg_web_wifi_config_equal(&old_config, &new_config))
    {
        data = cJSON_CreateObject();
        if (data == NULL)
        {
            return -ENOMEM;
        }

        ret = linkg_json_add_string(data, "apply", "none");
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_wifi_json_error(ret);
        }

        return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_CONFIG_SET, data, NULL, response);
    }

    dynamic_only = _linkg_web_wifi_narrow_only_changed(&old_config, &new_config);

    ret = linkg_config_save(&new_config, NULL);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "保存WiFi配置失败", response);
    }

    ret = linkg_config_replace(&new_config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "更新全局配置失败", response);
    }

    if (dynamic_only)
    {
        ret = linkg_network_set_wifi_narrow_config(new_config.links.wifi.wideband.narrow_params.mode, new_config.links.wifi.wideband.narrow_params.manual_rate);
        if (ret != 0)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "动态更新WiFi配置失败", response);
        }
    }
    else
    {
        ret = linkg_network_set_wifi_config(&new_config.links.wifi);
        if (ret != 0)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "更新Network WiFi配置失败", response);
        }

        ret = linkg_network_restart_wifi();
        if (ret != 0)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_CONFIG_SET, "请求WiFi重启失败", response);
        }
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(data, "apply", dynamic_only ? "dynamic" : "restart");
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(data);
        return _linkg_web_wifi_json_error(ret);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_CONFIG_SET, data, NULL, response);
}

int _linkg_web_handler_wifi_status_get(const cJSON *param, char **response)
{
	return 0;
}
