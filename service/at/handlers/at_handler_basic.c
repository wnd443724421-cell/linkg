/**
 * @file at_handler_basic.c
 * @brief LinkG AT基础指令处理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>

/****************************** 请求处理 ******************************/

/**
 * @brief 处理AT服务连通性检测。
 */
int _linkg_at_handler_basic(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size)
{
    (void)context;

    if (form != LINKG_AT_FORM_EXEC || arguments != NULL || data == NULL || data_size == 0U)
    {
        return -EINVAL;
    }

    data[0] = '\0';

    return 0;
}
