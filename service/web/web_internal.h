/**
 * @file web_internal.h
 * @brief LinkG Web服务内部接口
 */

#ifndef LINKG_WEB_INTERNAL_H
#define LINKG_WEB_INTERNAL_H

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"
#include "linkg_system_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 命令定义 ******************************/

typedef enum
{
    LINKG_WEB_CMD_INVALID               = 0,    // 无效命令
    LINKG_WEB_CMD_HEADER_GET            = 1001, // 获取Web固定头部状态
    LINKG_WEB_CMD_OVERVIEW_GET          = 1002, // 获取概览界面的参数
    LINKG_WEB_CMD_SWITCH_STATUS_GET     = 1003, // 获取Switch运行状态
    LINKG_WEB_CMD_WIFI_CONFIG_GET       = 1004, // 获取WiFi配置
    LINKG_WEB_CMD_WIFI_CONFIG_SET       = 1005, // 设置WiFi配置
    LINKG_WEB_CMD_WIFI_STATUS_GET       = 1006, // 获取WiFi运行状态
    LINKG_WEB_CMD_CELLULAR_CONFIG_GET   = 1007, // 获取蜂窝网络配置
    LINKG_WEB_CMD_CELLULAR_CONFIG_SET   = 1008, // 设置蜂窝网络配置
    LINKG_WEB_CMD_CELLULAR_STATUS_GET   = 1009, // 获取蜂窝网络运行状态
    LINKG_WEB_CMD_TRAFFIC_CONFIG_GET    = 1010, // 获取业务流量分类规则
    LINKG_WEB_CMD_UART_CONFIG_GET       = 1012, // 获取串口配置
    LINKG_WEB_CMD_UART_CONFIG_SET       = 1013, // 异步应用串口配置
    LINKG_WEB_CMD_UART_STATUS_GET       = 1014, // 获取串口运行和应用状态
    LINKG_WEB_CMD_TRAFFIC_CONFIG_SET    = 1011, // 设置业务流量分类规则
    LINKG_WEB_CMD_NETWORK_BASIC_GET     = 1015, // 获取运行及已保存基础网络
    LINKG_WEB_CMD_NETWORK_BASIC_SET     = 1016, // 保存基础网络及DHCP配置
    LINKG_WEB_CMD_NETWORK_BASIC_RESTART = 1017, // 确认后异步重启全部服务
    LINKG_WEB_CMD_DEVICE_INFO_GET       = 1018, // 设备信息及角色状态
    LINKG_WEB_CMD_DEVICE_ROLE_SET       = 1019, // 保存AP/STA角色
    LINKG_WEB_CMD_DEVICE_RESTART        = 1020, // 确认重启全部服务
    LINKG_WEB_CMD_UPGRADE_START         = 1021, // 启动升级包校验与安装
    LINKG_WEB_CMD_UPGRADE_STATUS_GET    = 1022  // 获取升级安装状态
} linkg_web_cmd_t;

/****************************** 类型定义 ******************************/

typedef int (*linkg_web_handler_t)(const cJSON *param, char **response);

/****************************** 服务接口 ******************************/

int  _linkg_web_server_init(void);
int  _linkg_web_server_start(void);
int  _linkg_web_server_stop(void);
int  _linkg_web_server_deinit(void);
int  _linkg_web_network_basic_init(void);
const char *_linkg_web_network_basic_instance(void);
void _linkg_web_network_response_complete(bool sent);
int  _linkg_web_device_init(void);
void _linkg_web_device_response_complete(bool sent);

/****************************** 请求分发 ******************************/

int  _linkg_web_dispatch_request(const char *request, size_t request_length, char **response, size_t *response_length);

/****************************** 响应构造 ******************************/

int  _linkg_web_response_success(int cmd, cJSON *data, const char *message, char **response);
int  _linkg_web_response_error(int cmd, const char *message, char **response);

/****************************** 请求处理 ******************************/

int  _linkg_web_handler_header_get(const cJSON *param, char **response);
int  _linkg_web_handler_overview_get(const cJSON *param, char **response);
int  _linkg_web_handler_switch_status_get(const cJSON *param, char **response);

int  _linkg_web_handler_wifi_config_get(const cJSON *param, char **response);
int  _linkg_web_handler_wifi_config_set(const cJSON *param, char **response);
int  _linkg_web_handler_wifi_status_get(const cJSON *param, char **response);

int  _linkg_web_handler_cellular_config_get(const cJSON *param, char **response);
int  _linkg_web_handler_cellular_config_set(const cJSON *param, char **response);
int  _linkg_web_handler_cellular_status_get(const cJSON *param, char **response);

int  _linkg_web_handler_uart_config_get(const cJSON *param, char **response);
int  _linkg_web_handler_uart_config_set(const cJSON *param, char **response);
int  _linkg_web_handler_uart_status_get(const cJSON *param, char **response);

int  _linkg_web_handler_traffic_config_get(const cJSON *param, char **response);
int  _linkg_web_handler_traffic_config_set(const cJSON *param, char **response);
int  _linkg_web_handler_network_basic_get(const cJSON *param, char **response);
int  _linkg_web_handler_network_basic_set(const cJSON *param, char **response);
int  _linkg_web_handler_network_basic_restart(const cJSON *param, char **response);

int _linkg_web_handler_device_info_get(const cJSON *param, char **response);
int _linkg_web_handler_device_role_set(const cJSON *param, char **response);
int _linkg_web_handler_device_restart(const cJSON *param, char **response);

int _linkg_web_handler_upgrade_start(const cJSON *param, char **response);
int _linkg_web_handler_upgrade_status_get(const cJSON *param, char **response);

#ifdef __cplusplus
}
#endif

#endif
