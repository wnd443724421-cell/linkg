/**
 * @file linkg_network_config.h
 * @brief LinkG网络配置定义及处理接口
 */

#ifndef LINKG_NETWORK_CONFIG_H
#define LINKG_NETWORK_CONFIG_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置常量 ******************************/

#define LINKG_NETWORK_TRAFFIC_RULE_MAX 16U // 最大业务流量分类规则数量

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_NETWORK_PORT_PROTOCOL_TCP = 0, // TCP协议
    LINKG_NETWORK_PORT_PROTOCOL_UDP      // UDP协议
} linkg_network_port_protocol_t;

typedef enum
{
    LINKG_NETWORK_TRAFFIC_CLASS_REALTIME = 0, // 实时业务流量
    LINKG_NETWORK_TRAFFIC_CLASS_VIDEO         // 视频业务流量
} linkg_network_traffic_class_t;

typedef struct
{
    linkg_network_traffic_class_t traffic_class; // 业务类型
    linkg_network_port_protocol_t protocol;      // 传输层协议
    uint16_t                      start_port;    // 起始端口
    uint16_t                      end_port;      // 结束端口
} linkg_network_traffic_rule_t;

typedef struct
{
    uint32_t                     count;                                 // 有效规则数量
    linkg_network_traffic_rule_t rules[LINKG_NETWORK_TRAFFIC_RULE_MAX]; // 业务流量分类规则
} linkg_network_traffic_config_t;

/**
 * @brief DHCP服务配置
 */
typedef struct
{
    bool enabled;         // 是否启用DHCP服务
    bool default_gateway; // 是否向客户端下发默认网关
} linkg_network_dhcp_config_t;

typedef struct
{
    struct in_addr ip;      // IPv4地址或网络地址，网络字节序
    struct in_addr netmask; // IPv4子网掩码，网络字节序
} linkg_network_ipv4_config_t;

typedef struct
{
    uint8_t                        node_id;          // LinkG节点编号
    struct in_addr                 virtual_network;  // 组网虚拟网络基地址，同一组网成员必须一致
    struct in_addr                 ethernet_network; // Ethernet网络基地址，本节点独立配置
    linkg_network_dhcp_config_t    dhcp;             // Ethernet DHCP服务配置
    linkg_network_traffic_config_t traffic;          // 用户业务流量分类配置
} linkg_network_config_t;

/****************************** 配置处理 ******************************/

void linkg_network_config_set_default(linkg_network_config_t *out);
int  linkg_network_config_parse(const cJSON *node, linkg_network_config_t *out);
int  linkg_network_config_validate(const linkg_network_config_t *config);
int  linkg_network_config_to_json(cJSON *parent, const char *key, const linkg_network_config_t *config);

/****************************** 地址查询 ******************************/

int linkg_network_config_get_ethernet(const linkg_network_config_t *config, linkg_network_ipv4_config_t *ethernet);
int linkg_network_config_get_ethernet_network(const linkg_network_config_t *config, linkg_network_ipv4_config_t *network);
int linkg_network_config_get_tun(const linkg_network_config_t *config, linkg_network_ipv4_config_t *tun);
int linkg_network_config_get_virtual_network(const linkg_network_config_t *config, linkg_network_ipv4_config_t *network);
int linkg_network_config_get_node_virtual_subnet(const linkg_network_config_t *config, uint8_t node_id, linkg_network_ipv4_config_t *subnet);
int linkg_network_config_get_node_address(const linkg_network_config_t *config, uint8_t node_id, struct in_addr *address);

#ifdef __cplusplus
}
#endif

#endif
