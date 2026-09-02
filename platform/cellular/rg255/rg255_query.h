/**
 * @file rg255_query.h
 * @brief RG255查询结果解析接口
 */

#ifndef RG255_QUERY_H
#define RG255_QUERY_H

#include <netinet/in.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_cellular_status.h"

#include "rg255_cmd.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** PDP类型 ******************************/

typedef enum
{
    RG255_PDP_TYPE_UNKNOWN = 0, // PDP类型未知
    RG255_PDP_TYPE_IPV4,        // IPv4
    RG255_PDP_TYPE_IPV6,        // IPv6
    RG255_PDP_TYPE_IPV4V6       // IPv4/IPv6双栈
} rg255_pdp_type_t;

/****************************** SIM插入状态 ******************************/

typedef enum
{
    RG255_SIM_INSERT_STATE_UNKNOWN  = -1, // SIM插入状态未知
    RG255_SIM_INSERT_STATE_REMOVED  = 0,  // SIM未插入
    RG255_SIM_INSERT_STATE_INSERTED = 1   // SIM已插入
} rg255_sim_insert_state_t;

/****************************** 服务小区信息 ******************************/

typedef struct
{
    linkg_cellular_network_type_t network_type; // 当前实际接入网络类型
    uint16_t                      band;         // 当前工作频段
    int32_t                       rsrp_dbm;     // 当前RSRP，单位dBm
    bool                          rsrp_valid;   // 当前RSRP是否有效
    int32_t                       rsrq_db;      // 当前RSRQ，单位dB
    bool                          rsrq_valid;   // 当前RSRQ是否有效
    int32_t                       sinr_db;      // 当前SINR，单位dB
    bool                          sinr_valid;   // 当前SINR是否有效
} rg255_serving_cell_info_t;

/****************************** USB网卡IPv4信息 ******************************/

typedef struct
{
    struct in_addr address; // RG255提供给Host的IPv4地址
    struct in_addr netmask; // RG255提供给Host的IPv4子网掩码
    struct in_addr gateway; // RG255提供给Host的IPv4网关
} rg255_network_card_ipv4_info_t;

/****************************** USB网卡IPv6信息 ******************************/

typedef struct
{
    struct in6_addr prefix;        // RG255提供给Host的IPv6网络前缀
    uint8_t         prefix_length; // IPv6前缀长度
    struct in6_addr gateway;       // RG255提供给Host的IPv6网关
} rg255_network_card_ipv6_info_t;

/****************************** PDP配置 ******************************/

typedef struct
{
    uint8_t          cid;                              // PDP上下文ID
    rg255_pdp_type_t pdp_type;                         // PDP协议类型
    char             apn[LINKG_CELLULAR_APN_MAX + 1U]; // 当前APN
} rg255_pdp_config_t;

/****************************** PDP地址 ******************************/

typedef struct
{
    bool            ipv4_valid;        // 模组PDP IPv4地址是否有效
    struct in_addr  ipv4;              // 模组当前PDP IPv4地址
    bool            global_ipv6_valid; // 模组PDP全局IPv6地址是否有效
    struct in6_addr global_ipv6;       // 模组当前PDP全局IPv6地址
} rg255_pdp_address_t;

/****************************** 网络设备状态 ******************************/

typedef struct
{
    rg255_netdev_type_t type;        // 当前USB网卡连接方式
    uint8_t             cid;         // 当前使用的PDP上下文ID
    bool                urc_enabled; // 是否开启QNETDEV状态URC
    bool                connected;   // 当前USB网卡是否连接成功
} rg255_netdev_status_t;

/****************************** SIM检测配置 ******************************/

typedef struct
{
    bool                     enabled;      // SIM插拔检测是否开启
    rg255_sim_insert_level_t insert_level; // SIM插入有效电平
} rg255_sim_detect_config_t;

/****************************** SIM状态URC ******************************/

typedef struct
{
    bool                     enabled; // SIM状态URC是否开启
    rg255_sim_insert_state_t state;   // 当前SIM插入检测状态
} rg255_sim_status_urc_t;

/****************************** SIM查询 ******************************/

int rg255_query_sim_state(at_channel_t *channel, linkg_cellular_sim_state_t *state);
int rg255_query_sim_detect(at_channel_t *channel, rg255_sim_detect_config_t *config);
int rg255_query_sim_status_urc(at_channel_t *channel, rg255_sim_status_urc_t *status);

/****************************** 网络查询 ******************************/

int rg255_query_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t *mode);
int rg255_query_registration(at_channel_t *channel, linkg_cellular_network_mode_t mode, linkg_cellular_registration_state_t *state);
int rg255_query_serving_cell(at_channel_t *channel, rg255_serving_cell_info_t *info);

/****************************** USB配置查询 ******************************/

int rg255_query_usbnet_mode(at_channel_t *channel, rg255_usbnet_mode_t *mode);
int rg255_query_network_card_mode(at_channel_t *channel, rg255_network_card_mode_t *mode);
int rg255_query_network_card_ipv4(at_channel_t *channel, rg255_network_card_ipv4_info_t *info);
int rg255_query_network_card_ipv6(at_channel_t *channel, rg255_network_card_ipv6_info_t *info);

/****************************** PDP查询 ******************************/

int rg255_query_pdp_configs(at_channel_t *channel, rg255_pdp_config_t *configs, size_t capacity, size_t *count);
int rg255_query_pdp_active(at_channel_t *channel, uint8_t cid, bool *active);
int rg255_query_pdp_address(at_channel_t *channel, uint8_t cid, rg255_pdp_address_t *address);

/****************************** 网络设备查询 ******************************/

int rg255_query_netdev_status(at_channel_t *channel, rg255_netdev_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
