/**
 * @file linkg_wifi_status.h
 * @brief LinkG Wi-Fi运行状态定义
 */

#ifndef LINKG_WIFI_STATUS_H
#define LINKG_WIFI_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_wifi_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 状态常量 ******************************/

#define LINKG_WIFI_MAC_LENGTH   6U  // Wi-Fi MAC地址长度
#define LINKG_WIFI_AP_PEER_MAX 16U  // AP最大对端数量

/****************************** 接口状态 ******************************/

typedef enum
{
    LINKG_WIFI_INTERFACE_STATE_DOWN = 0, // 无线接口未运行
    LINKG_WIFI_INTERFACE_STATE_READY,    // 无线接口已运行但无关联对端
    LINKG_WIFI_INTERFACE_STATE_CONNECTED // 无线接口存在关联对端
} linkg_wifi_interface_state_t;

/****************************** 对端连接状态 ******************************/

typedef enum
{
    LINKG_WIFI_PEER_STATE_DISCONNECTED = 0, // 当前未连接
    LINKG_WIFI_PEER_STATE_CONNECTED         // 当前已连接
} linkg_wifi_peer_state_t;

/****************************** 无线参数 ******************************/

typedef struct
{
    linkg_wifi_narrow_mode_t mode;                  // 窄带速率控制模式
    uint16_t                 configured_rate_level; // 固定模式配置的速率档位
    uint16_t                 current_rate_level;    // 当前实际使用的窄带速率档位
    bool                     current_rate_valid;    // 当前实际速率档位是否有效
} linkg_wifi_narrow_status_t;

typedef struct
{
    linkg_wifi_wide_bandwidth_t bandwidth; // 当前宽带带宽
} linkg_wifi_wide_status_t;

typedef struct
{
    linkg_wifi_work_mode_t work_mode;     // 当前宽窄带工作模式
    uint32_t               frequency_mhz; // 当前工作频率，单位MHz
    uint16_t               channel;       // 当前工作信道
    int32_t                noise_dbm;     // 当前信道噪声强度，单位dBm
    bool                   noise_valid;   // 当前噪声强度是否有效

    union
    {
        linkg_wifi_narrow_status_t narrow; // 窄带运行参数
        linkg_wifi_wide_status_t   wide;   // 宽带运行参数
    } params;
} linkg_wifi_radio_status_t;

/****************************** 本机状态 ******************************/

typedef struct
{
    linkg_device_role_t          role;                       // 本机设备角色
    linkg_wifi_interface_state_t interface_state;            // 当前无线接口状态
    uint8_t                      mac[LINKG_WIFI_MAC_LENGTH]; // 本机MAC地址
    linkg_wifi_radio_status_t    radio;                      // 当前无线参数
    int32_t                      chip_temperature_c;         // Wi-Fi芯片温度，单位摄氏度
    bool                         chip_temperature_valid;     // Wi-Fi芯片温度是否有效
    uint64_t                     updated_ms;                 // 状态更新时间
} linkg_wifi_local_status_t;

/****************************** 对端状态 ******************************/

typedef struct
{
    bool                    valid;                      // 状态槽位是否有效
    bool                    statistics_valid;           // 当前详细统计是否有效
    linkg_wifi_peer_state_t state;                      // 当前连接状态
    uint8_t                 mac[LINKG_WIFI_MAC_LENGTH]; // 对端MAC地址
    int32_t                 rssi_dbm;                   // 本机接收对端的信号强度
    uint32_t                tx_rate_kbps;               // 本机向对端发送的当前PHY速率
    uint32_t                rx_rate_kbps;               // 本机接收对端数据的当前PHY速率
    uint64_t                driver_tx_bytes;            // 驱动累计发送字节
    uint64_t                driver_rx_bytes;            // 驱动累计接收字节
    uint64_t                driver_tx_packets;          // 驱动累计发送包数
    uint64_t                driver_rx_packets;          // 驱动累计接收包数
    uint64_t                driver_tx_failed;           // 驱动累计发送失败次数
    uint32_t                connected_time_s;           // 当前连接持续时间
    uint32_t                inactive_ms;                // 距离最近接收活动的时间
    uint64_t                updated_ms;                 // 基础状态更新时间
    uint64_t                statistics_updated_ms;      // 详细统计更新时间
} linkg_wifi_peer_status_t;

/****************************** AP状态 ******************************/

typedef struct
{
    bool                     peers_truncated;               // 当前对端列表是否被截断
    uint8_t                  peer_count;                    // 已保存的有效对端数量
    uint8_t                  connected_count;               // 当前已连接对端数量
    linkg_wifi_peer_status_t peers[LINKG_WIFI_AP_PEER_MAX]; // 对端STA状态表
} linkg_wifi_ap_status_t;

/****************************** STA状态 ******************************/

typedef struct
{
    linkg_wifi_peer_status_t peer; // 当前或最近连接的AP状态
} linkg_wifi_sta_status_t;

/****************************** 状态快照 ******************************/

typedef struct
{
    bool                      partial; // 本次快照是否存在部分状态缺失
    linkg_wifi_local_status_t local;   // 本机Wi-Fi状态

    union
    {
        linkg_wifi_ap_status_t  ap;  // AP模式运行状态
        linkg_wifi_sta_status_t sta; // STA模式运行状态
    } role;
} linkg_wifi_status_snapshot_t;

#ifdef __cplusplus
}
#endif

#endif
