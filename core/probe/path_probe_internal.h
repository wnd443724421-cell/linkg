/**
 * @file path_probe_internal.h
 * @brief LinkG业务Path主动探测内部定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#ifndef PATH_PROBE_INTERNAL_H
#define PATH_PROBE_INTERNAL_H

#include "linkg_path_probe.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_node.h"
#include "linkg_thread.h"
#include "linkg_transport.h"

/****************************** 模块常量 ******************************/

#define LINKG_PATH_PROBE_THREAD_NAME               "path-probe"         // Probe工作线程名称
#define LINKG_PATH_PROBE_STA_INTERVAL_US           1000000ULL           // STA默认周期探测间隔
#define LINKG_PATH_PROBE_AP_INTERVAL_US            5000000ULL           // AP默认低频周期探测间隔
#define LINKG_PATH_PROBE_CELLULAR_FAST_INTERVAL_US 100000ULL            // Cellular主用低流量高频探测间隔
#define LINKG_PATH_PROBE_DEFAULT_TIMEOUT_US        300000ULL            // 默认Probe响应超时时间
#define LINKG_PATH_PROBE_CELLULAR_FAST_PPS_MAX     100U                 // Cellular高频Probe最大业务PPS
#define LINKG_PATH_PROBE_MAGIC_SIZE                3U                   // Probe协议Magic长度
#define LINKG_PATH_PROBE_PENDING_MAX               256U                 // 最大同时在途Probe数量
#define LINKG_PATH_PROBE_PEER_MAX                  LINKG_NODE_PEER_MAX  // 最大直接Peer数量
#define LINKG_PATH_PROBE_SEQUENCE_INVALID          0U                   // 无效Probe序列号
#define LINKG_PATH_PROBE_DIAGNOSTIC_INDEX_INVALID UINT32_MAX           // 无效诊断样本编号

/****************************** Wire类型 ******************************/

typedef enum
{
    LINKG_PATH_PROBE_WIRE_TYPE_INVALID  = 0, // 无效Probe报文
    LINKG_PATH_PROBE_WIRE_TYPE_REQUEST,      // Probe请求
    LINKG_PATH_PROBE_WIRE_TYPE_RESPONSE      // Probe响应
} linkg_path_probe_wire_type_t;

typedef struct
{
    uint8_t  magic[LINKG_PATH_PROBE_MAGIC_SIZE]; // Probe协议Magic
    uint8_t  type;                               // linkg_path_probe_wire_type_t
    uint32_t sequence;                           // Probe序列号
} linkg_path_probe_wire_t;

_Static_assert(sizeof(linkg_path_probe_wire_t) == 8U, "invalid path probe wire size");

/****************************** Class运行状态 ******************************/

typedef struct
{
    linkg_path_probe_class_snapshot_t snapshot;              // 当前公开周期探测结果
    uint64_t                          interval_us;            // 当前周期探测间隔
    uint64_t                          next_probe_us;          // 下一次周期Probe时间
    uint64_t                          latest_result_sent_us;  // 当前公开结果对应Probe发送时间
} linkg_path_probe_class_runtime_t;

/****************************** Path运行状态 ******************************/

typedef struct
{
    linkg_path_probe_class_runtime_t classes[LINKG_LINK_TX_CLASS_COUNT]; // 三个业务类别运行状态
    uint32_t                         link_id;                             // 当前业务Link运行实例标识
    bool                             active;                              // 当前业务Path是否存在
} linkg_path_probe_path_runtime_t;

/****************************** Peer运行状态 ******************************/

typedef struct
{
    linkg_path_probe_path_runtime_t wifi;         // Wi-Fi业务Path运行状态
    linkg_path_probe_path_runtime_t cellular;     // Cellular业务Path运行状态
    uint8_t                         peer_node_id; // 直接Peer节点编号
    bool                            used;         // 当前Peer槽位是否使用
} linkg_path_probe_peer_runtime_t;

/****************************** Pending类型 ******************************/

typedef enum
{
    LINKG_PATH_PROBE_PENDING_TYPE_INVALID = 0, // 无效Pending
    LINKG_PATH_PROBE_PENDING_TYPE_PERIODIC,    // 周期健康Probe
    LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC   // 主动诊断Probe
} linkg_path_probe_pending_type_t;

typedef struct
{
    uint32_t                        sequence;         // Probe序列号
    uint32_t                        link_id;          // 实际发送业务Link运行实例标识
    uint32_t                        diagnostic_index; // 对应诊断样本编号
    uint64_t                        sent_us;          // Probe实际发送时间
    uint64_t                        deadline_us;      // Probe响应超时时间点
    uint8_t                         peer_node_id;     // 目标直接Peer节点编号
    linkg_link_access_t             access;           // 实际发送业务Path
    linkg_link_tx_class_t           tx_class;         // 实际发送业务类别
    linkg_path_probe_pending_type_t type;             // Pending所属任务类型
    bool                            used;             // 当前Pending槽位是否使用
} linkg_path_probe_pending_t;

/****************************** 诊断样本 ******************************/

typedef enum
{
    LINKG_PATH_PROBE_DIAGNOSTIC_SAMPLE_UNUSED = 0, // 尚未发送
    LINKG_PATH_PROBE_DIAGNOSTIC_SAMPLE_PENDING,    // 已发送并等待响应
    LINKG_PATH_PROBE_DIAGNOSTIC_SAMPLE_RECEIVED,   // 已成功收到响应
    LINKG_PATH_PROBE_DIAGNOSTIC_SAMPLE_SEND_FAILED, // 本地发送失败
    LINKG_PATH_PROBE_DIAGNOSTIC_SAMPLE_TIMEOUT      // 已发送但响应超时
} linkg_path_probe_diagnostic_sample_state_t;

typedef struct
{
    uint32_t                                  sequence; // 当前样本Probe序列号
    uint32_t                                  rtt_us;   // 成功响应RTT，单位微秒
    uint64_t                                  sent_us;  // 当前样本实际发送时间
    linkg_path_probe_diagnostic_sample_state_t state;   // 当前诊断样本状态
} linkg_path_probe_diagnostic_sample_t;

/****************************** 诊断运行状态 ******************************/

typedef struct
{
    linkg_path_probe_diagnostic_request_t request; // 当前诊断请求
    linkg_path_probe_diagnostic_result_t  result;  // 当前诊断最终结果

    linkg_path_probe_diagnostic_sample_t samples[LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX]; // 当前诊断样本

    uint64_t started_us;          // 当前诊断开始时间
    uint64_t next_send_us;        // 下一Probe发送时间
    uint64_t send_interval_us;    // Probe发送间隔
    uint64_t response_timeout_us; // 单个Probe响应超时时间

    uint32_t next_sample_index;   // 下一待发送样本编号
    uint32_t pending_count;       // 当前仍在等待响应的诊断Probe数量
    uint32_t received_count;      // 当前成功收到响应数量
    uint32_t send_failed_count;   // 当前本地发送失败数量

    int      status;              // 当前诊断最终执行结果
    bool     active;              // 当前是否存在诊断调用占用任务槽位
    bool     completed;           // 当前诊断是否已经完成
} linkg_path_probe_diagnostic_runtime_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t                       lock;                                  // Probe公共状态保护锁
    pthread_cond_t                        diagnostic_condition;                  // 主动诊断完成等待条件变量
    linkg_thread_t                        thread;                                // Probe工作线程

    linkg_path_probe_peer_runtime_t       peers[LINKG_PATH_PROBE_PEER_MAX];      // 直接Peer固定槽位
    linkg_path_probe_pending_t            pending[LINKG_PATH_PROBE_PENDING_MAX]; // 全部在途Probe固定槽位
    linkg_path_probe_diagnostic_runtime_t diagnostic;                            // 当前唯一主动诊断任务

    linkg_device_role_t                   role;                                  // 本机设备角色
    uint32_t                              next_sequence;                         // 下一个Probe序列号
    uint32_t                              peer_count;                            // 当前直接Peer数量

    bool                                  initialized;                           // 模块是否已经初始化
    bool                                  running;                               // 模块是否正在运行
} linkg_path_probe_context_t;

extern linkg_path_probe_context_t g_path_probe;

/****************************** Transport接收 ******************************/

int _linkg_path_probe_transport_receive(const linkg_transport_delivery_t *items, uint32_t count, void *user_data);

#endif
