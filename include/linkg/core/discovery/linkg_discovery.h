/**
 * @file linkg_discovery.h
 * @brief LinkG设备发现接口
 */

#ifndef LINKG_DISCOVERY_H
#define LINKG_DISCOVERY_H

#include <stdint.h>

#include "linkg_system_resources.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 拓扑快照 ******************************/

typedef struct
{
    uint64_t revision;                                 // 当前应用的AP拓扑版本
    uint32_t node_count;                               // 当前远端在线STA数量
    uint8_t  node_ids[LINKG_RESOURCE_NETWORK_STA_MAX]; // 当前远端在线STA节点编号
} linkg_discovery_topology_snapshot_t;

/****************************** 生命周期 ******************************/

int linkg_discovery_init(void);
int linkg_discovery_start(void);
int linkg_discovery_stop(void);
int linkg_discovery_deinit(void);

/****************************** 状态查询 ******************************/

int linkg_discovery_get_network_node_count(uint32_t *count);
int linkg_discovery_get_topology_snapshot(linkg_discovery_topology_snapshot_t *snapshot);

#ifdef __cplusplus
}
#endif

#endif
