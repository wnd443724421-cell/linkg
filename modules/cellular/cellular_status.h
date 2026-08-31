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

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 刷新标志 ******************************/

typedef uint32_t cellular_status_refresh_mask_t; // 状态定向刷新标志集合

#define CELLULAR_STATUS_REFRESH_NONE              0U                // 不强制刷新任何状态
#define CELLULAR_STATUS_REFRESH_NETWORK_MODE     (1U << 0)          // 刷新网络选择模式
#define CELLULAR_STATUS_REFRESH_SIM              (1U << 1)          // 刷新SIM逻辑状态
#define CELLULAR_STATUS_REFRESH_REGISTRATION     (1U << 2)          // 刷新网络注册状态
#define CELLULAR_STATUS_REFRESH_RADIO            (1U << 3)          // 刷新服务小区和无线质量
#define CELLULAR_STATUS_REFRESH_PDP              (1U << 4)          // 刷新PDP激活状态
#define CELLULAR_STATUS_REFRESH_PDP_ADDRESS      (1U << 5)          // 刷新模组PDP地址
#define CELLULAR_STATUS_REFRESH_NETDEV           (1U << 6)          // 刷新USB网络设备状态
#define CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK (1U << 7)          // 刷新模组期望Host网络参数
#define CELLULAR_STATUS_REFRESH_HOST             (1U << 8)          // 刷新Linux Host实际网络状态
#define CELLULAR_STATUS_REFRESH_ALL             ((1U << 9) - 1U)    // 刷新全部状态事实

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
    cellular_status_meta_t        network_mode_meta; // 网络选择模式采集元数据
    linkg_cellular_network_mode_t network_mode;      // 当前网络选择模式
    cellular_status_meta_t        sim_meta;          // SIM逻辑状态采集元数据
    linkg_cellular_sim_state_t    sim_state;         // 当前SIM逻辑状态
} cellular_status_local_info_t;

/****************************** 移动网络状态 ******************************/

typedef struct
{
    cellular_status_meta_t              registration_meta; // 网络注册状态采集元数据
    linkg_cellular_registration_state_t registration;      // 当前网络注册状态
    cellular_status_meta_t              radio_meta;        // 服务小区无线状态采集元数据
    bool                                serving_cell_valid;// 当前是否存在可识别服务小区
    linkg_cellular_network_type_t       network_type;      // 当前实际接入网络类型
    uint16_t                            band;              // 当前工作频段
    int32_t                             rsrp_dbm;          // 当前RSRP，信号接收功率，单位dBm
    bool                                rsrp_valid;        // 当前RSRP是否有效
    int32_t                             rsrq_db;           // 当前RSRQ，信号接收质量，单位dB
    bool                                rsrq_valid;        // 当前RSRQ是否有效
    int32_t                             sinr_db;           // 当前SINR，单位dB
    bool                                sinr_valid;        // 当前SINR是否有效
} cellular_status_network_info_t;

/****************************** PDP状态 ******************************/

typedef struct
{
    cellular_status_meta_t active_meta;       // PDP激活状态采集元数据
    bool                   active;            // 默认PDP上下文是否激活
    cellular_status_meta_t address_meta;      // 模组PDP地址采集元数据
    bool                   ipv4_valid;        // 模组PDP IPv4地址是否有效
    struct in_addr         ipv4;              // 模组当前PDP IPv4地址
    bool                   global_ipv6_valid; // 模组PDP全局IPv6地址是否有效
    struct in6_addr        global_ipv6;       // 模组当前PDP全局IPv6地址
} cellular_status_pdp_info_t;

/****************************** USB网络设备状态 ******************************/

typedef enum
{
    CELLULAR_STATUS_NETDEV_MODE_UNKNOWN      = 0,  // USB网络设备工作模式未知
    CELLULAR_STATUS_NETDEV_MODE_DISCONNECTED,      // USB网络设备未连接
    CELLULAR_STATUS_NETDEV_MODE_ONCE,              // USB网络设备单次连接
    CELLULAR_STATUS_NETDEV_MODE_AUTO               // USB网络设备自动连接
} cellular_status_netdev_mode_t;

typedef struct
{
    cellular_status_meta_t        meta;        // USB网络设备状态采集元数据
    cellular_status_netdev_mode_t mode;        // 当前USB网络设备连接模式
    uint8_t                       cid;         // 当前使用的PDP上下文ID
    bool                          urc_enabled; // QNETDEV状态URC是否开启
    bool                          connected;   // 当前USB网络设备是否连接成功
} cellular_status_netdev_state_info_t;

typedef struct
{
    cellular_status_meta_t meta;    // Host期望IPv4参数采集元数据
    bool                   valid;   // 当前期望IPv4参数是否存在
    struct in_addr         address; // RG255提供给Host的IPv4地址
    struct in_addr         netmask; // RG255提供给Host的IPv4子网掩码
    struct in_addr         gateway; // RG255提供给Host的IPv4网关
} cellular_status_expected_ipv4_info_t;

typedef struct
{
    cellular_status_meta_t meta;          // Host期望IPv6参数采集元数据
    bool                   valid;         // 当前期望IPv6参数是否存在
    struct in6_addr        prefix;        // RG255提供给Host的IPv6网络前缀
    uint8_t                prefix_length; // IPv6网络前缀长度
    struct in6_addr        gateway;       // RG255提供给Host的IPv6网关
} cellular_status_expected_ipv6_info_t;

typedef struct
{
    cellular_status_netdev_state_info_t   state;         // USB网络设备当前状态
    cellular_status_expected_ipv4_info_t  expected_ipv4; // RG255期望Host IPv4参数
    cellular_status_expected_ipv6_info_t  expected_ipv6; // RG255期望Host IPv6参数
} cellular_status_netdev_info_t;

/****************************** Host网络状态 ******************************/

typedef struct
{
    cellular_status_meta_t interface_meta;          // 蜂窝接口存在状态采集元数据
    bool                   interface_present;       // 蜂窝网络接口是否存在
    unsigned int           interface_index;         // 蜂窝网络接口索引
    cellular_status_meta_t link_meta;               // 蜂窝接口UP状态采集元数据
    bool                   link_up;                 // 蜂窝网络接口是否处于UP状态
    cellular_status_meta_t ipv4_meta;               // Host IPv4地址采集元数据
    bool                   ipv4_valid;              // Host IPv4地址是否有效
    struct in_addr         ipv4;                    // Host当前IPv4地址
    cellular_status_meta_t ipv4_netmask_meta;       // Host IPv4子网掩码采集元数据
    bool                   ipv4_netmask_valid;      // Host IPv4子网掩码是否有效
    struct in_addr         ipv4_netmask;            // Host当前IPv4子网掩码
    cellular_status_meta_t ipv6_meta;               // Host全局IPv6地址采集元数据
    bool                   global_ipv6_valid;       // Host全局IPv6地址是否有效
    struct in6_addr        global_ipv6;             // Host当前全局IPv6地址
    cellular_status_meta_t ipv4_route_meta;         // Host IPv4默认路由采集元数据
    bool                   ipv4_gateway_valid;      // Host IPv4默认网关是否有效
    struct in_addr         ipv4_gateway;            // Host当前IPv4默认网关
    cellular_status_meta_t ipv6_route_meta;         // Host IPv6默认路由采集元数据
    bool                   ipv6_gateway_valid;      // Host IPv6默认网关是否有效
    struct in6_addr        ipv6_gateway;            // Host当前IPv6默认网关
} cellular_status_host_info_t;

/****************************** 内部状态快照 ******************************/

typedef struct
{
    bool                           valid;            // 是否至少发布过一次状态事实快照
    bool                           partial;          // 当前公开事实是否存在未可靠确认项
    uint64_t                       generation;       // 状态快照发布代数
    uint64_t                       published_ms;     // 最近一次状态快照发布时间
    uint64_t                       last_attempt_ms;  // 最近一次状态处理时间
    uint64_t                       last_complete_ms; // 最近一次公开事实完整确认时间
    int                            last_error;       // 当前公开事实中的首个采集错误
    cellular_status_local_info_t   local;            // 本机蜂窝状态事实
    cellular_status_network_info_t network;          // 移动网络状态事实
    cellular_status_pdp_info_t     pdp;              // PDP数据会话事实
    cellular_status_netdev_info_t  netdev;           // 模组USB网络设备事实
    cellular_status_host_info_t    host;             // Linux Host实际网络事实
} cellular_status_info_t;

/****************************** 生命周期 ******************************/

int cellular_status_init(void);
int cellular_status_start(at_channel_t *channel);
int cellular_status_stop(void);
int cellular_status_deinit(void);

/****************************** 定时处理 ******************************/

uint64_t cellular_status_get_deadline(void);
int      cellular_status_process(uint64_t now_ms, cellular_status_refresh_mask_t requested);

/****************************** 状态读取 ******************************/

int cellular_status_get_info(cellular_status_info_t *info);
int cellular_status_get_snapshot(linkg_cellular_status_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
