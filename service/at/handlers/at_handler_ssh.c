/**
 * @file at_handler_ssh.c
 * @brief LinkG AT SSH维护指令处理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>
#include <string.h>

/****************************** 请求处理 ******************************/

/**
 * @brief 处理已授权的AT+SSH设置、查询及测试指令。
 */
int _linkg_at_handler_ssh(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size)
{
    bool running;
    bool enable;
    int  ret;

    (void)context;

    if (data == NULL)
    {
        return -EINVAL;
    }

    if (data_size < sizeof("(on,off)"))
    {
        return -ENOSPC;
    }

    switch (form)
    {
        case LINKG_AT_FORM_SET:
            if (arguments == NULL)
            {
                return -EINVAL;
            }

            if (strcmp(arguments, "on") == 0)
            {
                enable = true;
            }
            else if (strcmp(arguments, "off") == 0)
            {
                enable = false;
            }
            else
            {
                return -EINVAL;
            }

            ret = _linkg_at_ssh_set(enable);
            if (ret != 0)
            {
                return ret;
            }

            running = enable;
            break;

        case LINKG_AT_FORM_QUERY:
            ret = _linkg_at_ssh_get(&running);
            if (ret != 0)
            {
                return ret;
            }
            break;

        case LINKG_AT_FORM_TEST:
            memcpy(data, "(on,off)", sizeof("(on,off)"));
            return 0;

        default:
            return -EINVAL;
    }

    if (running)
    {
        memcpy(data, "on", sizeof("on"));
    }
    else
    {
        memcpy(data, "off", sizeof("off"));
    }

    return 0;
}
