/**
 * @file linkg_cellular_status.h
 * @brief LinkG蜂窝网络运行状态定义
 */

#ifndef LINKG_CELLULAR_STATUS_H
#define LINKG_CELLULAR_STATUS_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_cellular_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 状态常量 ******************************/

#define LINKG_CELLULAR_MCC_LENGTH      3U // MCC固定长度
#define LINKG_CELLULAR_MNC_MAX_LENGTH  3U // MNC最大长度

/****************************** SIM状态 ******************************/

typedef enum
{
    LINKG_CELLULAR_SIM_STATE_UNKNOWN = 0, // SIM状态未知
    LINKG_CELLULAR_SIM_STATE_NOT_READY,   // SIM存在但当前尚未就绪
    LINKG_CELLULAR_SIM_STATE_ABSENT,      // 未检测到SIM卡
    LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED,// SIM需要PIN解锁
    LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED,// SIM需要PUK解锁
    LINKG_CELLULAR_SIM_STATE_READY        // SIM已就绪
} linkg_cellular_sim_state_t;

/****************************** 网络注册状态 ******************************/

typedef enum
{
    LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN = 0,       // 网络注册状态未知
    LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED,    // 当前未注册网络
    LINKG_CELLULAR_REGISTRATION_STATE_SEARCHING,         // 正在搜索可用网络
    LINKG_CELLULAR_REGISTRATION_STATE_DENIED,            // 网络注册被拒绝
    LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED_HOME,   // 已注册本地网络
    LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED_ROAMING,// 已注册漫游网络
    LINKG_CELLULAR_REGISTRATION_STATE_EMERGENCY_ONLY     // 当前仅允许紧急业务
} linkg_cellular_registration_state_t;

/****************************** 无线接入制式 ******************************/

typedef enum
{
    LINKG_CELLULAR_RAT_UNKNOWN = 0, // 当前无线接入制式未知
    LINKG_CELLULAR_RAT_LTE,         // LTE
    LINKG_CELLULAR_RAT_NR5G_NSA,    // 5G NR NSA
    LINKG_CELLULAR_RAT_NR5G_SA      // 5G NR SA
} linkg_cellular_rat_t;

/****************************** 服务小区状态 ******************************/

typedef struct
{
    bool                 valid;                                      // 当前服务小区信息是否有效
    linkg_cellular_rat_t rat;                                        // 当前无线接入制式

    bool                 plmn_valid;                                 // 当前PLMN信息是否有效
    char                 mcc[LINKG_CELLULAR_MCC_LENGTH + 1U];        // 当前移动国家码
    char                 mnc[LINKG_CELLULAR_MNC_MAX_LENGTH + 1U];    // 当前移动网络码

    uint64_t             cell_id;                                    // 当前服务小区ID
    uint32_t             tac;                                        // 当前跟踪区码
    uint16_t             pci;                                        // 当前物理小区ID

    uint16_t             band;                                       // 当前工作频段
    uint32_t             arfcn;                                      // 当前绝对射频信道号
    uint32_t             bandwidth_khz;                              // 当前无线带宽，单位kHz
    bool                 bandwidth_valid;                            // 当前无线带宽是否有效

    int32_t              rssi_dbm;                                   // 当前RSSI，单位dBm
    bool                 rssi_valid;                                 // 当前RSSI是否有效
    int32_t              rsrp_dbm;                                   // 当前RSRP，单位dBm
    bool                 rsrp_valid;                                 // 当前RSRP是否有效
    int32_t              rsrq_db;                                    // 当前RSRQ，单位dB
    bool                 rsrq_valid;                                 // 当前RSRQ是否有效
    int32_t              sinr_db;                                    // 当前SINR，单位dB
    bool                 sinr_valid;                                 // 当前SINR是否有效

    uint64_t             updated_ms;                                 // 服务小区状态更新时间
} linkg_cellular_serving_cell_status_t;

/****************************** 本机状态 ******************************/

typedef struct
{
    linkg_cellular_network_mode_t network_mode;            // 当前网络选择模式
    linkg_cellular_sim_state_t    sim_state;               // 当前SIM状态
    uint64_t                      network_mode_updated_ms; // 网络选择模式更新时间
    uint64_t                      sim_updated_ms;          // SIM状态更新时间
} linkg_cellular_local_status_t;

/****************************** 移动网络状态 ******************************/

typedef struct
{
    linkg_cellular_registration_state_t registration;            // 当前网络注册状态
    uint64_t                            registration_updated_ms; // 网络注册状态更新时间
    linkg_cellular_serving_cell_status_t serving_cell;           // 当前服务小区状态
} linkg_cellular_network_status_t;

/****************************** 数据状态 ******************************/

typedef struct
{
    bool            pdp_valid;          // PDP状态是否有效
    bool            pdp_active;         // PDP上下文当前是否激活
    uint64_t        pdp_updated_ms;     // PDP状态更新时间

    bool            ipv4_valid;         // 当前IPv4地址是否有效
    struct in_addr  ipv4;               // 当前主机蜂窝接口IPv4地址

    bool            global_ipv6_valid;  // 当前全局IPv6地址是否有效
    struct in6_addr global_ipv6;        // 当前主机蜂窝接口全局IPv6地址

    uint64_t        address_updated_ms; // 主机地址状态更新时间
} linkg_cellular_data_status_t;

/****************************** 状态快照 ******************************/

typedef struct
{
    bool                            partial; // 当前快照是否存在暂不可用的状态信息
    linkg_cellular_local_status_t   local;   // 本机蜂窝状态
    linkg_cellular_network_status_t network; // 当前移动网络状态
    linkg_cellular_data_status_t    data;    // 当前数据承载状态
} linkg_cellular_status_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif
