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
#include "linkg_packet_pool.h"
#include "linkg_thread.h"
#include "linkg_transport.h"

/****************************** 模块常量 ******************************/

#define LINKG_PATH_PROBE_THREAD_NAME                  "path-probe"        // Probe线程名称
#define LINKG_PATH_PROBE_INTERVAL_US                  1000000ULL          // AP全部路径及STA普通路径周期
#define LINKG_PATH_PROBE_CELLULAR_FAST_INTERVAL_US    100000ULL           // STA主用Cellular三类别快速周期
#define LINKG_PATH_PROBE_DEFAULT_TIMEOUT_US           300000ULL           // 周期Probe响应超时
#define LINKG_PATH_PROBE_TOPOLOGY_INTERVAL_US         1000000ULL          // Direct Peer及Path列表同步周期
#define LINKG_PATH_PROBE_POLICY_INTERVAL_US           100000ULL           // STA主链路计划读取周期
#define LINKG_PATH_PROBE_PPS_SAMPLE_INTERVAL_US       1000000ULL          // Cellular真实业务PPS统计窗口
#define LINKG_PATH_PROBE_HIGH_PPS_THRESHOLD           100U                // 超过该PPS后关闭100ms快速Probe
#define LINKG_PATH_PROBE_WORK_BUDGET                  32U                 // 单轮Worker最大发送或过期时隙处理数量
#define LINKG_PATH_PROBE_DIAGNOSTIC_MIN_GAP_US        1000ULL             // 主动诊断最小发送间隔
#define LINKG_PATH_PROBE_DIAGNOSTIC_WINDOW_MAX_MS     60000U              // 单次诊断最大发送窗口
#define LINKG_PATH_PROBE_DIAGNOSTIC_TIMEOUT_MAX_MS    10000U              // 单包诊断最大响应等待
#define LINKG_PATH_PROBE_DIAGNOSTIC_DISPATCH_GRACE_US 100000ULL           // 诊断总截止时间预留Worker启动调度余量
#define LINKG_PATH_PROBE_MAGIC_SIZE                   3U                  // Probe Magic长度
#define LINKG_PATH_PROBE_WIRE_SIZE                    8U                  // Probe固定载荷长度
#define LINKG_PATH_PROBE_PENDING_MAX                  256U                // 全部在途记录容量
#define LINKG_PATH_PROBE_PERIODIC_PENDING_MAX         128U                // 周期记录独立保留容量
#define LINKG_PATH_PROBE_PEER_MAX                     LINKG_NODE_PEER_MAX // 最大直接Peer数量
#define LINKG_PATH_PROBE_SEQUENCE_INVALID             0U                  // 无效Wire序列号
#define LINKG_PATH_PROBE_DIAGNOSTIC_INDEX_INVALID     UINT32_MAX          // 非诊断记录的样本索引

/****************************** Wire类型 ******************************/

typedef enum
{
    LINKG_PATH_PROBE_WIRE_TYPE_INVALID  = 0, // 无效Probe类型
    LINKG_PATH_PROBE_WIRE_TYPE_REQUEST  = 1, // Probe请求
    LINKG_PATH_PROBE_WIRE_TYPE_RESPONSE = 2  // Probe响应
} linkg_path_probe_wire_type_t;

typedef struct
{
    uint8_t  magic[LINKG_PATH_PROBE_MAGIC_SIZE]; // Probe Magic
    uint8_t  type;                               // linkg_path_probe_wire_type_t的单字节编码
    uint32_t sequence;                           // Wire为网络序，decode输出为主机序
} linkg_path_probe_wire_t;

_Static_assert(sizeof(linkg_path_probe_wire_t) == LINKG_PATH_PROBE_WIRE_SIZE, "invalid path probe wire size");
_Static_assert(LINKG_PATH_PROBE_PERIODIC_PENDING_MAX + LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX == LINKG_PATH_PROBE_PENDING_MAX, "invalid pending pool partition");
_Static_assert(LINKG_NODE_PATH_MAX == 2U && LINKG_TRANSPORT_CLASS_COUNT == 3U, "unsupported probe path or class count");
_Static_assert(LINKG_PATH_PROBE_PERIODIC_PENDING_MAX >= LINKG_PATH_PROBE_PEER_MAX * LINKG_NODE_PATH_MAX * LINKG_TRANSPORT_CLASS_COUNT, "insufficient periodic probe slots");

/****************************** 周期运行状态 ******************************/

typedef struct
{
    linkg_path_probe_class_snapshot_t snapshot;              // 当前公开周期结果
    uint64_t                          interval_us;           // 当前周期，100ms或1s
    uint64_t                          next_probe_us;         // 下次周期发送时间
    uint64_t                          latest_result_sent_us; // 最新公开结果对应的请求提交时间
} linkg_path_probe_class_runtime_t;

typedef struct
{
    linkg_path_probe_class_runtime_t classes[LINKG_TRANSPORT_CLASS_COUNT]; // 三类别周期状态
    linkg_path_endpoint_t            endpoint;                             // 仅用于识别端点变化，不作为缓存发送目的地址
    uint64_t                         pps_sample_started_us;                 // Cellular业务PPS统计窗口起始时间
    uint64_t                         pps_last_business_packets;            // 上次统计累计真实业务包数
    uint64_t                         probe_tx_packets;                      // 本模块经该Path成功提交的Probe帧数
    uint64_t                         probe_rx_packets;                      // 本模块经该Path收到的有效Probe帧数
    uint64_t                         generation;                           // 本模块观察到的Path代际
    uint32_t                         link_id;                              // 本地Link实例标识
    uint32_t                         current_pps;                          // 最近一个完整窗口的真实业务PPS
    bool                             active;                               // 最近同步的Node Path活动状态
    bool                             pps_sample_initialized;               // Cellular业务PPS是否已经建立基线
} linkg_path_probe_path_runtime_t;

typedef struct
{
    linkg_path_probe_path_runtime_t wifi;         // Wi-Fi Path状态
    linkg_path_probe_path_runtime_t cellular;     // Cellular Path状态
    uint8_t                         peer_node_id; // 当前直接Peer编号
    bool                            used;         // 槽位是否使用
} linkg_path_probe_peer_runtime_t;

/****************************** 在途记录 ******************************/

typedef enum
{
    LINKG_PATH_PROBE_PENDING_TYPE_INVALID = 0, // 无效类型
    LINKG_PATH_PROBE_PENDING_TYPE_PERIODIC,    // 周期请求
    LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC   // 主动诊断请求
} linkg_path_probe_pending_type_t;

typedef enum
{
    LINKG_PATH_PROBE_PENDING_FREE = 0,    // 空闲槽位
    LINKG_PATH_PROBE_PENDING_RESERVED,    // 已预留，尚未调用Scheduler
    LINKG_PATH_PROBE_PENDING_SUBMITTING,  // Scheduler调用进行中，可暂存提前到达的响应
    LINKG_PATH_PROBE_PENDING_WAITING      // Scheduler已接受，等待响应或超时
} linkg_path_probe_pending_state_t;

typedef struct
{
    uint64_t                         path_generation;  // 请求所属Path观察代际
    uint64_t                         diagnostic_id;    // 所属诊断编号，周期请求为0
    uint64_t                         sent_us;          // Scheduler提交前时间，未提交时为预留时间
    uint64_t                         deadline_us;      // 单包响应截止时间
    uint64_t                         early_reply_us;   // Scheduler返回前到达的首个有效响应时间
    uint32_t                         sequence;         // Wire序列号
    uint32_t                         link_id;          // 实际指定的本地Link
    uint32_t                         diagnostic_index; // 诊断样本索引，周期请求为INVALID
    uint8_t                          peer_node_id;     // 目标直接Peer
    linkg_transport_class_t          traffic_class;    // 目标业务类别
    linkg_path_probe_pending_type_t  type;             // 记录所属任务类型
    linkg_path_probe_pending_state_t state;            // 记录状态
    bool                             early_reply;      // 是否已收到提交期间的响应
} linkg_path_probe_pending_t;

/****************************** 诊断运行状态 ******************************/

typedef enum
{
    LINKG_PATH_PROBE_SAMPLE_UNUSED = 0,  // 尚未尝试发送
    LINKG_PATH_PROBE_SAMPLE_RESERVED,    // 已预留在途记录，等待提交结果
    LINKG_PATH_PROBE_SAMPLE_PENDING,     // Scheduler已接受，等待响应
    LINKG_PATH_PROBE_SAMPLE_RECEIVED,    // 已收到匹配响应
    LINKG_PATH_PROBE_SAMPLE_SEND_FAILED, // 本地提交失败或错过发送时隙
    LINKG_PATH_PROBE_SAMPLE_TIMEOUT,     // 已接受但响应超时
    LINKG_PATH_PROBE_SAMPLE_CANCELED     // 停止或Path变化取消，不计网络丢失
} linkg_path_probe_diagnostic_sample_state_t;

typedef struct
{
    uint64_t                                   sent_us;  // Scheduler提交前时间
    uint32_t                                   sequence; // 样本序列号
    uint32_t                                   rtt_us;   // 有效响应RTT
    linkg_path_probe_diagnostic_sample_state_t state;    // 样本状态
} linkg_path_probe_diagnostic_sample_t;

typedef struct
{
    linkg_path_probe_diagnostic_request_t request;                                         // 本次请求副本，不借用调用者内存
    linkg_path_probe_diagnostic_result_t  result;                                          // 完成结果，调用线程取走前不复用
    linkg_path_probe_diagnostic_sample_t  samples[LINKG_PATH_PROBE_DIAGNOSTIC_PACKET_MAX]; // 按发送顺序排列的样本
    uint64_t                              id;                                              // 本次任务唯一编号
    uint64_t                              path_generation;                                 // 固定目标Path代际
    uint64_t                              started_us;                                      // 诊断开始时间
    uint64_t                              send_window_us;                                  // 发送窗口长度
    uint64_t                              next_send_us;                                    // 下次计划发送时间
    uint64_t                              final_deadline_us;                               // 发送窗口、响应期限及调度余量形成的总截止时间
    uint32_t                              link_id;                                         // 诊断固定的本地Link实例
    uint32_t                              next_sample_index;                               // 下一待尝试样本
    uint32_t                              pending_count;                                   // 仍占用在途记录的样本数
    int                                   status;                                          // 0为正常完成，负值为诊断中止原因
    bool                                  active;                                          // 调用者占有此任务槽位
    bool                                  completed;                                       // 结果已完成，等待调用者读取
} linkg_path_probe_diagnostic_runtime_t;

/****************************** 发送任务 ******************************/

typedef struct
{
    linkg_path_endpoint_t   endpoint;        // 选中任务时端点，仅用于发送前核对是否变化
    uint64_t                path_generation; // 选中任务的Path代际
    uint32_t                pending_index;   // 在途槽位索引
    uint32_t                sequence;        // 槽位复用校验序列号
    uint32_t                link_id;         // 本地指定Link
    uint8_t                 peer_node_id;    // 直接Peer编号
    linkg_transport_class_t traffic_class;   // 业务类别
} linkg_path_probe_tx_task_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t                        lock;                                  // Probe状态锁，进程生命周期内保持有效
    pthread_cond_t                         diagnostic_condition;                  // MONOTONIC条件变量，进程生命周期内保持有效
    pthread_t                              worker_tid;                            // Worker线程身份，仅在Probe锁下读写
    bool                                   worker_tid_valid;                      // Worker身份是否有效
    linkg_thread_t                         thread;                                // 唯一周期及诊断发送线程
    linkg_packet_pool_t                   *packet_pool;                           // 应用层Packet Pool，仅借用，stop排空后方可销毁
    linkg_path_probe_peer_runtime_t        peers[LINKG_PATH_PROBE_PEER_MAX];      // 直接Peer固定槽位
    linkg_path_probe_pending_t             pending[LINKG_PATH_PROBE_PENDING_MAX]; // 在途固定槽位，不持有Packet或Path引用
    linkg_path_probe_diagnostic_runtime_t  diagnostic;                            // STA唯一主动诊断任务
    uint64_t                               next_path_generation;                  // Path观察代际分配器，重启模块不回退
    uint64_t                               next_diagnostic_id;                    // 诊断编号分配器，重启模块不回退
    uint64_t                               next_topology_us;                      // 下一次Peer及Path同步时间
    uint64_t                               next_policy_us;                        // 下一次STA主链路查询时间
    uintptr_t                              run_token;                             // 注册回调的运行代际，隔离延迟进入的旧回调
    linkg_device_role_t                    role;                                  // 本机角色
    uint32_t                               next_sequence;                         // 序列号分配器，跳过0及仍在途序列号
    uint32_t                               peer_count;                            // 已跟踪直接Peer数量
    uint32_t                               rx_users;                              // 已进入且尚未返回的Transport回调数量
    uint8_t                                local_node_id;                         // 本机Node ID
    bool                                   initialized;                           // 模块已初始化
    bool                                   running;                               // 接受新请求及回调
    bool                                   handler_registered;                    // PATH_PROBE回调属于本模块
} linkg_path_probe_context_t;

extern linkg_path_probe_context_t g_path_probe;

/****************************** Wire协议 ******************************/

int linkg_path_probe_wire_encode(linkg_packet_t *packet, linkg_path_probe_wire_type_t type, uint32_t sequence);
int linkg_path_probe_wire_decode(const linkg_packet_t *packet, linkg_path_probe_wire_t *wire);

/****************************** 运行状态 ******************************/

bool                             linkg_path_probe_endpoint_equal(const linkg_path_endpoint_t *left, const linkg_path_endpoint_t *right);
int                              linkg_path_probe_read_target(linkg_device_role_t role, uint8_t local_node_id, uint8_t peer_node_id, uint32_t link_id, linkg_path_endpoint_t *endpoint);
linkg_path_probe_path_runtime_t *linkg_path_probe_find_path_locked(uint8_t peer_node_id, uint32_t link_id);
void                             linkg_path_probe_runtime_reset_locked(void);
void                             linkg_path_probe_cancel_diagnostic_locked(int status, uint64_t now_us);
void                             linkg_path_probe_cancel_all_locked(int status, uint64_t now_us);
int                              linkg_path_probe_mark_sending_locked(const linkg_path_probe_tx_task_t *task, uint64_t now_us);
void                             linkg_path_probe_finish_send_locked(const linkg_path_probe_tx_task_t *task, int status, uint64_t now_us);
void                             linkg_path_probe_accept_reply_locked(uint8_t peer_node_id, uint32_t link_id, linkg_transport_class_t traffic_class, uint32_t sequence, uint64_t now_us);
void                             linkg_path_probe_record_rx_locked(uint8_t peer_node_id, uint32_t link_id);
void                             linkg_path_probe_record_tx_locked(uint8_t peer_node_id, uint32_t link_id);
void                             linkg_path_probe_expire_locked(uint64_t now_us);
void                             linkg_path_probe_worker(linkg_thread_t *thread, void *user_data);

/****************************** 数据发送 ******************************/

int linkg_path_probe_send_request(linkg_packet_pool_t *pool, linkg_device_role_t role, uint8_t local_node_id, const linkg_path_probe_tx_task_t *task);
int linkg_path_probe_send_response(linkg_packet_pool_t *pool, linkg_device_role_t role, uint8_t local_node_id, uint8_t peer_node_id, uint32_t link_id, linkg_transport_class_t traffic_class, uint32_t sequence);

/****************************** Transport接收 ******************************/

bool linkg_path_probe_enter_receive(void *user_data, linkg_packet_pool_t **pool, linkg_device_role_t *role, uint8_t *local_node_id);
void linkg_path_probe_leave_receive(void);
int  linkg_path_probe_transport_receive(const linkg_transport_delivery_t *items, uint32_t count, void *user_data);

#endif
