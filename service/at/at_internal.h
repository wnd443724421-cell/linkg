/**
 * @file at_internal.h
 * @brief LinkG AT服务内部接口
 */

#ifndef LINKG_AT_INTERNAL_H
#define LINKG_AT_INTERNAL_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>

/****************************** 模块常量 ******************************/

#define LINKG_AT_REQUEST_BUFFER_SIZE  1024U // 请求缓冲区大小，包含结束符
#define LINKG_AT_RESPONSE_BUFFER_SIZE 2048U // 响应缓冲区大小，包含结束符

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_AT_FORM_EXEC  = (1U << 0), // 执行形式
    LINKG_AT_FORM_SET   = (1U << 1), // 设置形式
    LINKG_AT_FORM_QUERY = (1U << 2), // 查询形式
    LINKG_AT_FORM_TEST  = (1U << 3)  // 测试形式
} linkg_at_form_t;

typedef struct
{
    struct sockaddr_in peer;            // UDP请求来源，仅用于寻址，不作为授权凭据
    struct in_addr     local_address;   // 请求命中的本机IPv4地址
    unsigned int       interface_index; // 请求入口接口索引
} linkg_at_request_context_t;

typedef int (*linkg_at_handler_t)(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size);

/****************************** 服务接口 ******************************/

int _linkg_at_server_init(void);
int _linkg_at_server_start(void);
int _linkg_at_server_stop(void);
int _linkg_at_server_deinit(void);

/****************************** 工厂权限 ******************************/

void _linkg_at_auth_reset(void);
int  _linkg_at_auth_grant(const linkg_at_request_context_t *context, const char *arguments);
bool _linkg_at_auth_allowed(const linkg_at_request_context_t *context);

/****************************** SSH维护 ******************************/

void _linkg_at_ssh_prepare(void);
int  _linkg_at_ssh_set(bool enable);
int  _linkg_at_ssh_get(bool *running);
int  _linkg_at_ssh_poll_timeout(void);
void _linkg_at_ssh_tick(void);
int  _linkg_at_ssh_shutdown(void);

/****************************** 请求分发 ******************************/

int _linkg_at_dispatch_request(const linkg_at_request_context_t *context, const char *request, size_t request_length, char *response, size_t response_size, size_t *response_length);

/****************************** 响应构造 ******************************/

int _linkg_at_response_success(const char *name, const char *data, size_t data_size, char *response, size_t response_size, size_t *response_length);
int _linkg_at_response_error(char *response, size_t response_size, size_t *response_length);

/****************************** 请求处理 ******************************/

int _linkg_at_handler_basic(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size);
int _linkg_at_handler_factory(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size);
int _linkg_at_handler_ssh(const linkg_at_request_context_t *context, linkg_at_form_t form, const char *arguments, char *data, size_t data_size);

#endif // LINKG_AT_INTERNAL_H
