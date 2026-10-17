
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

/**
 * @brief 指定Peer、方向、统计窗口的丢包观测。
 *
 * loss_permille使用千分比表示，0为无丢包，1000为全部丢包。
 * sample_packets为当前窗口参与计算的总帧数量。
 * 没有有效样本时valid必须为false，不能解释为0丢包。
 */
typedef struct
{
    bool     valid;          // 当前窗口是否存在有效丢包样本
    uint32_t loss_permille;  // 当前丢包率，0~1000
    uint32_t sample_packets; // 当前统计窗口参与计算的总帧数
    uint64_t updated_us;     // 当前丢包统计更新时间
} linkg_switch_loss_observation_t;

/**
 * @brief 指定Peer的双向Wire传输丢包及本地负载丢包观测。
 *
 * Wire由接收端根据链路独立Sequence统计，反映Socket成功提交
 * 之后到对端接收之间的报文损失，不等同于纯无线空口丢包。
 *
 * Load由发送端根据Path累计统计计算：
 * Wi-Fi和Cellular统一使用tx_queue_overflow_packets。
 *
 * 综合交付丢包率不在此处缓存，由Switch Policy根据Wire和
 * Load的有效样本计算。
 */
typedef struct
{
    linkg_switch_loss_observation_t uplink;        // STA到AP Wire丢包，由AP接收端统计
    linkg_switch_loss_observation_t uplink_load;   // STA到AP Load丢包，由STA发送端统计

    linkg_switch_loss_observation_t downlink;      // AP到STA Wire丢包，由STA接收端统计
    linkg_switch_loss_observation_t downlink_load; // AP到STA Load丢包，由AP发送端统计
} linkg_switch_path_loss_observation_t;

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
    uint16_t                 rate_level;             // 当前窄带速率档位
    bool                     temperature_valid;      // 当前Wi-Fi芯片温度是否有效
    int32_t                  temperature_c;          // 当前Wi-Fi芯片温度
    uint64_t                 status_updated_us;      // Wi-Fi基础状态更新时间
    uint64_t                 statistics_updated_us;  // Peer无线统计更新时间
} linkg_switch_wifi_radio_observation_t;

/****************************** Wi-Fi路径观测 ******************************/

typedef struct
{
    bool                                 available; // 当前Wi-Fi业务Path是否具备基本使用条件
    uint32_t                             link_id;   // 当前Wi-Fi Link实例
    linkg_switch_probe_observation_t     probe[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务类别周期Probe结果
    linkg_switch_traffic_observation_t   traffic;   // Wi-Fi业务流量，仅供运行监控和诊断
    linkg_switch_path_loss_observation_t loss;      // Wi-Fi双向Wire和Load丢包观测
    linkg_switch_wifi_radio_observation_t radio;    // Wi-Fi无线层状态
} linkg_switch_wifi_observation_t;

/****************************** Cellular路径观测 ******************************/

typedef struct
{
    bool                                 available;   // 公网可用且当前Peer存在Cellular Path
    uint32_t                             link_id;     // 当前Cellular Link实例
    linkg_switch_probe_observation_t     probe[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务类别周期Probe结果
    linkg_switch_path_loss_observation_t loss;        // Cellular双向Wire和Load丢包观测
    uint64_t                             updated_us;  // 本次Cellular观测更新时间
} linkg_switch_cellular_observation_t;

/****************************** 切换观测快照 ******************************/

typedef struct
{
    bool                                valid;        // 本轮是否形成有效观测快照
    uint8_t                             peer_node_id; // 当前直接Peer节点编号
    linkg_switch_wifi_observation_t     wifi;         // 当前Wi-Fi路径观测
    linkg_switch_cellular_observation_t cellular;     // 当前Cellular路径观测
    uint64_t                            collected_us; // 本轮观测采集完成时间
} linkg_switch_observation_t;

/****************************** 采样状态 ******************************/

/**
 * @brief 业务流量差分采样基线。
 *
 * 保留供运行监控和性能诊断使用，不作为正常切换的
 * 带宽准入条件。
 */
typedef struct
{
    bool     initialized; // 是否已经取得上一轮累计统计
    uint32_t link_id;     // 当前采样对应的Link实例
    uint64_t sampled_us;  // 上一轮累计统计采样时间
    uint64_t tx_bytes;    // 上一轮累计发送字节数
    uint64_t rx_bytes;    // 上一轮累计接收字节数
    uint64_t tx_packets;  // 上一轮累计发送帧数量
    uint64_t rx_packets;  // 上一轮累计接收帧数量
} linkg_switch_traffic_sampler_t;

/**
 * @brief Wire接收丢包差分采样基线。
 *
 * 接收端维护，不包含发送端尚未提交Socket的
 * 队列溢出和限速主动丢包。
 */
typedef struct
{
    bool     initialized;      // 是否已经取得上一轮累计统计
    uint32_t link_id;          // 当前采样对应的Link实例
    uint64_t received_packets; // 上一轮累计唯一接收帧数
    uint64_t lost_packets;     // 上一轮累计确认丢失帧数
} linkg_switch_loss_sampler_t;

/**
 * @brief TX本地负载丢包差分采样基线。
 *
 * Wi-Fi使用queue_overflow。
 * Cellular使用queue_overflow和shaped。
 *
 * sent_packets为Path成功提交底层发送接口的帧数量。
 * 所有累计值必须归属于同一个Peer的同一条Path。
 */
typedef struct
{
    bool     initialized;            // 是否已经取得上一轮累计统计
    uint32_t link_id;                // 当前采样对应的Link实例
    uint64_t sent_packets;           // 上一轮累计成功提交底层的帧数
    uint64_t queue_overflow_packets; // 上一轮累计TX队列溢出丢弃帧数
} linkg_switch_load_sampler_t;

/****************************** 观测操作 ******************************/

int  linkg_switch_observation_refresh(uint8_t peer_node_id, uint32_t peer_generation, uint64_t now_us);
int  linkg_switch_observation_update_remote_uplink_loss(uint8_t peer_node_id, uint32_t loss_permille, uint32_t sample_packets);
void linkg_switch_observation_reset_locked(uint8_t peer_node_id);

#endif
