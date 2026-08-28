/**
 * @file rg255_query.h
 * @brief RG255查询结果解析接口
 */

#ifndef RG255_QUERY_H
#define RG255_QUERY_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_cellular_config.h"
#include "linkg_cellular_status.h"

#include "at_channel.h"

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

/****************************** PDP配置 ******************************/

typedef struct
{
    uint8_t          cid;                               // PDP上下文ID
    rg255_pdp_type_t pdp_type;                          // PDP协议类型
    char             apn[LINKG_CELLULAR_APN_MAX + 1U]; // 当前APN
} rg255_pdp_config_t;

/****************************** PDP地址 ******************************/

typedef struct
{
    bool            ipv4_valid; // 模组PDP IPv4地址是否有效
    struct in_addr  ipv4;       // 模组当前PDP IPv4地址
    bool            ipv6_valid; // 模组PDP IPv6地址是否有效
    struct in6_addr ipv6;       // 模组当前PDP IPv6地址
} rg255_pdp_address_t;

/****************************** SIM查询 ******************************/

int rg255_query_sim_state(at_channel_t *channel, linkg_cellular_sim_state_t *state);

/****************************** 网络查询 ******************************/

int rg255_query_network_mode(at_channel_t *channel, linkg_cellular_network_mode_t *mode);
int rg255_query_registration(at_channel_t *channel, linkg_cellular_network_mode_t mode, linkg_cellular_network_type_t network_type, linkg_cellular_registration_state_t *state);
int rg255_query_serving_cell(at_channel_t *channel, rg255_serving_cell_info_t *info);

/****************************** USB配置查询 ******************************/

int rg255_query_usbnet_mode(at_channel_t *channel, int *mode);
int rg255_query_nat_enabled(at_channel_t *channel, bool *enabled);

/****************************** PDP查询 ******************************/

int rg255_query_pdp_config(at_channel_t *channel, rg255_pdp_config_t *config);
int rg255_query_pdp_active(at_channel_t *channel, bool *active);
int rg255_query_pdp_address(at_channel_t *channel, rg255_pdp_address_t *address);

/****************************** 网络设备查询 ******************************/

int rg255_query_netdev_active(at_channel_t *channel, bool *active);

#ifdef __cplusplus
}
#endif

#endif
