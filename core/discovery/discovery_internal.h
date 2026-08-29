/**
 * @file discovery_internal.h
 * @brief LinkG设备发现模块内部定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-14
 */

#ifndef DISCOVERY_INTERNAL_H
#define DISCOVERY_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_discovery.h"
#include "discovery_channel.h"
#include "linkg_link.h"
#include "linkg_system_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** Peer事件 ******************************/

#define LINKG_DISCOVERY_LIVENESS_TIMEOUT_US       10000000ULL // 路径存活超时时间，10s
#define LINKG_DISCOVERY_PEER_TOMBSTONE_TIMEOUT_US 15000000ULL // Peer离线记录保留时间，15s

typedef enum
{
    LINKG_DISCOVERY_PEER_EVENT_INVALID = 0,  // 无效事件
    LINKG_DISCOVERY_PEER_EVENT_NEW,          // 新Peer或离线Peer重新上线
    LINKG_DISCOVERY_PEER_EVENT_REFRESH,      // 同版本周期完整状态上报
    LINKG_DISCOVERY_PEER_EVENT_UPDATE,       // 同会话新版本状态更新
    LINKG_DISCOVERY_PEER_EVENT_STALE,        // 同会话旧版本状态上报
    LINKG_DISCOVERY_PEER_EVENT_RESTART       // Peer进入新Discovery会话
} linkg_discovery_peer_event_t;

/****************************** Peer状态 ******************************/

typedef struct
{
    bool     registered;   // 当前Access已经建立存活记录
    bool     active;       // 当前Access处于存活状态
    uint64_t last_seen_us; // 最近通过当前Access收到有效完整状态的时间
} linkg_discovery_liveness_t;

typedef struct
{
    bool                       used;                          // Peer状态槽位是否已使用
    bool                       online;                        // Peer当前是否在线
    bool                       route_cleanup_pending;         // Peer虚拟路由是否等待继续清理
    linkg_discovery_report_t   report;                        // 最近接受的完整Peer状态
    linkg_discovery_liveness_t liveness[LINKG_NODE_PATH_MAX]; // 各物理Access存活状态
    uint64_t                   offline_since_us;              // Peer进入离线状态时间
} linkg_discovery_peer_t;

/****************************** Channel状态 ******************************/

typedef struct
{
    bool                                registered; // Discovery Channel是否已经注册
    linkg_discovery_channel_send_func_t send;       // Wire报文发送函数
    void                               *user_data;  // Channel私有数据
} linkg_discovery_channel_state_t;

/****************************** 拓扑状态 ******************************/

typedef struct
{
    uint64_t ap_session_id;                                // 当前AP Discovery会话
    uint64_t revision;                                     // 最近应用的AP拓扑版本
    uint32_t node_count;                                   // 当前远端拓扑节点数量
    uint8_t  node_ids[LINKG_RESOURCE_NETWORK_STA_MAX];     // 当前远端在线STA节点编号
} linkg_discovery_topology_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t                 lock;                          // Discovery共享状态保护锁
    linkg_discovery_report_t        local_report;                  // 本机当前完整Discovery状态
    linkg_discovery_peer_t          peers[LINKG_NODE_PEER_MAX];    // 当前直接Peer状态
    linkg_discovery_channel_state_t channels[LINKG_NODE_PATH_MAX]; // Wi-Fi和Cellular Channel状态
    linkg_discovery_topology_t      topology;                      // STA当前应用的AP拓扑
    uint64_t                        topology_revision;             // AP本机在线拓扑版本
    uint32_t                        peer_count;                    // 当前在线直接Peer数量
    bool                            initialized;                   // 模块是否已经初始化
    bool                            running;                       // 模块是否正在运行
} linkg_discovery_context_t;

/****************************** 内部共享上下文 ******************************/

extern linkg_discovery_context_t g_discovery;

/****************************** 本机状态 ******************************/

bool _linkg_discovery_endpoint_equal(const linkg_path_endpoint_t *left, const linkg_path_endpoint_t *right);
int  _linkg_discovery_generate_session_id(uint64_t *session_id);
int  _linkg_discovery_build_local_report(linkg_discovery_report_t *report);
int  _linkg_discovery_prepare_local_endpoints(linkg_discovery_report_t *report);
int  _linkg_discovery_refresh_local_endpoints(void);
int  _linkg_discovery_build_local_leave_locked(linkg_discovery_leave_t *leave);

/****************************** 状态校验 ******************************/

int _linkg_discovery_validate_report(const linkg_discovery_report_t *report);
int _linkg_discovery_validate_peer_update(const linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report);
int _linkg_discovery_validate_ap_sync_locked(const linkg_discovery_ap_sync_t *sync);

/****************************** Peer状态 ******************************/

int _linkg_discovery_access_index(linkg_link_access_t access, uint32_t *index);
int _linkg_discovery_deactivate_access_locked(linkg_link_access_t access, uint64_t now_us);
int _linkg_discovery_handle_peer_report_locked(linkg_link_access_t access, const linkg_discovery_report_t *report, uint64_t now_us);
int _linkg_discovery_handle_peer_leave_locked(const linkg_discovery_leave_t *leave, uint64_t now_us);
int _linkg_discovery_age_peers_locked(uint64_t now_us);

/****************************** Peer运行资源 ******************************/

int _linkg_discovery_cleanup_peer_route_locked(linkg_discovery_peer_t *peer);
int _linkg_discovery_register_peer_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report);
int _linkg_discovery_update_peer_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report);
int _linkg_discovery_unregister_peer_locked(linkg_discovery_peer_t *peer, uint64_t now_us);

/****************************** Topology ******************************/

void _linkg_discovery_advance_topology_revision_locked(void);
bool _linkg_discovery_has_online_ap_locked(void);
int  _linkg_discovery_clear_topology_locked(void);
int  _linkg_discovery_build_ap_sync_locked(linkg_discovery_ap_sync_t *sync);
int  _linkg_discovery_handle_ap_sync_locked(linkg_link_access_t access, const linkg_discovery_ap_sync_t *sync, uint64_t now_us);

#ifdef __cplusplus
}
#endif

#endif
