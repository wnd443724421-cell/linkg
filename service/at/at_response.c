/**
 * @file at_response.c
 * @brief LinkG AT响应构造
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

/****************************** 模块常量 ******************************/

#define LINKG_AT_RESPONSE_OK    "OK\r\n"    // 成功响应结束符
#define LINKG_AT_RESPONSE_ERROR "ERROR\r\n" // 错误响应结束符

/****************************** 响应构造 ******************************/

/**
 * @brief 构造业务响应及成功结束符，不发送截断响应。
 */
int _linkg_at_response_success(const char *name, const char *data, size_t data_size, char *response, size_t response_size, size_t *response_length)
{
    int length;

    if (response == NULL || response_size == 0U || response_length == NULL)
    {
        return -EINVAL;
    }

    *response_length = 0U;
    response[0] = '\0';

    if (name == NULL || data == NULL || data_size == 0U || memchr(data, '\0', data_size) == NULL)
    {
        return -EINVAL;
    }

    if (data[0] == '\0')
    {
        length = snprintf(response, response_size, "%s", LINKG_AT_RESPONSE_OK);
    }
    else
    {
        length = snprintf(response, response_size, "%s:%s\r\n%s", name, data, LINKG_AT_RESPONSE_OK);
    }

    if (length < 0 || (size_t)length >= response_size)
    {
        response[0] = '\0';
        return -EMSGSIZE;
    }

    *response_length = (size_t)length;

    return 0;
}

/**
 * @brief 构造统一错误响应。
 */
int _linkg_at_response_error(char *response, size_t response_size, size_t *response_length)
{
    if (response == NULL || response_size == 0U || response_length == NULL)
    {
        return -EINVAL;
    }

    *response_length = 0U;
    response[0] = '\0';

    if (response_size < sizeof(LINKG_AT_RESPONSE_ERROR))
    {
        return -EMSGSIZE;
    }

    memcpy(response, LINKG_AT_RESPONSE_ERROR, sizeof(LINKG_AT_RESPONSE_ERROR));
    *response_length = sizeof(LINKG_AT_RESPONSE_ERROR) - 1U;

    return 0;
}
