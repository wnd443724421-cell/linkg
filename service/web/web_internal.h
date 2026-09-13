/**
 * @file web_internal.h
 * @brief LinkG Web服务内部接口
 */

#ifndef LINKG_WEB_INTERNAL_H
#define LINKG_WEB_INTERNAL_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "linkg_system_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 命令定义 ******************************/

typedef enum
{
    LINKG_WEB_CMD_INVALID    = 0,    // 无效命令
    LINKG_WEB_CMD_HEADER_GET = 1001  // 获取Web固定头部状态
} linkg_web_cmd_t;

/****************************** 类型定义 ******************************/

typedef int (*linkg_web_handler_t)(const cJSON *param, char **response);

/****************************** 服务接口 ******************************/

int _linkg_web_server_init(void);
int _linkg_web_server_start(void);
int _linkg_web_server_stop(void);
int _linkg_web_server_deinit(void);

/****************************** 请求分发 ******************************/

int _linkg_web_dispatch_request(const char *request, size_t request_length, char **response, size_t *response_length);

/****************************** 响应构造 ******************************/

int _linkg_web_response_success(int cmd, cJSON *data, const char *message, char **response);
int _linkg_web_response_error(int cmd, const char *message, char **response);

/****************************** 请求处理 ******************************/

int _linkg_web_handler_header_get(const cJSON *param, char **response);

#ifdef __cplusplus
}
#endif

#endif
