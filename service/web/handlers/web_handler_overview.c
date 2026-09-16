/**
 * @file web_handler_overview.c
 * @brief LinkG Web概览页面处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-15
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular.h"
#include "linkg_config.h"
#include "linkg_discovery.h"
#include "linkg_json.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_switch.h"
#include "linkg_time.h"
#include "linkg_transport.h"
#include "linkg_wifi.h"

/****************************** 内部常量 ******************************/

#define LINKG_WEB_OVERVIEW_WIFI_STATISTICS_MAX_AGE_MS 2400U // 允许三个Wi-Fi状态采集周期

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
    linkg_device_role_t      mode;                            // 当前Wi-Fi角色模式
    char                     ssid[LINKG_WIFI_SSID_MAX + 1U]; // 当前Wi-Fi SSID
    linkg_wifi_work_mode_t   work_mode;                       // 当前宽窄带工作模式
    linkg_wifi_narrow_mode_t narrow_mode;                     // 当前窄带速率控制模式
    uint16_t                 rate_level;                      // 当前固定窄带速率档位
    uint16_t                 bandwidth_mhz;                   // 当前宽带带宽
    uint16_t                 channel;                         // 当前工作信道
    int32_t                  noise_dbm;                       // 当前工作信道噪声
    bool                     noise_valid;                     // 当前噪声是否有效
    int32_t                  snr_db;                          // 当前STA接收AP的信噪比
    bool                     snr_valid;                       // 当前STA信噪比是否有效
} linkg_web_overview_wifi_info_t;

typedef struct
{
    bool                          available;                                                              // 当前蜂窝状态快照是否可用
    bool                          rsrp_valid;                                                             // 当前RSRP是否有效
    int32_t                       rsrp_dbm;                                                               // 当前RSRP
    bool                          plmn_valid;                                                             // 当前PLMN是否有效
    char                          plmn[LINKG_CELLULAR_MCC_MAX_LENGTH + LINKG_CELLULAR_MNC_MAX_LENGTH + 1U]; // 当前PLMN
    linkg_cellular_network_type_t network_type;                                                           // 当前实际网络类型
    bool                          ipv6_valid;                                                             // 当前全局IPv6是否有效
    char                          ipv6[INET6_ADDRSTRLEN];                                                 // 当前Host全局IPv6
    bool                          internet_available;                                                     // 当前公网是否验证可用
} linkg_web_overview_cellular_info_t;

typedef enum
{
    LINKG_WEB_OVERVIEW_NODE_RELATION_LOCAL = 0, // 本机节点
    LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT,    // 直接Peer
    LINKG_WEB_OVERVIEW_NODE_RELATION_VIA_AP     // 经AP转发的远端STA
} linkg_web_overview_node_relation_t;

typedef struct
{
    uint8_t                            node_id;             // 节点编号
    linkg_device_role_t                role;                // 节点角色
    linkg_web_overview_node_relation_t relation;            // 节点关系
    struct in_addr                     virtual_ip;          // 节点虚拟IPv4地址
    linkg_send_mode_t                  send_mode;           // 当前发送模式
    linkg_link_access_t                primary_access;      // 当前主链路
    bool                               wifi_path_valid;     // 当前Wi-Fi Path是否有效
    bool                               cellular_path_valid; // 当前Cellular Path是否有效
    bool                               traffic_valid;       // 当前Transport统计是否有效
    uint64_t                           tx_bytes;             // 累计用户业务发送字节
    uint64_t                           rx_bytes;             // 累计用户业务接收字节
} linkg_web_overview_node_info_t;

typedef struct
{
    uint32_t                       count;                                  // 当前组网节点数量
    linkg_web_overview_node_info_t nodes[LINKG_RESOURCE_NETWORK_NODE_MAX]; // 当前组网节点信息
} linkg_web_overview_nodes_info_t;

/****************************** 协议转换 ******************************/

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
 * @brief 将组网节点关系转换为Web协议字符串。
 */
static const char *_linkg_web_overview_node_relation_string(linkg_web_overview_node_relation_t relation)
{
    if (relation == LINKG_WEB_OVERVIEW_NODE_RELATION_LOCAL)
    {
        return "local";
    }

    if (relation == LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT)
    {
        return "direct";
    }

    if (relation == LINKG_WEB_OVERVIEW_NODE_RELATION_VIA_AP)
    {
        return "via_ap";
    }

    return NULL;
}

/**
 * @brief 将发送模式转换为Web协议字符串。
 */
static const char *_linkg_web_overview_send_mode_string(linkg_send_mode_t mode)
{
    if (mode == LINKG_SEND_MODE_NONE)
    {
        return "none";
    }

    if (mode == LINKG_SEND_MODE_SINGLE)
    {
        return "single";
    }

    if (mode == LINKG_SEND_MODE_REDUNDANT)
    {
        return "redundant";
    }

    return NULL;
}

/**
 * @brief 将链路接入类型转换为Web协议字符串。
 */
static const char *_linkg_web_overview_access_string(linkg_link_access_t access)
{
    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        return "wifi";
    }

    if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        return "cellular";
    }

    return "none";
}

/****************************** JSON辅助 ******************************/

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

/****************************** 节点辅助 ******************************/

/**
 * @brief 根据Link运行实例标识获取链路接入类型。
 */
static linkg_link_access_t _linkg_web_overview_link_id_access(uint32_t link_id)
{
    uint32_t wifi_link_id;
    uint32_t cellular_link_id;

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return LINKG_LINK_ACCESS_NONE;
    }

    wifi_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    if (wifi_link_id != LINKG_LINK_ID_INVALID && link_id == wifi_link_id)
    {
        return LINKG_LINK_ACCESS_WIFI;
    }

    if (cellular_link_id != LINKG_LINK_ID_INVALID && link_id == cellular_link_id)
    {
        return LINKG_LINK_ACCESS_CELLULAR;
    }

    return LINKG_LINK_ACCESS_NONE;
}

/**
 * @brief 判断指定直接Peer当前是否存在指定Access业务Path。
 */
static int _linkg_web_overview_get_path_available(uint8_t node_id, linkg_link_access_t access, bool *available)
{
    linkg_path_endpoint_t endpoint;
    linkg_path_t         *path;
    uint32_t              link_id;
    int                   ret;

    if (available == NULL)
    {
        return -EINVAL;
    }

    *available = false;

    link_id = linkg_link_manager_get_id(access);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    path = NULL;

    ret = linkg_node_acquire_path(node_id, link_id, &path, &endpoint);
    if (ret == -ENOENT || ret == -ENODEV)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    *available = true;

    linkg_path_release(path);

    return 0;
}

/**
 * @brief 获取直接Peer当前业务运行信息。
 */
static int _linkg_web_overview_get_direct_node_runtime(linkg_web_overview_node_info_t *info)
{
    linkg_transport_peer_stats_t stats;
    linkg_send_plan_t            plan;
    uint32_t                     class_index;
    int                          ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    info->send_mode           = LINKG_SEND_MODE_NONE;
    info->primary_access      = LINKG_LINK_ACCESS_NONE;
    info->wifi_path_valid     = false;
    info->cellular_path_valid = false;
    info->traffic_valid       = false;
    info->tx_bytes            = 0U;
    info->rx_bytes            = 0U;

    ret = linkg_switch_get_plan(info->node_id, &plan);
    if (ret == 0)
    {
        info->send_mode      = plan.mode;
        info->primary_access = _linkg_web_overview_link_id_access(plan.primary_link_id);
    }
    else if (ret != -ENOENT)
    {
        return ret;
    }

    ret = _linkg_web_overview_get_path_available(info->node_id, LINKG_LINK_ACCESS_WIFI, &info->wifi_path_valid);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_web_overview_get_path_available(info->node_id, LINKG_LINK_ACCESS_CELLULAR, &info->cellular_path_valid);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_transport_get_peer_stats(info->node_id, &stats);
    if (ret == -ENOENT)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        info->tx_bytes += stats.classes[class_index].tx_bytes;
        info->rx_bytes += stats.classes[class_index].rx_bytes;
    }

    info->traffic_valid = true;

    return 0;
}

/**
 * @brief 向Overview组网节点快照追加一个节点。
 */
static int _linkg_web_overview_append_node(linkg_web_overview_nodes_info_t *info, const linkg_network_config_t *network_config, uint8_t node_id, linkg_device_role_t role, linkg_web_overview_node_relation_t relation)
{
    linkg_web_overview_node_info_t *node;
    int                             ret;

    if (info == NULL || network_config == NULL)
    {
        return -EINVAL;
    }

    if (info->count >= LINKG_RESOURCE_NETWORK_NODE_MAX)
    {
        return -EOVERFLOW;
    }

    node = &info->nodes[info->count];

    memset(node, 0, sizeof(*node));

    node->node_id  = node_id;
    node->role     = role;
    node->relation = relation;

    ret = linkg_network_config_get_node_address(network_config, node_id, &node->virtual_ip);
    if (ret != 0)
    {
        return ret;
    }

    if (relation == LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT)
    {
        ret = _linkg_web_overview_get_direct_node_runtime(node);
        if (ret != 0)
        {
            return ret;
        }
    }

    info->count++;

    return 0;
}

/**
 * @brief 按Node ID升序整理Overview组网节点。
 */
static void _linkg_web_overview_sort_nodes(linkg_web_overview_nodes_info_t *info)
{
    linkg_web_overview_node_info_t current;
    uint32_t                       index;
    uint32_t                       position;

    if (info == NULL)
    {
        return;
    }

    for (index = 1U; index < info->count; index++)
    {
        current  = info->nodes[index];
        position = index;

        while (position > 0U && info->nodes[position - 1U].node_id > current.node_id)
        {
            info->nodes[position] = info->nodes[position - 1U];
            position--;
        }

        info->nodes[position] = current;
    }
}

/****************************** 状态采集 ******************************/

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
    uint64_t                     now_ms;
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

    info->mode        = snapshot.local.role;
    info->work_mode   = snapshot.local.radio.work_mode;
    info->channel     = snapshot.local.radio.channel;
    info->noise_valid = snapshot.local.radio.noise_valid;

    if (info->noise_valid)
    {
        info->noise_dbm = snapshot.local.radio.noise_dbm;
    }

    if (info->mode == LINKG_DEVICE_ROLE_AP)
    {
        memcpy(info->ssid, config.ap.ssid, sizeof(info->ssid));
    }
    else if (info->mode == LINKG_DEVICE_ROLE_STA)
    {
        memcpy(info->ssid, config.sta.ssid, sizeof(info->ssid));

        now_ms = linkg_time_elapsed_ms();

        if (info->noise_valid &&
            snapshot.role.sta.peer.valid &&
            snapshot.role.sta.peer.statistics_valid &&
            snapshot.role.sta.peer.state == LINKG_WIFI_PEER_STATE_CONNECTED &&
            snapshot.role.sta.peer.statistics_updated_ms != 0U &&
            now_ms >= snapshot.role.sta.peer.statistics_updated_ms &&
            now_ms - snapshot.role.sta.peer.statistics_updated_ms <= LINKG_WEB_OVERVIEW_WIFI_STATISTICS_MAX_AGE_MS)
        {
            info->snr_db    = snapshot.role.sta.peer.rssi_dbm - info->noise_dbm;
            info->snr_valid = true;
        }
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
 * @brief 获取当前完整组网节点概览。
 */
static int _linkg_web_overview_get_nodes_info(linkg_web_overview_nodes_info_t *info)
{
    linkg_discovery_topology_snapshot_t topology;
    linkg_node_peer_snapshot_t          peer;
    linkg_network_config_t              network_config;
    const linkg_node_info_t            *local;
    uint32_t                            peer_count;
    uint32_t                            found_count;
    uint32_t                            node_id;
    uint32_t                            index;
    int                                 ret;

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

    ret = linkg_config_get_network(&network_config);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_web_overview_append_node(info, &network_config, local->node_id, local->role, LINKG_WEB_OVERVIEW_NODE_RELATION_LOCAL);
    if (ret != 0)
    {
        return ret;
    }

    if (local->role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_node_get_peer_count(&peer_count);
        if (ret != 0)
        {
            return ret;
        }

        found_count = 0U;

        for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX && found_count < peer_count; node_id++)
        {
            ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &peer);
            if (ret == -ENOENT)
            {
                continue;
            }

            if (ret != 0)
            {
                return ret;
            }

            found_count++;

            if (peer.info.role != LINKG_DEVICE_ROLE_STA)
            {
                continue;
            }

            ret = _linkg_web_overview_append_node(info, &network_config, peer.info.node_id, peer.info.role, LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT);
            if (ret != 0)
            {
                return ret;
            }
        }

        _linkg_web_overview_sort_nodes(info);

        return 0;
    }

    if (local->role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &peer);
        if (ret == -ENOENT)
        {
            continue;
        }

        if (ret != 0)
        {
            return ret;
        }

        if (peer.info.role != LINKG_DEVICE_ROLE_AP)
        {
            continue;
        }

        ret = _linkg_web_overview_append_node(info, &network_config, peer.info.node_id, peer.info.role, LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT);
        if (ret != 0)
        {
            return ret;
        }

        break;
    }

    ret = linkg_discovery_get_topology_snapshot(&topology);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < topology.node_count; index++)
    {
        ret = _linkg_web_overview_append_node(info, &network_config, topology.node_ids[index], LINKG_DEVICE_ROLE_STA, LINKG_WEB_OVERVIEW_NODE_RELATION_VIA_AP);
        if (ret != 0)
        {
            return ret;
        }
    }

    _linkg_web_overview_sort_nodes(info);

    return 0;
}

/****************************** JSON构造 ******************************/

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

    if (info->noise_valid)
    {
        ret = linkg_json_add_int(wifi, "noise_dbm", info->noise_dbm);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(wifi);
            return _linkg_web_overview_json_error(ret);
        }
    }

    if (info->snr_valid)
    {
        ret = linkg_json_add_int(wifi, "snr_db", info->snr_db);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(wifi);
            return _linkg_web_overview_json_error(ret);
        }
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

/**
 * @brief 构造单个组网节点JSON对象。
 */
static int _linkg_web_overview_build_node_json(const linkg_web_overview_node_info_t *info, cJSON **out)
{
    const char *primary_link;
    const char *relation;
    const char *role;
    const char *send_mode;
    char        virtual_ip[INET_ADDRSTRLEN];
    cJSON      *node;
    cJSON      *traffic;
    int         ret;

    if (info == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    role = _linkg_web_overview_role_string(info->role);
    relation = _linkg_web_overview_node_relation_string(info->relation);

    if (role == NULL || relation == NULL)
    {
        return -EINVAL;
    }

    if (inet_ntop(AF_INET, &info->virtual_ip, virtual_ip, sizeof(virtual_ip)) == NULL)
    {
        return errno != 0 ? -errno : -EIO;
    }

    node = cJSON_CreateObject();
    if (node == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_uint32(node, "node_id", info->node_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(node, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(node, "relation", relation);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(node, "virtual_ip", virtual_ip);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (info->relation == LINKG_WEB_OVERVIEW_NODE_RELATION_DIRECT)
    {
        send_mode = _linkg_web_overview_send_mode_string(info->send_mode);
        primary_link = _linkg_web_overview_access_string(info->primary_access);

        if (send_mode == NULL)
        {
            cJSON_Delete(node);
            return -EINVAL;
        }

        ret = linkg_json_add_string(node, "send_mode", send_mode);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = linkg_json_add_string(node, "primary_link", primary_link);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = linkg_json_add_bool(node, "wifi_path", info->wifi_path_valid);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        ret = linkg_json_add_bool(node, "cellular_path", info->cellular_path_valid);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        if (info->traffic_valid)
        {
            traffic = cJSON_CreateObject();
            if (traffic == NULL)
            {
                cJSON_Delete(node);
                return -ENOMEM;
            }

            ret = _linkg_web_overview_json_add_uint64(traffic, "tx_bytes", info->tx_bytes);
            if (ret != LINKG_JSON_OK)
            {
                cJSON_Delete(traffic);
                goto error;
            }

            ret = _linkg_web_overview_json_add_uint64(traffic, "rx_bytes", info->rx_bytes);
            if (ret != LINKG_JSON_OK)
            {
                cJSON_Delete(traffic);
                goto error;
            }

            ret = linkg_json_add_object(node, "traffic", traffic);
            if (ret != LINKG_JSON_OK)
            {
                cJSON_Delete(traffic);
                goto error;
            }
        }
    }

    *out = node;

    return 0;

error:
    cJSON_Delete(node);
    return _linkg_web_overview_json_error(ret);
}

/**
 * @brief 构造完整组网节点JSON数组。
 */
static int _linkg_web_overview_build_nodes_json(const linkg_web_overview_nodes_info_t *info, cJSON **out)
{
    cJSON   *array;
    cJSON   *node;
    uint32_t index;
    int      ret;

    if (info == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    array = cJSON_CreateArray();
    if (array == NULL)
    {
        return -ENOMEM;
    }

    for (index = 0U; index < info->count; index++)
    {
        node = NULL;

        ret = _linkg_web_overview_build_node_json(&info->nodes[index], &node);
        if (ret != 0)
        {
            cJSON_Delete(array);
            return ret;
        }

        if (!cJSON_AddItemToArray(array, node))
        {
            cJSON_Delete(node);
            cJSON_Delete(array);
            return -ENOMEM;
        }
    }

    *out = array;

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
    linkg_web_overview_nodes_info_t    nodes_info;
    cJSON                             *device;
    cJSON                             *wifi;
    cJSON                             *cellular;
    cJSON                             *nodes;
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

    ret = _linkg_web_overview_get_nodes_info(&nodes_info);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_OVERVIEW_GET, "获取组网节点信息失败", response);
    }

    device   = NULL;
    wifi     = NULL;
    cellular = NULL;
    nodes    = NULL;

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

    ret = _linkg_web_overview_build_cellular_json(&cellular_info, &cellular);
    if (ret != 0)
    {
        cJSON_Delete(device);
        cJSON_Delete(wifi);
        return ret;
    }

    ret = _linkg_web_overview_build_nodes_json(&nodes_info, &nodes);
    if (ret != 0)
    {
        cJSON_Delete(device);
        cJSON_Delete(wifi);
        cJSON_Delete(cellular);
        return ret;
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(device);
        cJSON_Delete(wifi);
        cJSON_Delete(cellular);
        cJSON_Delete(nodes);
        return -ENOMEM;
    }

    ret = linkg_json_add_object(data, "device", device);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        cJSON_Delete(wifi);
        cJSON_Delete(cellular);
        cJSON_Delete(nodes);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_object(data, "wifi", wifi);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(wifi);
        cJSON_Delete(cellular);
        cJSON_Delete(nodes);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_object(data, "cellular", cellular);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(cellular);
        cJSON_Delete(nodes);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    ret = linkg_json_add_array(data, "nodes", nodes);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(nodes);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_OVERVIEW_GET, data, NULL, response);
}
