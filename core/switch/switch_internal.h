/**
 * @file switch_internal.h
 * @brief LinkG Switch内部运行状态及模块间共享接口
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
#include "linkg_packet_pool.h"
#include "linkg_switch.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"

#include "switch_event.h"
#include "switch_maintenance.h"
#include "switch_observation.h"
#include "switch_plan.h"
#include "switch_policy_load.h"
#include "switch_policy_loss.h"
#include "switch_policy_state.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_THREAD_NAME                "linkg-switch"                 // Switch后台Worker线程名称
#define LINKG_SWITCH_PEER_MAX                   LINKG_RESOURCE_NETWORK_STA_MAX // 直接Peer运行槽位容量，由系统资源定义
#define LINKG_SWITCH_PEER_GENERATION_INVALID    0U                             // 无效Peer运行代际
#define LINKG_SWITCH_OBSERVATION_INTERVAL_US    250000ULL                      // STA观测快照刷新周期
#define LINKG_SWITCH_PLAN_RECONCILE_INTERVAL_US 500000ULL                      // AP/STA发送计划校准周期

/****************************** STA侧Peer运行状态 ******************************/

typedef struct
{
    // 本地链路观测
    linkg_switch_observation_t          observation;                                                  // 最近一次完整链路观测快照

    // 本地Wi-Fi流量采样
    linkg_switch_traffic_sampler_t      wifi_traffic_sampler;                                         // Wi-Fi业务流量差分采样基线

    // 本地Wi-Fi质量采样
    linkg_switch_loss_sampler_t         wifi_downlink_loss_sampler;                                   // AP到STA Wi-Fi Wire丢包采样基线
    linkg_switch_load_sampler_t         wifi_uplink_load_sampler;                                     // STA到AP Wi-Fi Load丢包采样基线

    // 本地Cellular质量采样
    linkg_switch_loss_sampler_t         cellular_downlink_loss_sampler;                               // AP到STA Cellular Wire丢包采样基线
    linkg_switch_load_sampler_t         cellular_uplink_load_sampler;                                 // STA到AP Cellular Load丢包采样基线

    // AP上报的Wi-Fi质量数据
    linkg_switch_loss_observation_t     remote_wifi_uplink_loss;                                      // STA到AP Wi-Fi整链路Wire丢包
    linkg_switch_loss_observation_t     remote_wifi_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];      // STA到AP Wi-Fi三业务Wire丢包
    linkg_switch_loss_observation_t     remote_wifi_downlink_load;                                    // AP到STA Wi-Fi整链路Load丢包

    // AP上报的Cellular质量数据
    linkg_switch_loss_observation_t     remote_cellular_uplink_loss;                                  // STA到AP Cellular整链路Wire丢包
    linkg_switch_loss_observation_t     remote_cellular_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];  // STA到AP Cellular三业务Wire丢包
    linkg_switch_loss_observation_t     remote_cellular_downlink_load;                                // AP到STA Cellular整链路Load丢包

    // Quality Report接收状态
    uint32_t                            remote_quality_report_id;                                     // 最近接收的AP质量报告编号

    // Plan同步状态
    linkg_switch_plan_sync_runtime_t    plan_sync;                                                    // STA向AP同步完整三类发送计划的事务

    // Wi-Fi质量异常检测
    linkg_switch_loss_policy_runtime_t  wifi_loss_policy;                                             // Wi-Fi双向丢包共享异常积分
    linkg_switch_load_policy_runtime_t  wifi_load_policy;                                             // Wi-Fi队列溢出检测器

    // 三类业务切换事务
    linkg_switch_policy_runtime_t       policy;                                                       // 三类业务独立切换事务及运行状态
} linkg_switch_sta_peer_runtime_t;


/****************************** AP侧Peer运行状态 ******************************/

typedef struct
{
    // Wi-Fi质量上报采样
    linkg_switch_loss_sampler_t     wifi_uplink_loss_sampler;      // STA到AP Wi-Fi Wire丢包采样基线
    linkg_switch_load_sampler_t     wifi_downlink_load_sampler;    // AP到STA Wi-Fi Load丢包采样基线

    // Cellular质量上报采样
    linkg_switch_loss_sampler_t     cellular_uplink_loss_sampler;  // STA到AP Cellular Wire丢包采样基线
    linkg_switch_load_sampler_t     cellular_downlink_load_sampler;// AP到STA Cellular Load丢包采样基线

    // Path状态快照
    bool                            wifi_path_available;           // 最近采集的Wi-Fi Peer Path可用状态
    bool                            cellular_path_available;       // 最近采集的Cellular Peer Path可用状态
    uint64_t                        path_updated_us;               // 最近一次Path状态采集时间

    // 本机状态查询
    linkg_switch_loss_observation_t wifi_uplink_loss;              // 最近一次STA到AP Wi-Fi Wire丢包

    // Quality Report发送状态
    uint32_t                        report_message_id;             // 当前Peer的Quality Report消息编号

    // Plan同步接收状态
    uint32_t                        remote_plan_message_id;        // 最近处理的STA PLAN_SYNC消息编号
    int32_t                         remote_plan_status;            // 最近PLAN_SYNC执行结果，供重复消息重发ACK
} linkg_switch_ap_peer_runtime_t;


/****************************** Peer公共运行状态 ******************************/

typedef struct
{
    // Peer身份与生命周期
    bool                                    used;         // 当前Peer槽位是否已分配
    uint8_t                                 peer_node_id; // 当前直接Peer节点编号
    uint32_t                                generation;   // Peer运行代际，过滤旧事件和回调

    // Peer公共业务状态
    linkg_send_plan_t                       plan;        // 当前本地生效的三类业务发送计划
    linkg_switch_maintenance_peer_runtime_t maintenance; // 当前Peer的Maintenance运行状态

    // AP/STA专属运行状态
    union
    {
        linkg_switch_sta_peer_runtime_t sta; // STA专属运行状态
        linkg_switch_ap_peer_runtime_t  ap;  // AP专属运行状态
    } role;
} linkg_switch_peer_runtime_t;


/****************************** Switch全局运行上下文 ******************************/

typedef struct
{
    // 全局同步
    pthread_mutex_t                          lock;                         // Switch共享状态锁
    pthread_cond_t                           rx_condition;                 // 等待已进入的Transport回调退出

    // Worker线程
    linkg_thread_t                           thread;                       // 唯一Switch Worker线程对象
    pthread_t                                worker_tid;                   // 当前Worker线程标识
    bool                                     worker_tid_valid;             // 当前Worker线程标识是否有效

    // 全局资源与设备身份
    linkg_packet_pool_t                     *packet_pool;                  // 外部Packet Pool，仅借用，不管理其生命周期
    linkg_device_role_t                      role;                         // 本机AP或STA角色
    uint8_t                                  local_node_id;                // 本机节点编号

    // Peer与事件队列
    linkg_switch_peer_runtime_t              peers[LINKG_SWITCH_PEER_MAX]; // 全部直接Peer运行状态
    linkg_switch_event_queue_t               event_queue;                  // 等待Worker处理的Switch控制事件

    // Maintenance运行状态
    linkg_switch_maintenance_local_runtime_t local_maintenance;            // 本机正在维护的Access，约束所有Peer

    // Transport接收生命周期
    uintptr_t                                run_token;                    // Transport接收Handler运行代际，过滤旧回调
    uint32_t                                 rx_users;                     // 正在执行的Transport接收回调数
    bool                                     handler_registered;           // SWITCH Transport接收Handler是否已注册

    // 全局编号分配
    uint32_t                                 next_peer_generation;         // 进程内Peer代际分配基线，Peer重建不回退
    uint32_t                                 next_plan_message_id;         // 进程内STA PLAN_SYNC编号分配基线，Peer重建不回退
    uint32_t                                 next_maintenance_message_id;  // 进程内本机Maintenance事务编号分配基线
    uint32_t                                 next_plan_sync_id;            // 进程内逻辑Plan同步编号分配基线，Peer重建不回退
    
    // Worker周期调度
    uint64_t                                 next_observation_us;          // STA下一轮观测快照刷新时间
    uint64_t                                 next_reconcile_us;            // AP/STA下一轮发送计划校准时间
    uint64_t                                 next_report_us;               // AP下一轮质量上报时间
    uint64_t                                 next_policy_us;               // STA下一轮切换策略处理时间
    bool                                     reconcile_pending;            // AP/STA是否存在待处理的立即发送计划校准请求

    // Switch生命周期
    bool                                     initialized;                  // Switch模块是否完成初始化
    bool                                     running;                      // Switch当前是否处于运行期
} linkg_switch_context_t;

/****************************** 全局上下文 ******************************/

extern linkg_switch_context_t g_switch;

/****************************** Peer查找 ******************************/

linkg_switch_peer_runtime_t *linkg_switch_find_peer_locked(uint8_t peer_node_id);
linkg_switch_peer_runtime_t *linkg_switch_find_peer_generation_locked(uint8_t peer_node_id, uint32_t generation);
bool                         linkg_switch_peer_generation_current(uint8_t peer_node_id, uint32_t generation);

/****************************** Worker入口 ******************************/

void linkg_switch_worker(linkg_thread_t *thread, void *user_data);

/****************************** Transport接收生命周期 ******************************/

bool linkg_switch_enter_receive(void *user_data, linkg_device_role_t *role, uint8_t *local_node_id);
void linkg_switch_leave_receive(void);

#endif
