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
extern "C"
{
#endif

/****************************** 模块常量 ******************************/

#define LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX 128U // 单次主动诊断最大Probe数量

/****************************** 前置声明 ******************************/

typedef struct linkg_packet_pool linkg_packet_pool_t; // 发送Packet Pool，由应用层持有

/****************************** 探测快照 ******************************/

typedef struct
{
    bool     valid;      // 最近一次周期探测是否形成网络结果，本地提交失败为false
    bool     reachable;  // 最近一次周期探测是否在超时前收到匹配响应
    uint32_t rtt_us;     // 最近一次成功RTT，仅valid且reachable时作为当前值
    uint64_t updated_us; // 最近一次结果时间，CLOCK_MONOTONIC微秒
} linkg_path_probe_class_snapshot_t;

typedef struct
{
    bool                              active;                               // 最近同步时Node中是否存在活动Path
    uint32_t                          link_id;                              // 本地业务Link运行实例标识
    linkg_path_probe_class_snapshot_t classes[LINKG_TRANSPORT_CLASS_COUNT]; // 三业务类别周期结果
} linkg_path_probe_path_snapshot_t;

typedef struct
{
    uint8_t                          peer_node_id; // 直接Peer节点编号
    linkg_path_probe_path_snapshot_t wifi;         // Wi-Fi周期探测快照
    linkg_path_probe_path_snapshot_t cellular;     // Cellular周期探测快照
} linkg_path_probe_peer_snapshot_t;

/****************************** 主动诊断 ******************************/

typedef struct
{
    uint8_t                 peer_node_id;        // 直接Peer节点编号，STA仅允许其直接AP
    linkg_link_access_t     access;              // 指定接入类型，WIFI或CELLULAR
    linkg_transport_class_t traffic_class;       // 指定Transport业务类别
    uint32_t                packet_count;        // 1~128，发送不等待前一包响应
    uint32_t                send_window_ms;      // 发送时间窗，首包在起点，后续均匀分布
    uint32_t                response_timeout_ms; // 从Scheduler提交前计时的单包响应期限
} linkg_path_probe_diagnostic_request_t;

typedef struct
{
    uint32_t requested_packets;   // 请求Probe数量，诊断中止时可能未全部尝试
    uint32_t sent_packets;        // Scheduler成功接受的Probe数量，不等于硬件出线计数
    uint32_t received_packets;    // 期限内唯一匹配响应数量
    uint32_t send_failed_packets; // 本地提交失败或错过发送时隙数量
    uint32_t lost_packets;        // 已接受但响应超时数量，不含本地提交失败
    uint32_t loss_permille;       // 超时比例0~1000，无成功提交时为0且不能解释成零丢包
    uint32_t min_rtt_us;          // 成功样本最小RTT，无成功样本时为0
    uint32_t average_rtt_us;      // 成功样本平均RTT，无成功样本时为0
    uint32_t max_rtt_us;          // 成功样本最大RTT，无成功样本时为0
    uint32_t jitter_us;           // 按发送顺序相邻成功样本RTT绝对差均值，少于2个为0
    uint64_t started_us;          // 诊断开始时间，CLOCK_MONOTONIC微秒
    uint64_t completed_us;        // 诊断结束时间，CLOCK_MONOTONIC微秒
} linkg_path_probe_diagnostic_result_t;

/****************************** 生命周期 ******************************/

int linkg_path_probe_init(linkg_packet_pool_t *packet_pool);
int linkg_path_probe_start(void);
int linkg_path_probe_stop(void);
int linkg_path_probe_deinit(void);

/****************************** 状态查询 ******************************/

int linkg_path_probe_get_peer_snapshot(uint8_t peer_node_id, linkg_path_probe_peer_snapshot_t *snapshot);

/****************************** 主动诊断 ******************************/

int linkg_path_probe_diagnose(const linkg_path_probe_diagnostic_request_t *request, linkg_path_probe_diagnostic_result_t *result);

/****************************** 非阻塞单包Probe ******************************/

/**
 * @brief 单包探测当前状态。
 *
 * PENDING不是网络失败；收到ACK、超时和本地发送失败是不同终态。
 * CANCELLED表示绑定的物理Path发生变化，不能参与质量积分。
 */
typedef enum
{
    LINKG_PATH_PROBE_ASYNC_PENDING = 0,
    LINKG_PATH_PROBE_ASYNC_ACK,
    LINKG_PATH_PROBE_ASYNC_TIMEOUT,
    LINKG_PATH_PROBE_ASYNC_SEND_FAILED,
    LINKG_PATH_PROBE_ASYNC_CANCELED
} linkg_path_probe_async_state_t;

/**
 * @brief 单次Probe完成结果。
 *
 * sent_us从Scheduler提交前计时；未执行提交时为0。
 * completed_us是终态形成时间；ACK的rtt_us为真实往返耗时。
 */
typedef struct
{
    linkg_path_probe_async_state_t state;
    uint64_t                       sent_us;
    uint64_t                       completed_us;
    uint32_t                       sequence;
    uint32_t                       rtt_us;
    linkg_transport_class_t        traffic_class;
} linkg_path_probe_async_result_t;

/**
 * @brief 非阻塞提交一包Wi-Fi Probe，实际发送由原Probe Worker完成。
 *
 * 同一个业务同时只允许一个请求；旧终态必须poll消费或cancel后才可重用。
 * 返回0仅表示任务已建立，不代表Scheduler提交成功或收到ACK。
 */
int linkg_path_probe_async_submit(uint8_t peer_node_id, uint32_t wifi_link_id, linkg_transport_class_t traffic_class, uint32_t response_timeout_ms, uint64_t *handle);

/**
 * @brief 非阻塞读取单包结果。
 *
 * 返回0并复制结果；PENDING不释放请求，其余终态读取后释放槽位。
 * 返回负值代表无效句柄、模块停止或其他API错误。
 */
int linkg_path_probe_async_poll(uint64_t handle, linkg_path_probe_async_result_t *result);

/**
 * @brief 取消一包Probe并释放结果槽位。
 *
 * 对已完成但尚未poll的请求同样有效；不会等待发送或ACK。
 */
int linkg_path_probe_async_cancel(uint64_t handle);


#ifdef __cplusplus
}
#endif

#endif
