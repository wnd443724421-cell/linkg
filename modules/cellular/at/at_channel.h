/**
 * @file at_channel.h
 * @brief Cellular AT通道管理接口
 */

#ifndef AT_CHANNEL_H
#define AT_CHANNEL_H

#include <stdbool.h>

#include "linkg_uart.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef struct at_channel at_channel_t;

typedef void (*at_urc_callback_t)(const char *line, void *context);

typedef struct
{
    int         timeout_ms;        // 命令超时时间，单位ms
    const char *expect_prefix;     // 期望响应行前缀，NULL表示无指定前缀
    bool        accept_plain_text; // 是否接受无前缀普通文本响应
} at_command_config_t;

/****************************** 生命周期 ******************************/

at_channel_t *at_channel_create(const char *device, const uart_config_t *config);
int           at_channel_start(at_channel_t *channel);
int           at_channel_stop(at_channel_t *channel);
void          at_channel_destroy(at_channel_t *channel);

/****************************** 命令接口 ******************************/

int at_channel_exec(at_channel_t *channel, const char *command, const at_command_config_t *config, char *response, int response_size);

/****************************** URC接口 ******************************/

int at_channel_register_urc(at_channel_t *channel, at_urc_callback_t callback, void *context);

#ifdef __cplusplus
}
#endif

#endif
