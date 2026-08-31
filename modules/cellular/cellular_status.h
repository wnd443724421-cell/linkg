/**
 * @file cellular_status.h
 * @brief LinkG蜂窝网络内部状态管理接口
 */

#ifndef CELLULAR_STATUS_H
#define CELLULAR_STATUS_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_cellular_status.h"

#include "at_channel.h"
#include "rg255_query.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 状态元数据 ******************************/

typedef struct
{
    bool     confirmed;    // 是否至少成功确认过当前事实
    uint64_t attempted_ms; // 最近一次尝试采集时间
    uint64_t updated_ms;   // 最近一次成功确认事实的时间
    int      last_error;   // 最近一次采集结果，0表示成功
} cellular_status_meta_t;

/****************************** 本机状态 ******************************/

typedef struct
{
    cellular_status_meta_t          network_mode_meta; // 网络模式状态元数据
    linkg_cellular_network_mode_t   network_mode;      // 当前网络搜索模式
    cellular_status_meta_t          sim_meta;          // SIM状态元数据
    linkg_cellular_sim_state_t      sim_state;         // 当前SIM状态
} cellular_status_local_info_t;

/****************************** 移动网络状态 ******************************/

typedef struct
{
    cellular_status_meta_t                  registration_meta; // 注册状态元数据
    linkg_cellular_registration_state_t     registration;      // 当前网络注册状态
    cellular_status_meta_t                  serving_cell_meta; // 服务小区状态元数据
    rg255_serving_cell_info_t               serving_cell;      // 当前服务小区无线状态
} cellular_status_network_info_t;

/****************************** 模组数据状态 ******************************/

typedef struct
{
    cellular_status_meta_t                  pdp_meta;              // PDP激活状态元数据
    bool                                    pdp_active;            // 默认PDP上下文是否激活
    cellular_status_meta_t                  pdp_address_meta;      // PDP地址状态元数据
    rg255_pdp_address_t                     pdp_address;           // 模组当前PDP地址
    cellular_status_meta_t                  netdev_meta;           // USB网卡连接状态元数据
    rg255_netdev_status_t                   netdev;                // 当前USB网卡连接状态
    cellular_status_meta_t                  expected_ipv4_meta;    // Host期望IPv4参数元数据
    rg255_network_card_ipv4_info_t          expected_ipv4;         // RG255提供给Host的IPv4网络参数
    cellular_status_meta_t                  expected_ipv6_meta;    // Host期望IPv6参数元数据
    rg255_network_card_ipv6_info_t          expected_ipv6;         // RG255提供给Host的IPv6网络参数
} cellular_status_modem_data_info_t;

/****************************** Host网络状态 ******************************/

typedef struct
{
    cellular_status_meta_t meta;                                      // Host网络状态元数据
    bool                   interface_present;                         // 蜂窝网络接口是否存在
    bool                   link_up;                                   // 蜂窝网络接口是否处于UP状态
    bool                   ipv4_valid;                                // Host IPv4地址是否有效
    struct in_addr         ipv4;                                      // Host当前IPv4地址
    bool                   ipv4_netmask_valid;                        // Host IPv4子网掩码是否有效
    struct in_addr         ipv4_netmask;                              // Host当前IPv4子网掩码
    bool                   ipv4_gateway_valid;                        // Host IPv4默认网关是否有效
    struct in_addr         ipv4_gateway;                              // Host当前IPv4默认网关
    bool                   global_ipv6_valid;                         // Host全局IPv6地址是否有效
    struct in6_addr        global_ipv6;                               // Host当前全局IPv6地址
    bool                   ipv6_gateway_valid;                        // Host IPv6默认网关是否有效
    struct in6_addr        ipv6_gateway;                              // Host当前IPv6默认网关
} cellular_status_host_network_info_t;

/****************************** 内部状态快照 ******************************/

typedef struct
{
    bool                                valid;           // 是否至少完成过一次状态采集
    bool                                partial;         // 最近一次采集是否存在暂不可用的状态信息
    uint64_t                            generation;      // 状态快照更新代数
    uint64_t                            last_attempt_ms; // 最近一次状态采集时间
    uint64_t                            last_success_ms; // 最近一次完整采集成功时间
    int                                 last_error;      // 最近一次状态采集主要错误
    cellular_status_local_info_t        local;           // 本机蜂窝状态
    cellular_status_network_info_t      network;         // 移动网络状态
    cellular_status_modem_data_info_t   modem_data;      // 模组数据状态
    cellular_status_host_network_info_t host;            // Linux Host网络状态
} cellular_status_info_t;

/****************************** 生命周期 ******************************/

int  cellular_status_init(void);
int  cellular_status_start(at_channel_t *channel);
int  cellular_status_stop(void);
void cellular_status_deinit(void);

/****************************** 状态读取 ******************************/

int cellular_status_get_info(cellular_status_info_t *info);
int cellular_status_get_snapshot(linkg_cellular_status_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
