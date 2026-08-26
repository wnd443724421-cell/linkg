/**
 * @file wifi_service_ops.c
 * @brief LinkG Wi-Fi系统服务统一操作实现
 * @author Dawn
 * @version 1.1.1
 * @date 2026-08-26
 */

#include "wifi_service_ops.h"

#include <errno.h>

#include "wifi_hostapd.h"
#include "wifi_service_internal.h"
#include "wifi_wpa.h"

/****************************** 配置同步 ******************************/

/**
 * @brief 更新对应角色的Wi-Fi服务运行配置。
 */
int wifi_service_ops_update_config(linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    switch (role)
    {
        case LINKG_DEVICE_ROLE_AP:
            return wifi_hostapd_update_config(config);

        case LINKG_DEVICE_ROLE_STA:
            return wifi_wpa_update_config(config);

        default:
            return -EINVAL;
    }
}

/****************************** 服务控制 ******************************/

/**
 * @brief 启动对应角色的Wi-Fi系统服务。
 */
int wifi_service_ops_start(linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    switch (role)
    {
        case LINKG_DEVICE_ROLE_AP:
            return wifi_hostapd_start(config);

        case LINKG_DEVICE_ROLE_STA:
            return wifi_wpa_start(config);

        default:
            return -EINVAL;
    }
}

/**
 * @brief 停止对应角色的Wi-Fi系统服务。
 */
int wifi_service_ops_stop(linkg_device_role_t role)
{
    switch (role)
    {
        case LINKG_DEVICE_ROLE_AP:
            return wifi_hostapd_stop();

        case LINKG_DEVICE_ROLE_STA:
            return wifi_wpa_stop();

        default:
            return -EINVAL;
    }
}

/**
 * @brief 重启AP系统服务。
 *
 * @note STA重启必须由LinkG顶层按monitor stop、service restart、monitor start编排。
 */
int wifi_service_ops_restart(linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (role == LINKG_DEVICE_ROLE_STA)
    {
        WIFI_SERVICE_WARN("STA service restart requires LinkG lifecycle orchestration");
        return -EOPNOTSUPP;
    }

    if (role != LINKG_DEVICE_ROLE_AP)
    {
        return -EINVAL;
    }

    ret = wifi_service_ops_stop(role);
    if (ret != 0)
    {
        return ret;
    }

    return wifi_service_ops_start(role, config);
}

/****************************** STA连接控制 ******************************/

/**
 * @brief 主动断开STA当前连接。
 */
int wifi_service_ops_disconnect_sta(void)
{
    return wifi_wpa_disconnect();
}

/**
 * @brief 请求STA重新连接目标AP。
 */
int wifi_service_ops_reconnect_sta(void)
{
    return wifi_wpa_reconnect();
}
