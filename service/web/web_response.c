/**
 * @file web_response.c
 * @brief LinkG Web响应构造实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-13
 */

#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>

#include "linkg_json.h"

/****************************** 模块常量 ******************************/

#define LINKG_WEB_RESPONSE_CODE_OK      "ok"    // 请求处理成功
#define LINKG_WEB_RESPONSE_CODE_ERROR   "error" // 请求处理失败

/****************************** 内部辅助 ******************************/

/**
 * @brief 将JSON模块错误转换为Web模块错误。
 */
static int _linkg_web_response_json_error(int error)
{
    if (error == LINKG_JSON_ERR_MEMORY)
    {
        return -ENOMEM;
    }

    return -EINVAL;
}

/**
 * @brief 构造统一Web响应。
 *
 * data非NULL时本函数接管data所有权，无论成功或失败均由本函数负责释放。
 */
static int _linkg_web_response_make(int cmd, const char *code, cJSON *data, const char *message, char **response)
{
    cJSON *root;
    bool data_attached = false;
    int ret;

    if (code == NULL || response == NULL)
    {
        cJSON_Delete(data);
        return -EINVAL;
    }

    *response = NULL;

    root = cJSON_CreateObject();

    if (root == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    ret = linkg_json_add_int(root, "cmd", cmd);

    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    ret = linkg_json_add_string(root, "code", code);

    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    if (message != NULL && message[0] != '\0')
    {
        ret = linkg_json_add_string(root, "msg", message);

        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }
    }

    if (data != NULL)
    {
        ret = linkg_json_add_object(root, "data", data);

        if (ret != LINKG_JSON_OK)
        {
            goto error;
        }

        data_attached = true;
    }

    ret = linkg_json_print_unformatted(root, response);

    if (ret != LINKG_JSON_OK)
    {
        goto error;
    }

    cJSON_Delete(root);

    return 0;

error:
    if (data != NULL && !data_attached)
    {
        cJSON_Delete(data);
    }

    cJSON_Delete(root);

    return _linkg_web_response_json_error(ret);
}

/****************************** 响应构造 ******************************/

/**
 * @brief 构造成功Web响应。
 */
int _linkg_web_response_success(int cmd, cJSON *data, const char *message, char **response)
{
    return _linkg_web_response_make(cmd, LINKG_WEB_RESPONSE_CODE_OK, data, message, response);
}

/**
 * @brief 构造失败Web响应。
 */
int _linkg_web_response_error(int cmd, const char *message, char **response)
{
    if (message == NULL || message[0] == '\0')
    {
        return -EINVAL;
    }

    return _linkg_web_response_make(cmd, LINKG_WEB_RESPONSE_CODE_ERROR, NULL, message, response);
}
