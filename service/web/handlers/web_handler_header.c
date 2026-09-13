/**
 * @file web_handler_header.c
 * @brief LinkG Web固定头部状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-14
 */

#include "web_internal.h"

#include <errno.h>
#include <stdint.h>

#include "linkg_json.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_switch.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t peer_count;     // 当前直接STA数量
    uint32_t wifi_count;     // 当前以Wi-Fi作为主链路的STA数量
    uint32_t cellular_count; // 当前以5G作为主链路的STA数量
    uint32_t none_count;     // 当前无可用主链路的STA数量
} linkg_web_header_ap_summary_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 将设备角色转换为Web协议字符串。
 */
static const char *_linkg_web_header_role_string(linkg_device_role_t role)
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
 * @brief 将链路接入类型转换为Web协议字符串。
 */
static const char *_linkg_web_header_access_string(linkg_link_access_t access)
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

/**
 * @brief 获取指定直接Peer当前发送计划的主链路类型。
 */
static int _linkg_web_header_get_primary_access(uint8_t peer_node_id, linkg_link_access_t *access)
{
    linkg_send_plan_t plan;
    uint32_t          wifi_link_id;
    uint32_t          cellular_link_id;
    int               ret;

    if (access == NULL)
    {
        return -EINVAL;
    }

    *access = LINKG_LINK_ACCESS_NONE;

    ret = linkg_switch_get_plan(peer_node_id, &plan);
    if (ret == -ENOENT)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (plan.mode == LINKG_SEND_MODE_NONE || plan.primary_link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    wifi_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    if (wifi_link_id != LINKG_LINK_ID_INVALID && plan.primary_link_id == wifi_link_id)
    {
        *access = LINKG_LINK_ACCESS_WIFI;
        return 0;
    }

    if (cellular_link_id != LINKG_LINK_ID_INVALID && plan.primary_link_id == cellular_link_id)
    {
        *access = LINKG_LINK_ACCESS_CELLULAR;
        return 0;
    }

    return 0;
}

/**
 * @brief 获取STA当前直接AP的主用链路。
 */
static int _linkg_web_header_get_sta_active_link(linkg_link_access_t *access)
{
    linkg_node_peer_snapshot_t snapshot;
    uint32_t                   node_id;
    int                        ret;

    if (access == NULL)
    {
        return -EINVAL;
    }

    *access = LINKG_LINK_ACCESS_NONE;

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &snapshot);
        if (ret == -ENOENT)
        {
            continue;
        }

        if (ret != 0)
        {
            return ret;
        }

        if (snapshot.info.role != LINKG_DEVICE_ROLE_AP)
        {
            continue;
        }

        return _linkg_web_header_get_primary_access(snapshot.info.node_id, access);
    }

    return 0;
}

/**
 * @brief 汇总AP全部直接STA当前主链路分布。
 */
static int _linkg_web_header_get_ap_summary(linkg_web_header_ap_summary_t *summary)
{
    linkg_node_peer_snapshot_t snapshot;
    linkg_link_access_t        access;
    uint32_t                   node_id;
    int                        ret;

    if (summary == NULL)
    {
        return -EINVAL;
    }

    summary->peer_count = 0U;
    summary->wifi_count = 0U;
    summary->cellular_count = 0U;
    summary->none_count = 0U;

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &snapshot);
        if (ret == -ENOENT)
        {
            continue;
        }

        if (ret != 0)
        {
            return ret;
        }

        if (snapshot.info.role != LINKG_DEVICE_ROLE_STA)
        {
            continue;
        }

        summary->peer_count++;

        ret = _linkg_web_header_get_primary_access(snapshot.info.node_id, &access);
        if (ret != 0)
        {
            return ret;
        }

        if (access == LINKG_LINK_ACCESS_WIFI)
        {
            summary->wifi_count++;
        }
        else if (access == LINKG_LINK_ACCESS_CELLULAR)
        {
            summary->cellular_count++;
        }
        else
        {
            summary->none_count++;
        }
    }

    return 0;
}

/**
 * @brief 将JSON模块错误转换为Web Handler错误。
 */
static int _linkg_web_header_json_error(int error)
{
    if (error == LINKG_JSON_ERR_MEMORY)
    {
        return -ENOMEM;
    }

    return -EINVAL;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理Web固定头部状态查询。
 */
int _linkg_web_handler_header_get(const cJSON *param, char **response)
{
    linkg_web_header_ap_summary_t summary;
    const linkg_node_info_t      *local;
    const char                   *role;
    const char                   *active_link;
    linkg_link_access_t           access;
    cJSON                        *data;
    int                           ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    local = linkg_node_get_local();
    if (local == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_HEADER_GET, "获取本机节点状态失败", response);
    }

    role = _linkg_web_header_role_string(local->role);
    if (role == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_HEADER_GET, "本机设备角色无效", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(data, "role", role);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(data);
        return _linkg_web_header_json_error(ret);
    }

    ret = linkg_json_add_uint32(data, "node_id", local->node_id);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(data);
        return _linkg_web_header_json_error(ret);
    }

    if (local->role == LINKG_DEVICE_ROLE_STA)
    {
        ret = _linkg_web_header_get_sta_active_link(&access);
        if (ret != 0)
        {
            cJSON_Delete(data);
            return _linkg_web_response_error(LINKG_WEB_CMD_HEADER_GET, "获取当前主链路失败", response);
        }

        active_link = _linkg_web_header_access_string(access);

        ret = linkg_json_add_string(data, "active_link", active_link);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_header_json_error(ret);
        }
    }
    else
    {
        ret = _linkg_web_header_get_ap_summary(&summary);
        if (ret != 0)
        {
            cJSON_Delete(data);
            return _linkg_web_response_error(LINKG_WEB_CMD_HEADER_GET, "获取对端链路状态失败", response);
        }

        ret = linkg_json_add_uint32(data, "peer_count", summary.peer_count);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_header_json_error(ret);
        }

        ret = linkg_json_add_uint32(data, "wifi_count", summary.wifi_count);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_header_json_error(ret);
        }

        ret = linkg_json_add_uint32(data, "cellular_count", summary.cellular_count);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_header_json_error(ret);
        }

        ret = linkg_json_add_uint32(data, "none_count", summary.none_count);
        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(data);
            return _linkg_web_header_json_error(ret);
        }
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_HEADER_GET, data, NULL, response);
}
