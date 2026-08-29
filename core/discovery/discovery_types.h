/**
 * @file discovery_types.h
 * @brief LinkG设备发现内部共享类型
 */

#ifndef DISCOVERY_TYPES_H
#define DISCOVERY_TYPES_H

#include <stdint.h>

#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_system_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 路径状态 ******************************/

#define LINKG_DISCOVERY_PATH_WIFI_VALID     (1U << 0) // Wi-Fi Endpoint有效
#define LINKG_DISCOVERY_PATH_CELLULAR_VALID (1U << 1) // Cellular Endpoint有效
#define LINKG_DISCOVERY_PATH_VALID_MASK     (LINKG_DISCOVERY_PATH_WIFI_VALID | LINKG_DISCOVERY_PATH_CELLULAR_VALID) // 有效路径标志掩码

/****************************** 设备状态 ******************************/

typedef struct
{
    uint64_t              session_id;        // 当前Discovery运行会话
    uint64_t              revision;          // 当前设备状态版本
    linkg_node_info_t     node;              // 节点逻辑身份
    linkg_path_endpoint_t wifi_endpoint;     // Wi-Fi数据面Endpoint
    linkg_path_endpoint_t cellular_endpoint; // Cellular数据面Endpoint
    uint8_t               path_flags;        // 当前有效Path标志
} linkg_discovery_report_t;

/****************************** AP同步 ******************************/

typedef struct
{
    linkg_discovery_report_t ap;                                             // AP自身完整状态
    uint64_t                 topology_revision;                              // 在线STA拓扑版本
    uint32_t                 node_count;                                     // 当前在线STA数量
    uint8_t                  node_ids[LINKG_RESOURCE_NETWORK_STA_MAX];       // 当前在线STA节点编号
} linkg_discovery_ap_sync_t;

/****************************** Peer注销 ******************************/

typedef struct
{
    uint64_t session_id; // 当前Discovery运行会话
    uint64_t revision;   // 注销对应状态版本
    uint8_t  node_id;    // 注销节点编号
} linkg_discovery_leave_t;

#ifdef __cplusplus
}
#endif

#endif
