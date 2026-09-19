/**
 * @file linkg_switch_status.h
 * @brief LinkG链路切换运行状态公共接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-19
 */

#ifndef LINKG_SWITCH_STATUS_H
#define LINKG_SWITCH_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_switch.h"
#include "linkg_system_resources.h"
#include "linkg_transport_types.h"
#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 接入状态 ******************************/

typedef enum
{
    LINKG_SWITCH_STATUS_ACCESS_NONE = 0, // 当前没有有效接入
    LINKG_SWITCH_STATUS_ACCESS_WIFI,     // Wi-Fi接入
    LINKG_SWITCH_STATUS_ACCESS_CELLULAR, // Cellular接入
    LINKG_SWITCH_STATUS_ACCESS_COUNT     // 接入状态数量
} linkg_switch_status_access_t;

/****************************** 通用状态 ******************************/

typedef struct
{
    bool     valid;          // 当前Probe结果是否有效
    bool     reachable;      // 最近一次Probe是否可达
    uint32_t rtt_us;         // 最近一次有效往返时延，单位微秒
    uint64_t updated_us;     // 最近一次Probe状态更新时间
} linkg_switch_status_probe_t;

typedef struct
{
    bool     valid;          // 当前流量统计是否有效
    uint32_t tx_pps;         // 当前发送包速率，单位packet/s
    uint32_t rx_pps;         // 当前接收包速率，单位packet/s
    uint64_t tx_bps;         // 当前发送比特率，单位bit/s
    uint64_t rx_bps;         // 当前接收比特率，单位bit/s
    uint64_t updated_us;     // 最近一次流量统计更新时间
} linkg_switch_status_traffic_t;

typedef struct
{
    bool     valid;          // 当前丢包统计窗口是否具有有效样本
    uint32_t loss_permille;  // 当前窗口丢包率，单位千分比
    uint32_t sample_packets; // 当前窗口参与统计的数据包数量
    uint64_t updated_us;     // 最近一次丢包状态更新时间
} linkg_switch_status_loss_t;

typedef struct
{
    bool                         active;     // 当前是否存在Maintenance接入屏蔽
    linkg_switch_status_access_t access;     // 当前被维护并禁止选择的接入
    uint32_t                     message_id; // 当前Maintenance事务编号
    uint64_t                     started_us; // 当前Maintenance开始时间
} linkg_switch_maintenance_status_t;

/****************************** STA Wi-Fi状态 ******************************/

typedef struct
{
    bool                          available;             // 当前Peer Wi-Fi Path是否存在且可用
    bool                          radio_valid;           // 当前Wi-Fi基础无线状态是否有效
    bool                          connected;             // Wi-Fi STA当前是否保持关联
    bool                          statistics_valid;      // Wi-Fi底层统计数据是否有效
    int32_t                       rssi_dbm;              // 当前接收信号强度，单位dBm
    bool                          noise_valid;           // 当前噪声数据是否有效
    int32_t                       noise_dbm;             // 当前噪声，单位dBm
    uint32_t                      tx_phy_kbps;           // 当前Wi-Fi发送PHY速率，单位Kbit/s
    uint32_t                      rx_phy_kbps;           // 当前Wi-Fi接收PHY速率，单位Kbit/s
    uint32_t                      inactive_ms;           // 当前Peer最近一次无线活动距今时间，单位毫秒
    linkg_wifi_work_mode_t        work_mode;             // 当前宽带或窄带工作模式
    uint16_t                      bandwidth_mhz;         // 当前实际工作带宽，单位MHz
    linkg_wifi_narrow_mode_t      narrow_mode;           // 当前窄带速率控制模式
    bool                          rate_level_valid;      // 当前窄带实际速率档位是否有效
    uint16_t                      rate_level;            // 当前窄带实际速率档位
    bool                          temperature_valid;     // 当前Wi-Fi温度是否有效
    int32_t                       temperature_c;         // 当前Wi-Fi温度，单位摄氏度
    uint64_t                      status_updated_us;     // 最近一次Wi-Fi基础状态更新时间
    uint64_t                      statistics_updated_us; // 最近一次Wi-Fi统计状态更新时间
    linkg_switch_status_probe_t   probe[LINKG_TRANSPORT_CLASS_COUNT]; // 各业务Class最近一次Probe状态
    linkg_switch_status_traffic_t traffic;               // 当前Peer Wi-Fi业务流量状态
    linkg_switch_status_loss_t    uplink_loss;           // STA到AP Wi-Fi上行真实丢包
    linkg_switch_status_loss_t    downlink_loss;         // AP到STA Wi-Fi下行真实丢包
} linkg_switch_sta_wifi_status_t;

/****************************** STA Cellular状态 ******************************/

typedef struct
{
    bool     available;  // 当前Peer Cellular Path存在且公网状态可用
    uint64_t updated_us; // 最近一次Cellular备用状态更新时间
} linkg_switch_sta_cellular_status_t;

/****************************** STA状态 ******************************/

typedef struct
{
    bool                               peer_present;           // 当前是否存在已注册的直接AP Peer
    bool                               observation_valid;      // 当前是否已经形成有效完整观测快照
    uint8_t                            peer_node_id;           // 当前直接AP节点编号
    linkg_send_plan_t                  plan;                   // 当前STA到AP发送计划
    linkg_switch_sta_wifi_status_t     wifi;                   // 当前Wi-Fi完整观测状态
    linkg_switch_sta_cellular_status_t cellular;               // 当前Cellular备用状态
    linkg_switch_maintenance_status_t  local_maintenance;      // 当前STA本机Maintenance状态
    linkg_switch_maintenance_status_t  remote_maintenance;     // 当前AP通知的远端Maintenance状态
    uint64_t                           observation_updated_us; // 最近一次完整观测快照更新时间
} linkg_switch_sta_status_t;

/****************************** AP Peer状态 ******************************/

typedef struct
{
    uint8_t                           peer_node_id;            // 当前直接STA节点编号
    linkg_send_plan_t                 plan;                    // 当前AP到该STA发送计划
    bool                              wifi_path_available;     // 当前Wi-Fi Path是否存在且可用
    bool                              cellular_path_available; // 当前Cellular Path是否存在且可用
    linkg_switch_status_loss_t        wifi_uplink_loss;        // 当前STA到AP Wi-Fi上行真实丢包
    linkg_switch_maintenance_status_t remote_maintenance;      // 当前STA上报的远端Maintenance状态
    uint64_t                          path_updated_us;         // 最近一次Path状态更新时间，0表示尚未形成Path状态
} linkg_switch_ap_peer_status_t;

/****************************** AP状态 ******************************/

typedef struct
{
    linkg_switch_maintenance_status_t local_maintenance;                     // 当前AP本机Maintenance状态
    uint32_t                          peer_count;                            // 当前有效直接STA数量
    linkg_switch_ap_peer_status_t     peers[LINKG_RESOURCE_NETWORK_STA_MAX]; // 当前全部直接STA状态
} linkg_switch_ap_status_t;

/****************************** Switch状态 ******************************/

typedef struct
{
    linkg_device_role_t role;         // 当前本机设备角色
    uint64_t            collected_us; // 本次对外状态快照形成时间

    union
    {
        linkg_switch_sta_status_t sta; // STA角色运行状态
        linkg_switch_ap_status_t  ap;  // AP角色运行状态
    } data;
} linkg_switch_status_t;

/****************************** 状态查询 ******************************/

int linkg_switch_get_status(linkg_switch_status_t *status);

#ifdef __cplusplus
}
#endif

#endif
