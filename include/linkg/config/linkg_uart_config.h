/**
 * @file linkg_uart_config.h
 * @brief LinkG串口及网络路由配置定义与处理接口
 */

#ifndef LINKG_UART_CONFIG_H
#define LINKG_UART_CONFIG_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置常量 ******************************/

#define LINKG_UART_CONFIG_PORT_COUNT_MAX  3U  // 最大串口配置数量
#define LINKG_UART_CONFIG_ROUTE_COUNT_MAX 16U // 每个串口的最大路由数量

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_UART_PORT_TYPE_NORMAL = 0, // 普通串口
    LINKG_UART_PORT_TYPE_SBUS        // SBUS串口
} linkg_uart_port_type_t;

typedef enum
{
    LINKG_UART_ROUTE_PROTO_TCP = 0, // TCP协议
    LINKG_UART_ROUTE_PROTO_UDP      // UDP协议
} linkg_uart_route_proto_t;

typedef enum
{
    LINKG_UART_ROUTE_ENDPOINT_LOCAL = 0, // 本地监听或绑定
    LINKG_UART_ROUTE_ENDPOINT_REMOTE     // 远端连接或发送
} linkg_uart_route_endpoint_t;

typedef enum
{
    LINKG_UART_ROUTE_DIR_TX = 0, // 串口到网络
    LINKG_UART_ROUTE_DIR_RX,     // 网络到串口
    LINKG_UART_ROUTE_DIR_BOTH    // 双向转发
} linkg_uart_route_dir_t;

typedef struct
{
    linkg_uart_port_type_t type;            // 串口类型：normal或sbus
    uint32_t               baudrate;        // 波特率，单位bit/s
    uint8_t                data_bits;       // 数据位数量
    uint8_t                stop_bits;       // 停止位数量
    char                   parity;          // 校验方式：n、e、o
} linkg_uart_port_config_t;

typedef struct
{
    linkg_uart_route_proto_t    proto;    // 网络协议：tcp或udp
    linkg_uart_route_endpoint_t endpoint; // 端点角色：local或remote
    linkg_uart_route_dir_t      dir;      // 转发方向：tx、rx或rxtx
    struct in_addr              ip;       // 监听或远端IPv4地址，网络字节序
    uint16_t                    port;     // TCP或UDP端口
} linkg_uart_route_config_t;

typedef struct
{
    uint8_t                   uart_num;                                         // 逻辑串口编号，范围0～2
    linkg_uart_port_config_t  port_config;                                      // 串口物理参数
    uint32_t                  route_count;                                      // 有效路由数量
    linkg_uart_route_config_t routes[LINKG_UART_CONFIG_ROUTE_COUNT_MAX];        // 串口网络路由列表
} linkg_uart_item_config_t;

typedef struct
{
    uint32_t                 count;                                          // 有效串口数量
    linkg_uart_item_config_t items[LINKG_UART_CONFIG_PORT_COUNT_MAX];        // 串口配置列表
} linkg_uart_config_t;

/****************************** 配置处理 ******************************/

void linkg_uart_config_set_default(linkg_uart_config_t *out);
int  linkg_uart_config_parse(const cJSON *node, linkg_uart_config_t *out);
int  linkg_uart_config_validate(const linkg_uart_config_t *config);
int  linkg_uart_config_to_json(cJSON *parent, const char *key, const linkg_uart_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
