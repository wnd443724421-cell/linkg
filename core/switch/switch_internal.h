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
#include "switch_observation.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_PEER_MAX                 LINKG_RESOURCE_NETWORK_STA_MAX // 最大直接Peer运行槽位数量
#define LINKG_SWITCH_OBSERVATION_INTERVAL_US  250000ULL                      // 默认观测快照刷新周期
#define LINKG_SWITCH_EVENT_QUEUE_CAPACITY     64U                            // Switch内部控制事件队列容量

/****************************** Peer运行状态 ******************************/

typedef struct
{
    linkg_switch_observation_t         observation;                 // 最近一次完整切换观测快照
    linkg_switch_traffic_sampler_t     wifi_traffic_sampler;        // Wi-Fi Path流量采样基线
    linkg_switch_loss_sampler_t        wifi_downlink_loss_sampler;  // AP到STA下行丢包采样基线
    linkg_switch_loss_observation_t    remote_wifi_uplink_loss;     // AP上报的STA到AP上行丢包
    uint32_t                           remote_wifi_uplink_report_id;// 最近接收的Wi-Fi上行质量报告编号
} linkg_switch_sta_peer_runtime_t;

typedef struct
{
    linkg_switch_loss_sampler_t wifi_uplink_loss_sampler; // STA到AP上行丢包采样基线
    uint32_t                    report_message_id;        // 最近分配的Wi-Fi质量报告编号
} linkg_switch_ap_peer_runtime_t;

typedef struct
{
    bool                     used;         // 当前Peer槽位是否已经使用
    uint8_t                  peer_node_id; // 当前直接Peer节点编号
    linkg_send_plan_t        plan;         // 当前唯一生效发送计划

    union
    {
        linkg_switch_sta_peer_runtime_t sta; // STA面对直接AP的切换运行状态
        linkg_switch_ap_peer_runtime_t  ap;  // AP面对直接STA的被动执行运行状态
    } role;
} linkg_switch_peer_runtime_t;


/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t             lock;                         // 模块状态锁，保护Peer运行状态和内部事件
    linkg_thread_t              thread;                       // Switch后台工作线程
    linkg_packet_pool_t        *packet_pool;                  // 外部Packet Pool，仅借用，不拥有生命周期
    linkg_switch_peer_runtime_t peers[LINKG_SWITCH_PEER_MAX]; // 全部直接Peer运行状态
    linkg_switch_event_queue_t  event_queue;                  // 待Worker处理的控制事件
    linkg_device_role_t         role;                         // 当前本机设备角色
    uint8_t                     local_node_id;                // 当前本机节点编号
    uint64_t                    next_observation_us;          // STA下一轮观测刷新时间
    uint64_t                    next_report_us;               // AP下一轮质量上报时间
    bool                        initialized;                  // 模块是否已经初始化
    bool                        running;                      // Switch工作线程是否正在运行
} linkg_switch_context_t;

/****************************** 全局上下文 ******************************/

extern linkg_switch_context_t g_switch;

/****************************** Peer管理 ******************************/

linkg_switch_peer_runtime_t *linkg_switch_find_peer_locked(uint8_t peer_node_id);

/****************************** Runtime ******************************/

void linkg_switch_worker(linkg_thread_t *thread, void *user_data);


/****************************** Transport接收 ******************************/

bool linkg_switch_enter_receive(void *user_data, linkg_device_role_t *role, uint8_t *local_node_id);
void linkg_switch_leave_receive(void);

#endif
