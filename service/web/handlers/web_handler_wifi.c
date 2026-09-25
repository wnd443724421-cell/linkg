/**
 * @file web_handler_wifi.c
 * @brief LinkG Web Wi-Fi配置与状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-25
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <net/if_arp.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_network.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_path_probe.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_wifi.h"
#include "linkg_wifi_config.h"

/****************************** 模块常量 ******************************/

#define LINKG_WEB_WIFI_STATUS_MAX_AGE_MS 2400U // 允许三个Wi-Fi状态采集周期
#define LINKG_WEB_WIFI_PROBE_MAX_AGE_MS  3000U // 周期Probe超过三个发送周期后不展示RTT

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

/****************************** 状态协议转换 ******************************/

/**
 * @brief 将无线接口状态转换为Web协议字符串。
 */
static const char *_linkg_web_wifi_interface_state_string(linkg_wifi_interface_state_t state)
{
    if (state == LINKG_WIFI_INTERFACE_STATE_DOWN)
    {
        return "down";
    }

    if (state == LINKG_WIFI_INTERFACE_STATE_READY)
    {
        return "ready";
    }

    if (state == LINKG_WIFI_INTERFACE_STATE_CONNECTED)
    {
        return "connected";
    }

    return "unknown";
}

/**
 * @brief 将无线工作模式转换为Web协议字符串。
 */
static const char *_linkg_web_wifi_work_mode_string(linkg_wifi_work_mode_t mode)
{
    if (mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        return "narrow";
    }

    if (mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        return "wide";
    }

    return "unknown";
}

/**
 * @brief 将窄带速率模式转换为Web协议字符串。
 */
static const char *_linkg_web_wifi_narrow_mode_string(linkg_wifi_narrow_mode_t mode)
{
    if (mode == LINKG_WIFI_NARROW_MODE_FIXED)
    {
        return "fixed";
    }

    if (mode == LINKG_WIFI_NARROW_MODE_ADAPTIVE)
    {
        return "adaptive";
    }

    return "unknown";
}

/**
 * @brief 将对端连接状态转换为Web协议字符串。
 */
static const char *_linkg_web_wifi_peer_state_string(linkg_wifi_peer_state_t state)
{
    if (state == LINKG_WIFI_PEER_STATE_CONNECTED)
    {
        return "connected";
    }

    return "disconnected";
}

/****************************** 状态JSON辅助 ******************************/

/**
 * @brief 判断采集数据是否仍在允许的显示时间内。
 */
static bool _linkg_web_wifi_status_is_fresh(uint64_t updated_ms, uint64_t now_ms)
{
    return updated_ms != 0U &&
           updated_ms <= now_ms &&
           now_ms - updated_ms <= LINKG_WEB_WIFI_STATUS_MAX_AGE_MS;
}

/**
 * @brief 向JSON对象写入无符号64位整数。
 */
static int _linkg_web_wifi_status_add_uint64(cJSON *object, const char *key, uint64_t value)
{
    if (object == NULL || key == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    if (cJSON_AddNumberToObject(object, key, (double)value) == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    return LINKG_JSON_OK;
}

/**
 * @brief 将驱动累计计数写为十进制字符串，避免Web端丢失64位整数精度。
 */
static int _linkg_web_wifi_status_add_counter(cJSON *object, const char *key, uint64_t value)
{
    char text[32];
    int  written;

    if (object == NULL || key == NULL)
    {
        return LINKG_JSON_ERR_PARAM;
    }

    written = snprintf(text, sizeof(text), "%" PRIu64, value);
    if (written < 0 || (size_t)written >= sizeof(text))
    {
        return LINKG_JSON_ERR_PARAM;
    }

    return linkg_json_add_string(object, key, text);
}

/**
 * @brief 将MAC地址写入JSON对象。
 */
static int _linkg_web_wifi_status_add_mac(cJSON *object, const char *key, const uint8_t mac[LINKG_WIFI_MAC_LENGTH])
{
    char text[18];

    (void)snprintf(text, sizeof(text), "%02X:%02X:%02X:%02X:%02X:%02X",
                   mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return linkg_json_add_string(object, key, text);
}

/**
 * @brief 向对端JSON写入驱动累计统计。
 */
static int _linkg_web_wifi_status_add_driver(cJSON *object, const linkg_wifi_peer_status_t *peer)
{
    cJSON *driver;
    int    ret;

    driver = cJSON_CreateObject();
    if (driver == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    ret = _linkg_web_wifi_status_add_counter(driver, "tx_bytes", peer->driver_tx_bytes);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_counter(driver, "rx_bytes", peer->driver_rx_bytes);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_counter(driver, "tx_packets", peer->driver_tx_packets);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_counter(driver, "rx_packets", peer->driver_rx_packets);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_counter(driver, "tx_failed", peer->driver_tx_failed);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_object(object, "driver", driver);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    return LINKG_JSON_OK;

error:
    cJSON_Delete(driver);

    return ret;
}

/**
 * @brief 将Wi-Fi MAC对应到当前已注册的业务Path节点。
 *
 * Path的下一跳是Wi-Fi IPv4地址，末字节即Node ID。通过内核ARP确认
 * 该IPv4确实属于无线驱动上报的MAC，避免AP多对端时串用Probe结果。
 */
static int _linkg_web_wifi_status_peer_node_id(const uint8_t mac[LINKG_WIFI_MAC_LENGTH], uint8_t *node_id)
{
    linkg_path_endpoint_t endpoints[LINKG_NODE_PEER_MAX];
    const struct sockaddr_in *address;
    struct arpreq              arp;
    uint32_t                   count;
    uint32_t                   index;
    uint32_t                   link_id;
    uint32_t                   host_address;
    int                        socket_fd;
    int                        ret;

    if (mac == NULL || node_id == NULL)
    {
        return -EINVAL;
    }

    link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return -ENOENT;
    }

    ret = linkg_node_get_path_endpoints(link_id, endpoints, LINKG_NODE_PEER_MAX, &count);
    if (ret != 0)
    {
        return ret;
    }

    socket_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (socket_fd < 0)
    {
        return -errno;
    }

    ret = -ENOENT;

    for (index = 0U; index < count; index++)
    {
        address = (const struct sockaddr_in *)&endpoints[index].address;
        if (endpoints[index].length < sizeof(*address) || address->sin_family != AF_INET)
        {
            continue;
        }

        memset(&arp, 0, sizeof(arp));
        memcpy(&arp.arp_pa, address, sizeof(arp.arp_pa));
        (void)snprintf(arp.arp_dev, sizeof(arp.arp_dev), "%s", LINKG_RESOURCE_INTERFACE_WIFI);

        if (ioctl(socket_fd, SIOCGARP, &arp) != 0 ||
            (arp.arp_flags & ATF_COM) == 0 ||
            arp.arp_ha.sa_family != ARPHRD_ETHER ||
            memcmp(arp.arp_ha.sa_data, mac, LINKG_WIFI_MAC_LENGTH) != 0)
        {
            continue;
        }

        host_address = ntohl(address->sin_addr.s_addr);
        *node_id     = (uint8_t)(host_address & 0xFFU);
        ret          = 0;
        break;
    }

    (void)close(socket_fd);

    return ret;
}

/**
 * @brief 将数据业务的周期Probe RTT加入对端状态。
 */
static int _linkg_web_wifi_status_add_rtt(cJSON *object, const linkg_wifi_peer_status_t *peer)
{
    linkg_path_probe_peer_snapshot_t  snapshot;
    const linkg_path_probe_class_snapshot_t *probe;
    uint64_t                          now_us;
    uint8_t                           node_id;
    bool                              valid;
    int                               ret;

    valid = false;

    if (peer->state == LINKG_WIFI_PEER_STATE_CONNECTED &&
        _linkg_web_wifi_status_peer_node_id(peer->mac, &node_id) == 0 &&
        linkg_path_probe_get_peer_snapshot(node_id, &snapshot) == 0 &&
        snapshot.wifi.active)
    {
        probe  = &snapshot.wifi.classes[LINKG_TRANSPORT_CLASS_DATA];
        now_us = linkg_time_monotonic_us();
        valid  = probe->valid && probe->reachable &&
                 probe->updated_us <= now_us &&
                 now_us - probe->updated_us <=
                     (uint64_t)LINKG_WEB_WIFI_PROBE_MAX_AGE_MS * 1000U;

        if (valid)
        {
            ret = linkg_json_add_uint32(object, "rtt_us", probe->rtt_us);
            if (ret != LINKG_JSON_OK)
            {
                return ret;
            }
        }
    }

    return linkg_json_add_bool(object, "rtt_valid", valid);
}

/**
 * @brief 构造对端运行状态JSON。
 */
static int _linkg_web_wifi_status_build_peer(const linkg_wifi_peer_status_t *peer, uint64_t now_ms, cJSON **out)
{
    cJSON *object;
    bool   statistics_valid;
    int    ret;

    if (peer == NULL || out == NULL || !peer->valid)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    statistics_valid = peer->state == LINKG_WIFI_PEER_STATE_CONNECTED &&
                       peer->statistics_valid &&
                       _linkg_web_wifi_status_is_fresh(peer->statistics_updated_ms, now_ms);

    ret = _linkg_web_wifi_status_add_mac(object, "mac", peer->mac);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_rtt(object, peer);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(object, "state", _linkg_web_wifi_peer_state_string(peer->state));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "statistics_valid", statistics_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "connected_time_s", peer->connected_time_s);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_uint64(object, "updated_ms", peer->updated_ms);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (statistics_valid)
    {
        ret = linkg_json_add_int(object, "rssi_dbm", peer->rssi_dbm);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = linkg_json_add_uint32(object, "tx_rate_kbps", peer->tx_rate_kbps);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = linkg_json_add_uint32(object, "rx_rate_kbps", peer->rx_rate_kbps);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = linkg_json_add_uint32(object, "inactive_ms", peer->inactive_ms);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = _linkg_web_wifi_status_add_uint64(object, "statistics_updated_ms", peer->statistics_updated_ms);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = _linkg_web_wifi_status_add_driver(object, peer);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_wifi_json_error(ret);
}

/**
 * @brief 构造本机无线参数JSON。
 */
static int _linkg_web_wifi_status_build_radio(const linkg_wifi_radio_status_t *radio, cJSON **out)
{
    cJSON *object;
    cJSON *mode_params;
    int    ret;

    if (radio == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(object, "work_mode", _linkg_web_wifi_work_mode_string(radio->work_mode));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint32(object, "frequency_mhz", radio->frequency_mhz);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint16(object, "channel", radio->channel);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "noise_valid", radio->noise_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (radio->noise_valid)
    {
        ret = linkg_json_add_int(object, "noise_dbm", radio->noise_dbm);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    mode_params = cJSON_CreateObject();
    if (mode_params == NULL)
    {
        ret = LINKG_JSON_ERR_MEMORY;
        goto error;
    }

    if (radio->work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        ret = linkg_json_add_string(mode_params, "mode", _linkg_web_wifi_narrow_mode_string(radio->params.narrow.mode));
        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_uint16(mode_params, "configured_rate_level", radio->params.narrow.configured_rate_level);
        }
        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_bool(mode_params, "current_rate_valid", radio->params.narrow.current_rate_valid);
        }
        if (ret == LINKG_JSON_OK && radio->params.narrow.current_rate_valid)
        {
            ret = linkg_json_add_uint16(mode_params, "current_rate_level", radio->params.narrow.current_rate_level);
        }
        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_object(object, "narrow", mode_params);
        }
    }
    else if (radio->work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        ret = linkg_json_add_uint16(mode_params, "bandwidth_mhz", (uint16_t)radio->params.wide.bandwidth);
        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_object(object, "wide", mode_params);
        }
    }
    else
    {
        ret = LINKG_JSON_ERR_PARAM;
    }

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(mode_params);
        goto error;
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_wifi_json_error(ret);
}

/**
 * @brief 构造本机Wi-Fi运行状态JSON。
 */
static int _linkg_web_wifi_status_build_local(const linkg_wifi_local_status_t *local, cJSON **out)
{
    cJSON *object;
    cJSON *radio;
    int    ret;

    if (local == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(object, "interface_state", _linkg_web_wifi_interface_state_string(local->interface_state));
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_mac(object, "mac", local->mac);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = _linkg_web_wifi_status_add_uint64(object, "updated_ms", local->updated_ms);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_bool(object, "chip_temperature_valid", local->chip_temperature_valid);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (local->chip_temperature_valid)
    {
        ret = linkg_json_add_int(object, "chip_temperature_c", local->chip_temperature_c);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    radio = NULL;
    ret = _linkg_web_wifi_status_build_radio(&local->radio, &radio);
    if (ret != 0)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_json_add_object(object, "radio", radio);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(radio);
        goto error;
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(object);

    return _linkg_web_wifi_json_error(ret);
}

/**
 * @brief 构造AP多对端运行状态JSON。
 */
static int _linkg_web_wifi_status_build_ap(const linkg_wifi_ap_status_t *ap, uint64_t now_ms, cJSON **out)
{
    cJSON  *object;
    cJSON  *peers;
    cJSON  *peer;
    uint8_t peer_count;
    uint8_t connected_count;
    uint8_t index;
    bool    truncated;
    int     ret;

    if (ap == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    peers  = cJSON_CreateArray();
    if (object == NULL || peers == NULL)
    {
        cJSON_Delete(object);
        cJSON_Delete(peers);
        return -ENOMEM;
    }

    peer_count      = 0U;
    connected_count = 0U;
    truncated       = ap->peers_truncated || ap->peer_count > LINKG_WIFI_AP_PEER_MAX;

    for (index = 0U; index < ap->peer_count && index < LINKG_WIFI_AP_PEER_MAX; index++)
    {
        if (!ap->peers[index].valid)
        {
            continue;
        }

        peer = NULL;
        ret  = _linkg_web_wifi_status_build_peer(&ap->peers[index], now_ms, &peer);
        if (ret != 0)
        {
            cJSON_Delete(peers);
            cJSON_Delete(object);
            return ret;
        }

        if (!cJSON_AddItemToArray(peers, peer))
        {
            cJSON_Delete(peer);
            cJSON_Delete(peers);
            cJSON_Delete(object);
            return -ENOMEM;
        }

        peer_count++;
        if (ap->peers[index].state == LINKG_WIFI_PEER_STATE_CONNECTED)
        {
            connected_count++;
        }
    }

    ret = linkg_json_add_bool(object, "peers_truncated", truncated);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint16(object, "peer_count", peer_count);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_uint16(object, "connected_count", connected_count);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_array(object, "peers", peers);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    *out = object;

    return 0;

error:
    cJSON_Delete(peers);
    cJSON_Delete(object);

    return _linkg_web_wifi_json_error(ret);
}

/**
 * @brief 构造STA单对端运行状态JSON。
 */
static int _linkg_web_wifi_status_build_sta(const linkg_wifi_sta_status_t *sta, uint64_t now_ms, cJSON **out)
{
    cJSON *object;
    cJSON *peer;
    int    ret;

    if (sta == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return -ENOMEM;
    }

    if (!sta->peer.valid)
    {
        if (cJSON_AddNullToObject(object, "peer") == NULL)
        {
            cJSON_Delete(object);
            return -ENOMEM;
        }

        *out = object;
        return 0;
    }

    peer = NULL;
    ret = _linkg_web_wifi_status_build_peer(&sta->peer, now_ms, &peer);
    if (ret != 0)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_json_add_object(object, "peer", peer);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(peer);
        cJSON_Delete(object);
        return _linkg_web_wifi_json_error(ret);
    }

    *out = object;

    return 0;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理Wi-Fi运行状态查询。
 *
 * Wi-Fi链路关闭时仅返回链路启用标志，不读取无线模块状态。
 */
int _linkg_web_handler_wifi_status_get(const cJSON *param, char **response)
{
    linkg_wifi_status_snapshot_t snapshot;
    linkg_config_t               config;
    const char                  *role;
    cJSON                       *data;
    cJSON                       *local;
    cJSON                       *role_status;
    uint64_t                     now_ms;
    bool                         available;
    int                          ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_STATUS_GET, "获取全局配置失败", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_bool(data, "path_enabled", config.paths.wifi.enabled);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (!config.paths.wifi.enabled)
    {
        return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_STATUS_GET, data, NULL, response);
    }

    role = _linkg_web_wifi_role_string(config.device.role);
    if (role == NULL)
    {
        cJSON_Delete(data);
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_STATUS_GET, "设备角色无效", response);
    }

    ret = linkg_json_add_string(data, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_wifi_get_status(&snapshot);
    now_ms = linkg_time_elapsed_ms();
    available = ret == 0 && _linkg_web_wifi_status_is_fresh(snapshot.local.updated_ms, now_ms);

    ret = linkg_json_add_bool(data, "available", available);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (!available)
    {
        return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_STATUS_GET, data, NULL, response);
    }

    ret = linkg_json_add_bool(data, "partial", snapshot.partial);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    local = NULL;
    ret = _linkg_web_wifi_status_build_local(&snapshot.local, &local);
    if (ret != 0)
    {
        cJSON_Delete(data);
        return ret;
    }

    ret = linkg_json_add_object(data, "local", local);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(local);
        goto error;
    }

    role_status = NULL;
    if (snapshot.local.role == LINKG_DEVICE_ROLE_AP)
    {
        ret = _linkg_web_wifi_status_build_ap(&snapshot.role.ap, now_ms, &role_status);
    }
    else if (snapshot.local.role == LINKG_DEVICE_ROLE_STA)
    {
        ret = _linkg_web_wifi_status_build_sta(&snapshot.role.sta, now_ms, &role_status);
    }
    else
    {
        cJSON_Delete(data);
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_STATUS_GET, "WiFi运行角色无效", response);
    }

    if (ret != 0)
    {
        cJSON_Delete(data);
        return ret;
    }

    ret = linkg_json_add_object(data, role, role_status);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(role_status);
        goto error;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_STATUS_GET, data, NULL, response);

error:
    cJSON_Delete(data);

    return _linkg_web_wifi_json_error(ret);
}
