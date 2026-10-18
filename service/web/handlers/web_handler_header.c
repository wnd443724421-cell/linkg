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
#include "linkg_node.h"

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
 *
 * 仅提供设备身份和AP直接Peer数量；业务发送计划统一交由Overview展示。
 */
int _linkg_web_handler_header_get(const cJSON *param, char **response)
{
    const linkg_node_info_t *local;
    const char              *role;
    cJSON                   *data;
    uint32_t                 peer_count;
    int                      ret;

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

    // 先完成Node状态获取，避免JSON构造后再处理跨模块查询失败。
    peer_count = 0U;
    if (local->role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_node_get_peer_count(&peer_count);
        if (ret != 0)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_HEADER_GET, "获取直连STA数量失败", response);
        }
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

    ret = linkg_json_add_uint32(data, "node_id", local->node_id);
    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (local->role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_json_add_uint32(data, "peer_count", peer_count);
        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_HEADER_GET, data, NULL, response);

error:
    cJSON_Delete(data);
    return _linkg_web_header_json_error(ret);
}
