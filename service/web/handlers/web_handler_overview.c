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

#include "linkg_discovery.h"
#include "linkg_json.h"
#include "linkg_node.h"
#include "linkg_time.h"

#include "web_internal.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_device_role_t role;               // 本机设备角色
    uint8_t             node_id;            // 本机节点编号
    uint32_t            network_node_count; // 当前组网在线节点数量，包含本机
    uint64_t            uptime_ms;          // 本次LinkG程序运行时间
} linkg_web_overview_device_info_t;

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

/****************************** 请求处理 ******************************/

/**
 * @brief 处理Web概览状态查询。
 */
int _linkg_web_handler_overview_get(const cJSON *param, char **response)
{
    linkg_web_overview_device_info_t info;
    cJSON                           *device;
    cJSON                           *data;
    int                              ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    ret = _linkg_web_overview_get_device_info(&info);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_OVERVIEW_GET, "获取设备信息失败", response);
    }

    device = NULL;

    ret = _linkg_web_overview_build_device_json(&info, &device);
    if (ret != 0)
    {
        return ret;
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(device);
        return -ENOMEM;
    }

    ret = linkg_json_add_object(data, "device", device);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(device);
        cJSON_Delete(data);
        return _linkg_web_overview_json_error(ret);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_OVERVIEW_GET, data, NULL, response);
}
