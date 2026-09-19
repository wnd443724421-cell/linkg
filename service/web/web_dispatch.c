/**
 * @file web_dispatch.c
 * @brief LinkG Web请求解析与命令分发实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-13
 */

#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "linkg_json.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    int                 cmd;     // Web命令编号
    linkg_web_handler_t handler; // 命令处理函数
} linkg_web_command_entry_t;

/****************************** 命令映射 ******************************/

static const linkg_web_command_entry_t g_web_commands[] =
{
    {LINKG_WEB_CMD_HEADER_GET,        _linkg_web_handler_header_get},
    {LINKG_WEB_CMD_OVERVIEW_GET,      _linkg_web_handler_overview_get},
    {LINKG_WEB_CMD_SWITCH_STATUS_GET, _linkg_web_handler_switch_status_get},
    {LINKG_WEB_CMD_INVALID,           NULL}
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 根据命令编号查找Web处理函数。
 */
static linkg_web_handler_t _linkg_web_find_handler(int cmd)
{
    size_t index;

    for (index = 0U; g_web_commands[index].handler != NULL; index++)
    {
        if (g_web_commands[index].cmd == cmd)
        {
            return g_web_commands[index].handler;
        }
    }

    return NULL;
}

/****************************** 请求分发 ******************************/

/**
 * @brief 解析并分发一条完整Web请求。
 *
 * request由调用方持有，本函数只借用。
 * 成功返回的response所有权转移给调用方，必须使用linkg_json_string_free释放。
 */
int _linkg_web_dispatch_request(const char *request, size_t request_length, char **response, size_t *response_length)
{
    linkg_web_handler_t handler;
    const cJSON        *param;
    cJSON              *root;
    bool                param_found;
    int                 cmd;
    int                 ret;

    if (request == NULL || request_length == 0U || response == NULL || response_length == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    *response_length = 0U;

    root = NULL;
    param = NULL;
    param_found = false;
    cmd = LINKG_WEB_CMD_INVALID;

    ret = linkg_json_parse_buffer(request, request_length, &root);
    if (ret != LINKG_JSON_OK)
    {
        ret = _linkg_web_response_error(LINKG_WEB_CMD_INVALID, "请求JSON格式无效", response);
        if (ret != 0)
        {
            return ret;
        }

        *response_length = strlen(*response);

        return 0;
    }

    ret = linkg_json_get_int(root, "cmd", &cmd);
    if (ret != LINKG_JSON_OK)
    {
        ret = _linkg_web_response_error(LINKG_WEB_CMD_INVALID, "请求缺少有效的cmd字段", response);

        cJSON_Delete(root);

        if (ret != 0)
        {
            return ret;
        }

        *response_length = strlen(*response);

        return 0;
    }

    ret = linkg_json_try_get_object(root, "param", &param, &param_found);
    if (ret != LINKG_JSON_OK)
    {
        ret = _linkg_web_response_error(cmd, "param字段必须为JSON对象", response);

        cJSON_Delete(root);

        if (ret != 0)
        {
            return ret;
        }

        *response_length = strlen(*response);

        return 0;
    }

    if (!param_found)
    {
        param = NULL;
    }

    handler = _linkg_web_find_handler(cmd);
    if (handler == NULL)
    {
        ret = _linkg_web_response_error(cmd, "不支持的Web命令", response);

        cJSON_Delete(root);

        if (ret != 0)
        {
            return ret;
        }

        *response_length = strlen(*response);

        return 0;
    }

    ret = handler(param, response);

    cJSON_Delete(root);

    if (ret != 0)
    {
        return ret;
    }

    if (*response == NULL)
    {
        return -EPROTO;
    }

    *response_length = strlen(*response);

    return 0;
}
