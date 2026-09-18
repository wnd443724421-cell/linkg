/**
 * @file switch_observation.h
 * @brief LinkG链路切换观测快照内部接口
 */

#ifndef SWITCH_OBSERVATION_H
#define SWITCH_OBSERVATION_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_switch.h"
#include "linkg_transport_types.h"
#include "linkg_wifi_config.h"

/****************************** Probe观测 ******************************/

typedef struct
{
    bool     valid;      // 最近一次周期Probe是否形成有效网络结果
    bool     reachable;  // 最近一次周期Probe是否可达
    uint32_t rtt_us;     // 最近一次成功Probe往返时延
    uint64_t updated_us; // 最近一次Probe结果更新时间
} linkg_switch_probe_observation_t;

/****************************** 流量观测 ******************************/

typedef struct
{
    bool     valid;      // 是否已经形成有效速率采样
    uint32_t tx_pps;     // 当前发送包速率
    uint32_t rx_pps;     // 当前接收包速率
    uint64_t tx_bps;     // 当前发送比特率
    uint64_t rx_bps;     // 当前接收比特率
    uint64_t updated_us; // 最近一次速率计算时间
} linkg_switch_traffic_observation_t;

/****************************** 丢包观测 ******************************/

typedef struct
{
    bool     valid;          // 当前丢包统计是否有效
    uint32_t loss_permille;  // 当前丢包率，0~1000
    uint32_t sample_packets; // 当前统计窗口参与计算的数据包数量
    uint64_t updated_us;     // 当前丢包统计更新时间
} linkg_switch_loss_observation_t;

typedef struct
{
    linkg_switch_loss_observation_t uplink;   // STA到AP上行丢包，由AP统计并上报
    linkg_switch_loss_observation_t downlink; // AP到STA下行丢包，由STA本机统计
} linkg_switch_wifi_loss_observation_t;

/****************************** Wi-Fi无线观测 ******************************/

typedef struct
{
    bool                     valid;                  // 当前Wi-Fi基础状态是否有效
    bool                     connected;              // 当前是否已经关联AP
    bool                     statistics_valid;       // 当前Peer无线统计是否有效
    int32_t                  rssi_dbm;               // 当前接收信号强度
    uint32_t                 tx_phy_kbps;            // 当前发送PHY速率
    uint32_t                 rx_phy_kbps;            // 当前接收PHY速率
    uint32_t                 inactive_ms;            // 当前对端最近无线活动间隔
    bool                     noise_valid;            // 当前信道噪声是否有效
    int32_t                  noise_dbm;              // 当前信道噪声
    linkg_wifi_work_mode_t   work_mode;              // 当前宽带或窄带工作模式
    uint16_t                 bandwidth_mhz;          // 当前实际工作带宽
    linkg_wifi_narrow_mode_t narrow_mode;            // 当前窄带速率控制模式
    bool                     rate_level_valid;       // 当前实际窄带速率档位是否有效
    uint16_t                 rate_level;             // 当前实际窄带速率档位
    bool                     temperature_valid;      // 当前Wi-Fi芯片温度是否有效
    int32_t                  temperature_c;          // 当前Wi-Fi芯片温度
    uint64_t                 status_updated_us;      // Wi-Fi基础状态更新时间
    uint64_t                 statistics_updated_us;  // Peer无线统计更新时间
} linkg_switch_wifi_radio_observation_t;

/****************************** Wi-Fi路径观测 ******************************/

typedef struct
{
    bool                                   available; // 当前Wi-Fi业务Path是否具备基本使用条件
    uint32_t                               link_id;   // 当前Wi-Fi Link实例
    linkg_switch_probe_observation_t       probe[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务类别周期Probe结果
    linkg_switch_traffic_observation_t     traffic;   // 当前Wi-Fi Path业务流量
    linkg_switch_wifi_loss_observation_t   loss;      // 当前Wi-Fi双向真实业务丢包
    linkg_switch_wifi_radio_observation_t  radio;     // 当前Wi-Fi无线层状态
} linkg_switch_wifi_observation_t;

/****************************** Cellular备用链路 ******************************/

typedef struct
{
    bool     available;  // 公网可用且当前Peer存在Cellular Path
    uint32_t link_id;    // 当前Cellular Link实例
    uint64_t updated_us; // 本次备用链路确认时间
} linkg_switch_cellular_observation_t;

/****************************** 切换观测快照 ******************************/

typedef struct
{
    bool                                 valid;        // 本轮是否形成有效观测快照
    uint8_t                              peer_node_id; // 当前直接Peer节点编号
    linkg_send_plan_t                    plan;         // 采集时冻结的当前发送计划
    linkg_switch_wifi_observation_t      wifi;         // 当前Wi-Fi质量观测
    linkg_switch_cellular_observation_t  cellular;     // 当前Cellular备用可用状态
    uint64_t                             collected_us; // 本轮观测采集完成时间
} linkg_switch_observation_t;

/****************************** 采样状态 ******************************/

/****************************** 采样状态 ******************************/

typedef struct
{
    bool     initialized; // 是否已经取得上一轮累计统计
    uint32_t link_id;     // 当前采样对应Wi-Fi Link实例
    uint64_t sampled_us;  // 上一轮累计统计采样时间
    uint64_t tx_bytes;    // 上一轮累计发送字节数
    uint64_t rx_bytes;    // 上一轮累计接收字节数
    uint64_t tx_packets;  // 上一轮累计发送包数量
    uint64_t rx_packets;  // 上一轮累计接收包数量
} linkg_switch_traffic_sampler_t;

typedef struct
{
    bool     initialized;      // 是否已经取得上一轮累计丢包统计
    uint32_t link_id;          // 当前采样对应Wi-Fi Link实例
    uint64_t received_packets; // 上一轮累计成功接收包数量
    uint64_t lost_packets;     // 上一轮累计确认丢包数量
} linkg_switch_loss_sampler_t;

/****************************** 观测操作 ******************************/

int  linkg_switch_observation_refresh(uint8_t peer_node_id, uint64_t now_us);
void linkg_switch_observation_reset_locked(uint8_t peer_node_id);

#endif
