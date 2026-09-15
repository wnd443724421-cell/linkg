/**
 * @file web_handler_overview.c
 * @brief LinkG Web概览页面处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-15
 */

#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#include "linkg_config.h"
#include "linkg_discovery.h"
#include "linkg_json.h"
#include "linkg_node.h"
#include "linkg_time.h"
#include "linkg_wifi.h"
#include "linkg_cellular.h"

#include "web_internal.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_device_role_t role;               // 本机设备角色
    uint8_t             node_id;            // 本机节点编号
    uint32_t            network_node_count; // 当前组网在线节点数量，包含本机
    uint64_t            uptime_ms;          // 本次LinkG程序运行时间
} linkg_web_overview_device_info_t;

typedef struct
{
    linkg_device_role_t      mode;                          // 当前Wi-Fi角色模式
    char                     ssid[LINKG_WIFI_SSID_MAX + 1U];// 当前Wi-Fi SSID
    linkg_wifi_work_mode_t   work_mode;                     // 当前宽窄带工作模式
    linkg_wifi_narrow_mode_t narrow_mode;                   // 当前窄带速率控制模式
    uint16_t                 rate_level;                    // 当前固定窄带速率档位
    uint16_t                 bandwidth_mhz;                 // 当前宽带带宽
    uint16_t                 channel;                       // 当前工作信道
} linkg_web_overview_wifi_info_t;

typedef struct
{
    bool                          available;          // 当前蜂窝状态快照是否可用
    bool                          rsrp_valid;         // 当前RSRP是否有效
    int32_t                       rsrp_dbm;           // 当前RSRP
    bool                          plmn_valid;         // 当前PLMN是否有效
    char                          plmn[LINKG_CELLULAR_MCC_MAX_LENGTH + LINKG_CELLULAR_MNC_MAX_LENGTH + 1U]; // 当前PLMN
    linkg_cellular_network_type_t network_type;       // 当前实际网络类型
    bool                          ipv6_valid;         // 当前全局IPv6是否有效
    char                          ipv6[INET6_ADDRSTRLEN]; // 当前Host全局IPv6
    bool                          internet_available; // 当前公网是否验证可用
} linkg_web_overview_cellular_info_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 将设备角色转换为Web协议字符串。
 */
static const char *_linkg_web_overview_role_string(linkg_device_role_t role)
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
 * @brief 将Wi-Fi工作模式转换为Web协议字符串。
 */
static const char *_linkg_web_overview_wifi_work_mode_string(linkg_wifi_work_mode_t work_mode)
{
    if (work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        return "narrow";
    }

    if (work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        return "wide";
    }

    return NULL;
}

/**
 * @brief 将Wi-Fi窄带速率控制模式转换为Web协议字符串。
 */
static const char *_linkg_web_overview_wifi_narrow_mode_string(linkg_wifi_narrow_mode_t mode)
{
    if (mode == LINKG_WIFI_NARROW_MODE_FIXED)
    {
        return "fixed";
    }

    if (mode == LINKG_WIFI_NARROW_MODE_ADAPTIVE)
    {
        return "adaptive";
    }

    return NULL;
}

/**
 * @brief 将蜂窝网络类型转换为Web协议字符串。
 */
static const char *_linkg_web_overview_cellular_network_type_string(linkg_cellular_network_type_t network_type)
{
    if (network_type == LINKG_CELLULAR_NETWORK_TYPE_LTE)
    {
        return "lte";
    }

    if (network_type == LINKG_CELLULAR_NETWORK_TYPE_NR5G_SA)
    {
        return "5g_sa";
    }

    return "unknown";
}


/**
 * @brief 将JSON模块错误转换为Web Handler错误。
 */
static int _linkg_web_overview_json_error(int error)
{
    if (error == LINKG_JSON_ERR_MEMORY)
    {
        return -ENOMEM;
    }

    return -EINVAL;
}

/**
 * @brief 向JSON对象写入无符号64位整数。
 */
static int _linkg_web_overview_json_add_uint64(cJSON *object, const char *key, uint64_t value)
{
    cJSON *item;

    if (object == NULL || key == NULL || key[0] == '\0')
    {
        return LINKG_JSON_ERR_PARAM;
    }

    item = cJSON_AddNumberToObject(object, key, (double)value);
    if (item == NULL)
    {
        return LINKG_JSON_ERR_MEMORY;
    }

    return LINKG_JSON_OK;
}

/**
 * @brief 获取当前设备信息概览。
 */
static int _linkg_web_overview_get_device_info(linkg_web_overview_device_info_t *info)
{
    const linkg_node_info_t *local;
    uint32_t                 peer_count;
    int                      ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));

    local = linkg_node_get_local();
    if (local == NULL)
    {
        return -ENODEV;
    }

    info->role      = local->role;
    info->node_id   = local->node_id;
    info->uptime_ms = linkg_time_elapsed_ms();

    if (local->role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_node_get_peer_count(&peer_count);
        if (ret != 0)
        {
            return ret;
        }

        // AP组网节点 = 本机AP + 当前全部在线直接STA。
        info->network_node_count = peer_count + 1U;

        return 0;
    }

    if (local->role == LINKG_DEVICE_ROLE_STA)
    {
        // STA组网规模由Discovery保存的AP拓扑快照确定。
        return linkg_discovery_get_network_node_count(&info->network_node_count);
    }

    return -EINVAL;
}

/**
 * @brief 获取当前Wi-Fi信息概览。
 */
static int _linkg_web_overview_get_wifi_info(linkg_web_overview_wifi_info_t *info)
{
    linkg_wifi_status_snapshot_t snapshot;
    linkg_wifi_config_t          config;
    int                          ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));

    ret = linkg_config_get_wifi(&config);
    if (ret != 0)
    {
        return -EIO;
    }

    ret = linkg_wifi_get_status(&snapshot);
    if (ret != 0)
    {
        return ret;
    }

    info->mode      = snapshot.local.role;
    info->work_mode = snapshot.local.radio.work_mode;
    info->channel   = snapshot.local.radio.channel;

    if (info->mode == LINKG_DEVICE_ROLE_AP)
    {
        memcpy(info->ssid, config.ap.ssid, sizeof(info->ssid));
    }
    else if (info->mode == LINKG_DEVICE_ROLE_STA)
    {
        memcpy(info->ssid, config.sta.ssid, sizeof(info->ssid));
    }
    else
    {
        return -EINVAL;
    }

    info->ssid[LINKG_WIFI_SSID_MAX] = '\0';

    if (info->work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        info->narrow_mode = snapshot.local.radio.params.narrow.mode;

        if (info->narrow_mode == LINKG_WIFI_NARROW_MODE_FIXED)
        {
            info->rate_level = snapshot.local.radio.params.narrow.configured_rate_level;
        }

        return 0;
    }

    if (info->work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        info->bandwidth_mhz = (uint16_t)snapshot.local.radio.params.wide.bandwidth;
        return 0;
    }

    return -EINVAL;
}

/**
 * @brief 获取当前蜂窝网络信息概览。
 */
static int _linkg_web_overview_get_cellular_info(linkg_web_overview_cellular_info_t *info)
{
    linkg_cellular_status_snapshot_t snapshot;
    size_t                           mcc_length;
    size_t                           mnc_length;
    int                              ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));
    info->network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;

    ret = linkg_cellular_get_internet_available(&info->internet_available);
    if (ret != 0 && ret != -ENODEV)
    {
        return ret;
    }

    ret = linkg_cellular_get_status(&snapshot);
    if (ret == -ENODEV || ret == -ENETDOWN || ret == -EAGAIN)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    info->available    = true;
    info->network_type = snapshot.network.network_type;

    if (snapshot.network.serving_cell.rsrp_valid)
    {
        info->rsrp_valid = true;
        info->rsrp_dbm   = snapshot.network.serving_cell.rsrp_dbm;
    }

    if (snapshot.network.serving_cell.plmn_valid)
    {
        mcc_length = strlen(snapshot.network.serving_cell.mcc);
        mnc_length = strlen(snapshot.network.serving_cell.mnc);

        if (mcc_length + mnc_length < sizeof(info->plmn))
        {
            memcpy(info->plmn, snapshot.network.serving_cell.mcc, mcc_length);
            memcpy(info->plmn + mcc_length, snapshot.network.serving_cell.mnc, mnc_length + 1U);
            info->plmn_valid = true;
        }
    }

    if (snapshot.data.global_ipv6_valid && inet_ntop(AF_INET6, &snapshot.data.global_ipv6, info->ipv6, sizeof(info->ipv6)) != NULL)
    {
        info->ipv6_valid = true;
    }

    return 0;
}

/**
 * @brief 构造设备信息JSON对象。
 */
static int _linkg_web_overview_build_device_json(const linkg_web_overview_device_info_t *info, cJSON **out)
{
    const char *role;
    cJSON      *device;
    int         ret;

    if (info == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    role = _linkg_web_overview_role_string(info->role);
    if (role == NULL)
    {
        return -EINVAL;
    }

    device = cJSON_CreateObject();
    if (device == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(device, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_uint32(device, "node_id", info->node_id);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_uint32(device, "network_node_count", info->network_node_count);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        return _linkg_web_overview_json_error(ret);
    }

    ret = _linkg_web_overview_json_add_uint64(device, "uptime_ms", info->uptime_ms);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        return _linkg_web_overview_json_error(ret);
    }

    *out = device;

    return 0;
}

/**
 * @brief 构造Wi-Fi信息JSON对象。
 */
static int _linkg_web_overview_build_wifi_json(const linkg_web_overview_wifi_info_t *info, cJSON **out)
{
    const char *mode;
    const char *work_mode;
    const char *narrow_mode;
    cJSON      *wifi;
    int         ret;

    if (info == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    mode = _linkg_web_overview_role_string(info->mode);
    work_mode = _linkg_web_overview_wifi_work_mode_string(info->work_mode);

    if (mode == NULL || work_mode == NULL)
    {
        return -EINVAL;
    }

    wifi = cJSON_CreateObject();
    if (wifi == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(wifi, "mode", mode);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(wifi);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_string(wifi, "ssid", info->ssid);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(wifi);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_string(wifi, "work_mode", work_mode);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(wifi);
        return _linkg_web_overview_json_error(ret);
    }

    if (info->work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        narrow_mode = _linkg_web_overview_wifi_narrow_mode_string(info->narrow_mode);
        if (narrow_mode == NULL)
        {
            cJSON_Delete(wifi);
            return -EINVAL;
        }

        ret = linkg_json_add_string(wifi, "narrow_mode", narrow_mode);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(wifi);
            return _linkg_web_overview_json_error(ret);
        }

        if (info->narrow_mode == LINKG_WIFI_NARROW_MODE_FIXED)
        {
            ret = linkg_json_add_uint16(wifi, "rate_level", info->rate_level);
            if (ret != LINKG_JSON_OK)
            {
                cJSON_Delete(wifi);
                return _linkg_web_overview_json_error(ret);
            }
        }
    }
    else
    {
        ret = linkg_json_add_uint16(wifi, "bandwidth_mhz", info->bandwidth_mhz);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(wifi);
            return _linkg_web_overview_json_error(ret);
        }
    }

    ret = linkg_json_add_uint16(wifi, "channel", info->channel);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(wifi);
        return _linkg_web_overview_json_error(ret);
    }

    *out = wifi;

    return 0;
}

/**
 * @brief 构造蜂窝网络信息JSON对象。
 */
static int _linkg_web_overview_build_cellular_json(const linkg_web_overview_cellular_info_t *info, cJSON **out)
{
    const char *network_type;
    cJSON      *cellular;
    int         ret;

    if (info == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    network_type = _linkg_web_overview_cellular_network_type_string(info->network_type);

    cellular = cJSON_CreateObject();
    if (cellular == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_bool(cellular, "available", info->available);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(cellular);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_string(cellular, "network_type", network_type);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(cellular);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_bool(cellular, "internet_available", info->internet_available);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(cellular);
        return _linkg_web_overview_json_error(ret);
    }

    if (info->rsrp_valid)
    {
        ret = linkg_json_add_int(cellular, "rsrp_dbm", info->rsrp_dbm);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(cellular);
            return _linkg_web_overview_json_error(ret);
        }
    }

    if (info->plmn_valid)
    {
        ret = linkg_json_add_string(cellular, "plmn", info->plmn);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(cellular);
            return _linkg_web_overview_json_error(ret);
        }
    }

    if (info->ipv6_valid)
    {
        ret = linkg_json_add_string(cellular, "ipv6", info->ipv6);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(cellular);
            return _linkg_web_overview_json_error(ret);
        }
    }

    *out = cellular;

    return 0;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理Web概览状态查询。
 */
int _linkg_web_handler_overview_get(const cJSON *param, char **response)
{
    linkg_web_overview_device_info_t   device_info;
    linkg_web_overview_wifi_info_t     wifi_info;
    linkg_web_overview_cellular_info_t cellular_info;
    cJSON                             *device;
    cJSON                             *wifi;
    cJSON                             *cellular;
    cJSON                             *data;
    int                                ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = _linkg_web_overview_get_device_info(&device_info);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_OVERVIEW_GET, "获取设备信息失败", response);
    }

    ret = _linkg_web_overview_get_wifi_info(&wifi_info);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_OVERVIEW_GET, "获取Wi-Fi信息失败", response);
    }

    ret = _linkg_web_overview_get_cellular_info(&cellular_info);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_OVERVIEW_GET, "获取蜂窝网络信息失败", response);
    }

    device = NULL;
    wifi = NULL;

    ret = _linkg_web_overview_build_device_json(&device_info, &device);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_web_overview_build_wifi_json(&wifi_info, &wifi);
    if (ret != 0)
    {
        cJSON_Delete(device);
        return ret;
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(device);
        cJSON_Delete(wifi);
        return -ENOMEM;
    }

    ret = linkg_json_add_object(data, "device", device);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        cJSON_Delete(wifi);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_object(data, "wifi", wifi);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(wifi);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_OVERVIEW_GET, data, NULL, response);
}
