/**
 * @file switch_observation.h
 * @brief LinkG Switch统一指标样本及观测接口
 */

#ifndef SWITCH_OBSERVATION_H
#define SWITCH_OBSERVATION_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_transport_types.h"
#include "linkg_wifi_config.h"

/****************************** 可用性状态 ******************************/

typedef enum
{
    LINKG_SWITCH_OBSERVATION_UNKNOWN  = 0, // 证据缺失、过期或采集失败，不能判定可用性
    LINKG_SWITCH_OBSERVATION_USABLE   = 1, // 已确认当前路径具备基本使用条件
    LINKG_SWITCH_OBSERVATION_UNUSABLE = 2  // 已确认当前路径不具备基本使用条件
} linkg_switch_observation_state_t;

/****************************** Probe观测 ******************************/

typedef struct
{
    bool     valid;      // 最近结果是否有效：超时为有效的不可达结果，提交失败为无效结果
    bool     reachable;  // 有效结果是否在截止时间前收到匹配响应
    uint32_t rtt_us;     // 最近成功响应RTT，仅valid且reachable时使用
    uint64_t updated_us; // 结果源更新时间，CLOCK_MONOTONIC微秒；过期不刷新
} linkg_switch_probe_observation_t;

/****************************** 流量观测 ******************************/

typedef struct
{
    bool     valid;      // 本轮是否完成有效差分；无新样本或计数回退时为false
    uint32_t tx_pps;     // 当前发送包速率
    uint32_t rx_pps;     // 当前接收包速率
    uint64_t tx_bps;     // 当前发送比特率
    uint64_t rx_bps;     // 当前接收比特率
    uint64_t updated_us; // 本次有效速率计算时间
} linkg_switch_traffic_observation_t;

/****************************** 丢包观测 ******************************/

/**
 * @brief 当前统计窗口丢包样本；无样本为未知，不等于0丢包。
 *
 * 此结构仅提供计数，不定义质量阈值；Policy负责计算与判决。
 */
typedef struct
{
    bool     valid;          // 有效统计窗口是否存在
    uint32_t lost_packets;   // 本窗口确认丢包数量
    uint32_t sample_packets; // 本窗口总样本数量，包含成功与确认丢失
    uint64_t updated_us;     // 本窗口生成时间；重复复制不改变
} linkg_switch_loss_observation_t;

typedef struct
{
    linkg_switch_loss_observation_t uplink;                                        // STA到AP Wire整链路丢包，由AP上报
    linkg_switch_loss_observation_t uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];   // STA到AP Wire三业务丢包
    linkg_switch_loss_observation_t uplink_load;                                   // STA到AP TX队列溢出，由STA采集
    linkg_switch_loss_observation_t downlink;                                      // AP到STA Wire整链路丢包，由STA采集
    linkg_switch_loss_observation_t downlink_classes[LINKG_TRANSPORT_CLASS_COUNT]; // AP到STA Wire三业务丢包
    linkg_switch_loss_observation_t downlink_load;                                 // AP到STA TX队列溢出，由AP上报
} linkg_switch_path_loss_observation_t;

/****************************** Wi-Fi HCC观测 ******************************/

/**
 * @brief 本机HI1105 HCC发送队列及Device流控状态。
 *
 * VO、VI、BE分别对应实时、视频、普通业务发送优先级。
 * 队列深度为本机HCC TX瞬时积压，不代表指定Peer的独立统计。
 * FLOWCTRL_OFF及tx_allowed属于HCC全局流控状态。
 * 本结构只记录原始观测，不执行负载质量判决。
 */
typedef struct
{
    bool     valid;              // 本轮HCC状态是否成功读取

    uint32_t vo_queue_length;    // VO实时业务发送队列当前长度
    uint32_t vi_queue_length;    // VI视频业务发送队列当前长度
    uint32_t be_queue_length;    // BE普通业务发送队列当前长度

    uint32_t flowctrl_off_count; // Device FLOWCTRL_OFF累计次数
    bool     tx_allowed;         // 当前HCC是否允许Host向Device发送

    uint64_t updated_us;         // 本轮有效采集时间，CLOCK_MONOTONIC微秒
} linkg_switch_wifi_hcc_observation_t;

/****************************** Wi-Fi无线观测 ******************************/

typedef struct
{
    bool                     valid;                 // 无线基础状态存在，且更新时间未过期
    bool                     connected;             // 有效基础状态下，是否明确关联指定AP
    bool                     statistics_valid;      // 指定AP无线统计存在且未过期
    int32_t                  rssi_dbm;              // 当前接收信号强度，statistics_valid时有效
    uint32_t                 tx_phy_kbps;           // 当前发送PHY速率，statistics_valid时有效
    uint32_t                 rx_phy_kbps;           // 当前接收PHY速率，statistics_valid时有效
    uint32_t                 inactive_ms;           // 最近一次与Peer无线活动间隔
    bool                     noise_valid;           // 当前信道噪声数据有效
    int32_t                  noise_dbm;             // 当前信道噪声，noise_valid时有效
    linkg_wifi_work_mode_t   work_mode;             // 当前宽带/窄带工作模式
    uint16_t                 bandwidth_mhz;         // 当前工作带宽
    linkg_wifi_narrow_mode_t narrow_mode;           // 当前窄带速率控制模式
    bool                     rate_level_valid;      // 当前窄带速率档位有效
    uint16_t                 rate_level;            // 当前窄带速率档位
    bool                     temperature_valid;     // 当前Wi-Fi芯片温度有效
    int32_t                  temperature_c;         // 当前Wi-Fi芯片温度
    uint64_t                 status_updated_us;     // 最近无线基础状态源更新时间
    uint64_t                 statistics_updated_us; // 最近Peer无线统计源更新时间
} linkg_switch_wifi_radio_observation_t;

/****************************** Wi-Fi路径观测 ******************************/

typedef struct
{
    linkg_switch_observation_state_t      state;                              // 该Peer Wi-Fi路径可用性：可用/不可用/未知
    uint32_t                              link_id;                            // 当前Wi-Fi Link运行实例，无效值表示未获取到
    linkg_switch_probe_observation_t      probe[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务周期Probe结果
    linkg_switch_traffic_observation_t    traffic;                            // 当前Wi-Fi Path收发速率样本
    linkg_switch_path_loss_observation_t  loss;                               // 双向Wire及队列溢出样本
    linkg_switch_wifi_radio_observation_t radio;                              // Wi-Fi无线层指标
    linkg_switch_wifi_hcc_observation_t   uplink_hcc;                         // STA本机Wi-Fi上行发送HCC
    linkg_switch_wifi_hcc_observation_t   downlink_hcc;                       // AP远端Wi-Fi下行发送HCC
} linkg_switch_wifi_observation_t;

/****************************** Cellular路径观测 ******************************/

typedef struct
{
    linkg_switch_observation_state_t     state;                              // 该Peer Cellular路径可用性：可用/不可用/未知
    uint32_t                             link_id;                            // 当前Cellular Link运行实例
    linkg_switch_probe_observation_t     probe[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务周期Probe结果
    linkg_switch_path_loss_observation_t loss;                               // 双向Wire及队列溢出样本
    uint64_t                             updated_us;                         // 当前Cellular可用性评估完成时间
} linkg_switch_cellular_observation_t;

/****************************** 切换观测快照 ******************************/

typedef struct
{
    bool                                valid;            // 当前Peer观测快照已完成发布，不代表所有字段有效
    uint8_t                             peer_node_id;     // 当前直接AP Peer节点编号
    linkg_switch_wifi_observation_t     wifi;             // Wi-Fi指标与三态可用性
    linkg_switch_cellular_observation_t cellular;         // Cellular指标与三态可用性
    uint64_t                            collected_us;     // 本轮采集完成时间，CLOCK_MONOTONIC微秒
    int32_t                             collection_error; // 本轮首个非预期采集错误；0表示未遇到此类错误
} linkg_switch_observation_t;

/****************************** 差分采样基线 ******************************/

typedef struct
{
    bool     initialized; // 已取得上一轮累计值
    uint32_t link_id;     // 基线所属Link实例
    uint64_t sampled_us;  // 上一轮采样时间
    uint64_t tx_bytes;    // 上一轮累计发送字节数
    uint64_t rx_bytes;    // 上一轮累计接收字节数
    uint64_t tx_packets;  // 上一轮累计发送帧数
    uint64_t rx_packets;  // 上一轮累计接收帧数
} linkg_switch_traffic_sampler_t;

typedef struct
{
    uint64_t lost_packets;   // 上一轮累计确认丢包数
    uint64_t sample_packets; // 上一轮累计样本数
} linkg_switch_loss_class_sampler_t;

typedef struct
{
    bool                              initialized;                          // 已取得上一轮累计值
    uint32_t                          link_id;                              // 基线所属Link实例
    uint64_t                          received_packets;                     // 上一轮累计接收帧数
    uint64_t                          lost_packets;                         // 上一轮累计确认丢失帧数
    linkg_switch_loss_class_sampler_t classes[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务独立累计基线
} linkg_switch_loss_sampler_t;

typedef struct
{
    bool     initialized;            // 已取得上一轮累计值
    uint32_t link_id;                // 基线所属Link实例
    uint64_t sent_packets;           // 上一轮累计成功提交帧数
    uint64_t queue_overflow_packets; // 上一轮累计TX队列溢出丢弃帧数
} linkg_switch_load_sampler_t;

/****************************** 观测操作 ******************************/

int  linkg_switch_observation_refresh(uint8_t peer_node_id, uint32_t peer_generation, uint64_t now_us);
void linkg_switch_observation_reset_locked(uint8_t peer_node_id);

/****************************** Probe状态转换 ******************************/

linkg_switch_observation_state_t linkg_switch_observation_probe_state(const linkg_switch_probe_observation_t *probe);

#endif
