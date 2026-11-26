/**
 * @file at_dispatch.c
 * @brief LinkG AT请求解析与命令分发
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#include "at_internal.h"

#include <errno.h>
#include <string.h>
#include <strings.h>

/****************************** 内部类型 ******************************/

typedef struct
{
    const char        *name;    // 完整AT命令名，不包含参数
    unsigned int       forms;   // 支持的请求形式掩码
    linkg_at_handler_t handler; // 命令处理函数
} linkg_at_command_entry_t;

/****************************** 命令映射 ******************************/

static const linkg_at_command_entry_t g_at_commands[] =
{
    {"AT", LINKG_AT_FORM_EXEC, _linkg_at_handler_basic},
    {NULL, 0U,                 NULL}
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 复制并校验单条文本请求，拒绝NUL和内部换行。
 */
static int _linkg_at_normalize_request(const char *request, size_t request_length, char *command, size_t command_size)
{
    size_t begin;
    size_t end;
    size_t index;
    unsigned char value;

    if (request_length == 0U || request_length >= command_size)
    {
        return -EMSGSIZE;
    }

    begin = 0U;
    end   = request_length;

    while (begin < end && (request[begin] == ' ' || request[begin] == '\t'))
    {
        begin++;
    }

    while (end > begin && (request[end - 1U] == '\r' || request[end - 1U] == '\n' || request[end - 1U] == ' ' || request[end - 1U] == '\t'))
    {
        end--;
    }

    if (end - begin < 2U)
    {
        return -EINVAL;
    }

    for (index = begin; index < end; index++)
    {
        value = (unsigned char)request[index];
        if (value < 0x20U || value > 0x7eU)
        {
            return -EINVAL;
        }
    }

    memcpy(command, request + begin, end - begin);
    command[end - begin] = '\0';

    return 0;
}

/**
 * @brief 查找命令并严格匹配EXEC、SET、QUERY、TEST形式。
 */
static const linkg_at_command_entry_t *_linkg_at_find_handler(const char *command, linkg_at_form_t *form, const char **arguments)
{
    const linkg_at_command_entry_t *entry;
    const char                    *suffix;
    size_t                         name_length;
    size_t                         index;

    for (index = 0U; g_at_commands[index].handler != NULL; index++)
    {
        entry       = &g_at_commands[index];
        name_length = strlen(entry->name);

        if (strncasecmp(command, entry->name, name_length) != 0)
        {
            continue;
        }

        suffix = command + name_length;
        if (*suffix != '\0' && *suffix != '=' && *suffix != '?')
        {
            continue;
        }

        *arguments = NULL;

        if (*suffix == '\0')
        {
            *form = LINKG_AT_FORM_EXEC;
        }
        else if (strcmp(suffix, "?") == 0)
        {
            *form = LINKG_AT_FORM_QUERY;
        }
        else if (strcmp(suffix, "=?") == 0)
        {
            *form = LINKG_AT_FORM_TEST;
        }
        else if (*suffix == '=')
        {
            *form      = LINKG_AT_FORM_SET;
            *arguments = suffix + 1;
        }
        else
        {
            return NULL;
        }

        if ((entry->forms & (unsigned int)*form) == 0U)
        {
            return NULL;
        }

        return entry;
    }

    return NULL;
}

/****************************** 请求分发 ******************************/

/**
 * @brief 分发一条完整AT请求，协议错误统一返回ERROR响应。
 *
 * Handler同步执行，不能持有请求上下文和参数指针。
 * 尚未接入授权模块，命令表只注册无副作用的基础AT指令。
 */
int _linkg_at_dispatch_request(const linkg_at_request_context_t *context, const char *request, size_t request_length, char *response, size_t response_size, size_t *response_length)
{
    const linkg_at_command_entry_t *entry;
    const char                    *arguments;
    linkg_at_form_t                form;
    char                           command[LINKG_AT_REQUEST_BUFFER_SIZE];
    char                           data[LINKG_AT_RESPONSE_BUFFER_SIZE] = {0};
    int                            ret;

    if (response == NULL || response_size == 0U || response_length == NULL)
    {
        return -EINVAL;
    }

    *response_length = 0U;
    response[0] = '\0';

    if (context == NULL || request == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_at_normalize_request(request, request_length, command, sizeof(command));
    if (ret != 0)
    {
        return _linkg_at_response_error(response, response_size, response_length);
    }

    entry = _linkg_at_find_handler(command, &form, &arguments);
    if (entry == NULL)
    {
        return _linkg_at_response_error(response, response_size, response_length);
    }

    ret = entry->handler(context, form, arguments, data, sizeof(data));
    if (ret != 0)
    {
        return _linkg_at_response_error(response, response_size, response_length);
    }

    ret = _linkg_at_response_success(entry->name, data, sizeof(data), response, response_size, response_length);
    if (ret != 0)
    {
        return _linkg_at_response_error(response, response_size, response_length);
    }

    return 0;
}
