/**
 * @file linkg_path_probe.h
 * @brief LinkG业务Path主动探测接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#ifndef LINKG_PATH_PROBE_H
#define LINKG_PATH_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"
#include "linkg_transport_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 模块常量 ******************************/

#define LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX 128U // 单次主动诊断最大Probe数量

/****************************** Class探测快照 ******************************/

typedef struct
{
    bool     valid;      // 当前是否已有周期探测结果
    bool     reachable;  // 最近一次周期探测是否可达
    uint32_t rtt_us;     // 最近一次成功周期探测RTT，单位微秒
    uint64_t updated_us; // 最近一次周期探测结果更新时间
} linkg_path_probe_class_snapshot_t;

/****************************** Path探测快照 ******************************/

typedef struct
{
    bool                              active;                              // 当前业务Path是否存在
    uint32_t                          link_id;                             // 当前业务Link运行实例标识
    linkg_path_probe_class_snapshot_t classes[LINKG_LINK_TX_CLASS_COUNT];  // 三个业务类别周期探测快照
} linkg_path_probe_path_snapshot_t;

/****************************** Peer探测快照 ******************************/

typedef struct
{
    uint8_t                          peer_node_id; // 直接Peer节点编号
    linkg_path_probe_path_snapshot_t wifi;         // Wi-Fi业务Path探测快照
    linkg_path_probe_path_snapshot_t cellular;     // Cellular业务Path探测快照
} linkg_path_probe_peer_snapshot_t;

/****************************** 主动诊断请求 ******************************/

typedef struct
{
    uint8_t                peer_node_id;        // 直接Peer节点编号
    linkg_link_access_t    access;              // 目标业务Path接入类型
    linkg_link_tx_class_t  tx_class;            // 目标业务类别
    uint32_t               packet_count;        // 本次诊断请求Probe数量
    uint32_t               send_window_ms;      // Probe发送时间窗，单位毫秒
    uint32_t               response_timeout_ms; // 单个Probe响应超时时间，单位毫秒
} linkg_path_probe_diagnostic_request_t;

/****************************** 主动诊断结果 ******************************/

typedef struct
{
    uint32_t requested_packets;   // 请求发送Probe数量
    uint32_t sent_packets;        // 实际成功发送Probe数量
    uint32_t received_packets;    // 成功收到响应数量
    uint32_t send_failed_packets; // 本地发送失败数量
    uint32_t lost_packets;        // 已发送但未收到响应数量
    uint32_t loss_permille;       // 网络丢包率，千分比0~1000

    uint32_t min_rtt_us;          // 成功样本最小RTT，单位微秒
    uint32_t average_rtt_us;      // 成功样本平均RTT，单位微秒
    uint32_t max_rtt_us;          // 成功样本最大RTT，单位微秒
    uint32_t jitter_us;           // 相邻成功样本RTT平均绝对差，单位微秒

    uint64_t started_us;          // 本次诊断开始时间
    uint64_t completed_us;        // 本次诊断完成时间
} linkg_path_probe_diagnostic_result_t;

/****************************** 生命周期 ******************************/

int linkg_path_probe_init(void);
int linkg_path_probe_start(void);
int linkg_path_probe_stop(void);
int linkg_path_probe_deinit(void);

/****************************** 状态查询 ******************************/

/**
 * @brief 获取指定直接Peer当前全部业务Path的周期探测快照。
 *
 * @note reachable为false时，rtt_us仅保留最近一次成功探测的历史值，
 *       调用方不得将其作为当前有效RTT使用。
 */
int linkg_path_probe_get_peer_snapshot(uint8_t peer_node_id, linkg_path_probe_peer_snapshot_t *snapshot);

/****************************** 主动诊断 ******************************/

/**
 * @brief 对指定直接Peer业务Path执行一次多包主动诊断。
 *
 * 本接口为同步阻塞接口，调用线程会等待整个发送时间窗及最后在途
 * Probe完成或超时。不得从Link RX、Transport RX、Scheduler或其他
 * 数据面热路径调用。
 *
 * packet_count必须位于1~LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX。
 * packet_count为1时允许send_window_ms为0，多包诊断send_window_ms必须大于0。
 * response_timeout_ms必须大于0。
 *
 * loss_permille仅统计成功发送到网络后的Probe丢失比例，
 * 本地发送失败通过send_failed_packets单独返回。
 *
 * 完成后满足：
 * requested_packets = sent_packets + send_failed_packets
 * sent_packets = received_packets + lost_packets
 *
 * RTT统计仅基于成功收到响应的Probe样本。
 */
int linkg_path_probe_diagnose(const linkg_path_probe_diagnostic_request_t *request, linkg_path_probe_diagnostic_result_t *result);

#ifdef __cplusplus
}
#endif

#endif
