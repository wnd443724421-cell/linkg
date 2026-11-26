/**
 * @file at_handler_factory.c
 * @brief LinkG AT工厂授权指令处理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>
#include <string.h>

/****************************** 请求处理 ******************************/

/**
 * @brief 处理AT+FACTORY设置指令。
 */
int _linkg_at_handler_factory(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size)
{
    int ret;

    if (form != LINKG_AT_FORM_SET || arguments == NULL || data == NULL)
    {
        return -EINVAL;
    }

    if (data_size < sizeof("FACTORY MODE"))
    {
        return -ENOSPC;
    }

    ret = _linkg_at_auth_grant(context, arguments);
    if (ret != 0)
    {
        return ret;
    }

    memcpy(data, "FACTORY MODE", sizeof("FACTORY MODE"));

    return 0;
}
