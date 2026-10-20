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

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_THREAD_NAME                "linkg-switch"                 // Switch后台Worker线程名称
#define LINKG_SWITCH_PEER_MAX                   LINKG_RESOURCE_NETWORK_STA_MAX // 直接Peer运行槽位容量，由系统资源定义
#define LINKG_SWITCH_PEER_GENERATION_INVALID    0U                             // 无效Peer运行代际
#define LINKG_SWITCH_OBSERVATION_INTERVAL_US    250000ULL                      // STA观测快照刷新周期
#define LINKG_SWITCH_PLAN_RECONCILE_INTERVAL_US 500000ULL                      // AP/STA发送计划校准周期

#define LINKG_SWITCH_DUAL_CLASS_BIT(traffic_class) (1U << (uint32_t)(traffic_class))

/****************************** Wi-Fi Loss Policy运行状态 ******************************/

typedef struct
{
    uint64_t last_uplink_updated_us;   // 最近处理的上行观测时间
    uint64_t last_downlink_updated_us; // 最近处理的下行观测时间
    uint64_t last_valid_us;            // 最近一次有效样本判断时间
    uint32_t wifi_link_id;             // 当前积分归属的Wi-Fi Link
    uint8_t  score;                    // 上下行融合后的异常积分
    bool     initialized;              // 积分状态是否已经初始化
} linkg_switch_loss_policy_runtime_t;

/****************************** Load积分状态 ******************************/

typedef struct
{
    linkg_send_plan_t plan;                     // 上一次判决使用的完整发送计划
    uint64_t          last_uplink_updated_us;   // 上行最近消费的窗口时间戳
    uint64_t          last_downlink_updated_us; // 下行最近消费的窗口时间戳
    uint64_t          last_valid_us;            // 最近有效观测时间
    uint32_t          wifi_link_id;             // 当前积分对应的Wi-Fi Link
    uint8_t           score;                    // 当前共享积分，0~3
    bool              initialized;              // 检测器是否初始化
    bool              waiting_sync;             // 是否处于Plan同步过渡期
} linkg_switch_load_policy_runtime_t;

/****************************** Wi-Fi冗余事务 ******************************/

typedef enum
{
    LINKG_SWITCH_DUAL_REASON_NONE = 0, // 当前无策略冗余事务
    LINKG_SWITCH_DUAL_REASON_LOSS,     // Wi-Fi丢包触发的质量比较
    LINKG_SWITCH_DUAL_REASON_LOAD      // 队列溢出触发的定时迁移
} linkg_switch_dual_reason_t;

/****************************** Wi-Fi业务冗余状态 ******************************/

typedef struct
{
    linkg_switch_dual_reason_t reason;      // 当前业务进入冗余的类型
    uint64_t                   started_us;  // 当前业务确认进入冗余的时间
    uint64_t                   deadline_us; // 当前业务冗余事务截止时间
} linkg_switch_dual_class_runtime_t;

/****************************** Wi-Fi冗余事务 ******************************/

typedef struct
{
    linkg_switch_dual_class_runtime_t classes[LINKG_TRANSPORT_CLASS_COUNT]; // 三类业务独立冗余状态
} linkg_switch_dual_runtime_t;

/****************************** STA侧Peer运行状态 ******************************/

typedef struct
{
    linkg_switch_observation_t       observation;                       // 最近一次完整链路观测快照
    linkg_switch_traffic_sampler_t   wifi_traffic_sampler;              // Wi-Fi业务流量差分采样基线

    linkg_switch_loss_sampler_t      wifi_downlink_loss_sampler;        // AP到STA Wi-Fi Wire丢包采样基线
    linkg_switch_load_sampler_t      wifi_uplink_load_sampler;          // STA到AP Wi-Fi Load丢包采样基线
    linkg_switch_loss_sampler_t      cellular_downlink_loss_sampler;    // AP到STA Cellular Wire丢包采样基线
    linkg_switch_load_sampler_t      cellular_uplink_load_sampler;      // STA到AP Cellular Load丢包采样基线

    linkg_switch_loss_observation_t  remote_wifi_uplink_loss;                                    // AP上报的STA到AP Wi-Fi整链路Wire丢包
    linkg_switch_loss_observation_t  remote_wifi_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];    // AP上报的STA到AP Wi-Fi三业务Wire丢包
    linkg_switch_loss_observation_t  remote_wifi_downlink_load;                                  // AP上报的AP到STA Wi-Fi整链路Load丢包
    linkg_switch_loss_observation_t  remote_cellular_uplink_loss;                                // AP上报的STA到AP Cellular整链路Wire丢包
    linkg_switch_loss_observation_t  remote_cellular_uplink_classes[LINKG_TRANSPORT_CLASS_COUNT];// AP上报的STA到AP Cellular三业务Wire丢包
    linkg_switch_loss_observation_t  remote_cellular_downlink_load;                              // AP上报的AP到STA Cellular整链路Load丢包

    linkg_switch_plan_sync_runtime_t plan_sync;                         // 当前STA向AP同步完整三类发送计划的事务
    uint32_t                         remote_quality_report_id;          // 最近接收的AP质量报告编号

    linkg_switch_loss_policy_runtime_t wifi_loss_policy;                // Wi-Fi双向丢包共享异常积分
    linkg_switch_load_policy_runtime_t wifi_load_policy;                // Wi-Fi队列溢出检测器
    linkg_switch_dual_runtime_t        wifi_dual;                       // 当前Wi-Fi策略冗余事务
} linkg_switch_sta_peer_runtime_t;

/****************************** AP侧Peer运行状态 ******************************/

typedef struct
{
    linkg_switch_loss_sampler_t     wifi_uplink_loss_sampler;          // STA到AP Wi-Fi Wire丢包采样基线
    linkg_switch_load_sampler_t     wifi_downlink_load_sampler;        // AP到STA Wi-Fi Load丢包采样基线
    linkg_switch_loss_sampler_t     cellular_uplink_loss_sampler;      // STA到AP Cellular Wire丢包采样基线
    linkg_switch_load_sampler_t     cellular_downlink_load_sampler;    // AP到STA Cellular Load丢包采样基线

    linkg_switch_loss_observation_t wifi_uplink_loss;                  // 最近一次STA到AP Wi-Fi Wire丢包
    linkg_switch_loss_observation_t wifi_downlink_load;                // 最近一次AP到STA Wi-Fi Load丢包
    linkg_switch_loss_observation_t cellular_uplink_loss;              // 最近一次STA到AP Cellular Wire丢包
    linkg_switch_loss_observation_t cellular_downlink_load;            // 最近一次AP到STA Cellular Load丢包

    bool                            wifi_path_available;               // 当前Peer的Wi-Fi业务Path是否可用
    bool                            cellular_path_available;           // 当前Peer的Cellular业务Path是否可用
    uint64_t                        path_updated_us;                   // 最近一次Path状态刷新时间

    uint32_t                        report_message_id;                 // 最近分配的Quality Report消息编号
    uint32_t                        remote_plan_message_id;            // 最近处理的STA PLAN_SYNC消息编号
    int32_t                         remote_plan_status;                // 上述PLAN_SYNC的处理结果，供重复消息重发ACK
} linkg_switch_ap_peer_runtime_t;

/****************************** Peer公共运行状态 ******************************/

typedef struct
{
    bool                                    used;          // 当前Peer槽位是否已分配
    uint8_t                                 peer_node_id;  // 当前直接Peer节点编号
    uint32_t                                generation;    // 当前Peer运行代际，区分同编号Peer的重新注册
    linkg_send_plan_t                       plan;          // 当前完整三类业务发送计划，一次整组更新
    linkg_switch_maintenance_peer_runtime_t maintenance;   // 当前Peer的远端Maintenance及本机通知事务状态

    union
    {
        linkg_switch_sta_peer_runtime_t sta;              // STA侧：观测采样和PLAN_SYNC事务状态
        linkg_switch_ap_peer_runtime_t  ap;               // AP侧：观测、Path和PLAN_SYNC接收状态
    } role;
} linkg_switch_peer_runtime_t;

/****************************** Switch全局运行上下文 ******************************/

typedef struct
{
    pthread_mutex_t                          lock;                         // Switch共享状态锁
    pthread_cond_t                           rx_condition;                 // 等待已进入的Transport回调退出
    linkg_thread_t                           thread;                       // 唯一Switch Worker线程对象
    pthread_t                                worker_tid;                   // 当前Worker线程标识
    linkg_packet_pool_t                     *packet_pool;                  // 外部Packet Pool，仅借用，不管理其生命周期

    linkg_switch_peer_runtime_t              peers[LINKG_SWITCH_PEER_MAX]; // 全部直接Peer运行状态
    linkg_switch_event_queue_t               event_queue;                  // 等待Worker处理的Switch控制事件
    linkg_switch_maintenance_local_runtime_t local_maintenance;            // 本机正在维护的Access，约束所有Peer

    linkg_device_role_t                      role;                         // 本机AP或STA角色
    uint8_t                                  local_node_id;                // 本机节点编号

    uintptr_t                                run_token;                    // Transport接收Handler运行代际，过滤旧回调
    uint32_t                                 rx_users;                     // 正在执行的Transport接收回调数
    uint32_t                                 next_peer_generation;         // 进程内Peer代际分配基线，Peer重建不回退
    uint32_t                                 next_plan_message_id;         // 进程内STA PLAN_SYNC编号分配基线，Peer重建不回退
    uint32_t                                 next_maintenance_message_id;  // 进程内本机Maintenance事务编号分配基线

    uint64_t                                 next_observation_us;          // STA下一轮观测快照刷新时间
    uint64_t                                 next_reconcile_us;            // AP/STA下一轮发送计划校准时间
    uint64_t                                 next_report_us;               // AP下一轮质量上报时间
    uint64_t                                 next_policy_us;               // STA下一轮切换策略处理时间
    bool                                     reconcile_pending;            // AP/STA是否存在待处理的立即发送计划校准请求

    bool                                     worker_tid_valid;             // 当前Worker线程标识是否有效
    bool                                     handler_registered;           // SWITCH Transport接收Handler是否已注册
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

/****************************** 冗余计划提交 ******************************/

int linkg_switch_plan_enter_dual_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, linkg_switch_dual_reason_t reason, uint64_t now_us);

#endif
