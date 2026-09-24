/**
 * @file switch_internal.h
 * @brief LinkG链路切换内部运行定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#ifndef SWITCH_INTERNAL_H
#define SWITCH_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_switch.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_packet_pool.h"
#include "switch_event.h"
#include "switch_maintenance.h"
#include "switch_observation.h"
#include "switch_plan.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_THREAD_NAME              "linkg-switch"                     // Switch后台线程名称
#define LINKG_SWITCH_PEER_MAX                 LINKG_RESOURCE_NETWORK_STA_MAX     // 最大直接Peer运行槽位数量
#define LINKG_SWITCH_PEER_GENERATION_INVALID  0U                                 // 无效Peer运行代际
#define LINKG_SWITCH_OBSERVATION_INTERVAL_US  250000ULL                          // 默认观测快照刷新周期

/****************************** Peer运行状态 ******************************/

typedef struct
{
    linkg_switch_observation_t         observation;                  // 最近一次完整切换观测快照
    linkg_switch_traffic_sampler_t     wifi_traffic_sampler;         // Wi-Fi Path流量采样基线
    linkg_switch_loss_sampler_t        wifi_downlink_loss_sampler;   // AP到STA下行丢包采样基线
    linkg_switch_loss_observation_t    remote_wifi_uplink_loss;      // AP上报的STA到AP上行丢包
    linkg_switch_plan_sync_runtime_t   plan_sync;                    // 当前STA到AP发送计划同步事务
    uint32_t                           remote_wifi_uplink_report_id; // 最近接收的Wi-Fi上行质量报告编号
    uint32_t                           next_plan_message_id;         // 下一发送计划同步消息编号基线
} linkg_switch_sta_peer_runtime_t;

typedef struct
{
    linkg_switch_loss_sampler_t     wifi_uplink_loss_sampler; // STA到AP上行丢包采样基线
    linkg_switch_loss_observation_t wifi_uplink_loss;         // 最近一次STA到AP上行丢包观测
    bool                            wifi_path_available;      // 当前Wi-Fi Path是否存在且可用
    bool                            cellular_path_available;  // 当前Cellular Path是否存在且可用
    uint64_t                        path_updated_us;          // 最近一次Path状态刷新时间
    uint32_t                        report_message_id;        // 最近分配的Wi-Fi质量报告编号
    uint32_t                        remote_plan_message_id;   // 最近处理的STA发送计划同步编号
    int32_t                         remote_plan_status;       // 最近一次发送计划同步处理结果
} linkg_switch_ap_peer_runtime_t;

typedef struct
{
    bool                                    used;         // 当前Peer槽位是否已经使用
    uint8_t                                 peer_node_id; // 当前直接Peer节点编号
    uint32_t                                generation;   // 当前Peer运行代际
    linkg_send_plan_t                       plan;         // 当前唯一生效发送计划
    linkg_switch_maintenance_peer_runtime_t maintenance;  // 当前Peer Maintenance运行状态

    union
    {
        linkg_switch_sta_peer_runtime_t sta; // STA面对直接AP的切换运行状态
        linkg_switch_ap_peer_runtime_t  ap;  // AP面对直接STA的被动执行运行状态
    } role;
} linkg_switch_peer_runtime_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t                          lock;                         // 模块状态锁，保护全部Switch共享运行状态
    pthread_cond_t                           rx_condition;                 // 等待已进入Transport回调全部退出
    linkg_thread_t                           thread;                       // Switch后台工作线程
    pthread_t                                worker_tid;                   // 当前Worker线程标识
    linkg_packet_pool_t                     *packet_pool;                  // 外部Packet Pool，仅借用，不拥有生命周期
    linkg_switch_peer_runtime_t              peers[LINKG_SWITCH_PEER_MAX]; // 全部直接Peer运行状态
    linkg_switch_event_queue_t               event_queue;                  // 待Worker处理的内部控制事件
    linkg_switch_maintenance_local_runtime_t local_maintenance;            // 当前本机Access Maintenance运行状态
    linkg_device_role_t                      role;                         // 当前本机设备角色
    uint8_t                                  local_node_id;                // 当前本机节点编号
    uintptr_t                                run_token;                    // 当前Transport Handler运行代际
    uint32_t                                 rx_users;                     // 当前正在执行的Transport回调数量
    uint32_t                                 next_peer_generation;         // 下一Peer运行代际编号基线
    uint32_t                                 next_maintenance_message_id;  // 下一Maintenance事务消息编号基线
    uint64_t                                 next_observation_us;          // STA下一轮观测刷新时间
    uint64_t                                 next_report_us;               // AP下一轮质量上报时间
    uint64_t                                 next_policy_us;               // STA下一轮切换策略检查时间
    bool                                     policy_check_pending;         // 是否存在待执行的立即策略检查
    bool                                     worker_tid_valid;             // Worker线程标识当前是否有效
    bool                                     handler_registered;           // SWITCH Transport Handler是否已注册
    bool                                     initialized;                  // 模块是否已经初始化
    bool                                     running;                      // Switch是否正在运行
} linkg_switch_context_t;

/****************************** 全局上下文 ******************************/

extern linkg_switch_context_t g_switch;

/****************************** Peer管理 ******************************/

linkg_switch_peer_runtime_t *linkg_switch_find_peer_locked(uint8_t peer_node_id);
linkg_switch_peer_runtime_t *linkg_switch_find_peer_generation_locked(uint8_t peer_node_id, uint32_t generation);
bool                         linkg_switch_peer_generation_current(uint8_t peer_node_id, uint32_t generation);

/****************************** Runtime ******************************/

void linkg_switch_worker(linkg_thread_t *thread, void *user_data);


/****************************** Transport接收 ******************************/

bool linkg_switch_enter_receive(void *user_data, linkg_device_role_t *role, uint8_t *local_node_id);
void linkg_switch_leave_receive(void);

#endif
