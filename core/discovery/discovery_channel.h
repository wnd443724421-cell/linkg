/**
 * @file discovery_channel.h
 * @brief LinkG设备发现Channel内部接口
 */

#ifndef DISCOVERY_CHANNEL_H
#define DISCOVERY_CHANNEL_H

#include <stdint.h>

#include "linkg_link.h"

#include "discovery_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 类型定义 ******************************/

typedef int (*linkg_discovery_channel_send_leave_func_t)(const linkg_discovery_leave_t *leave, void *user_data);

/****************************** 通道管理 ******************************/

int linkg_discovery_channel_register(linkg_link_access_t access, linkg_discovery_channel_send_leave_func_t send_leave, void *user_data);
int linkg_discovery_channel_unregister(linkg_link_access_t access, uint64_t now_us);

/****************************** 本机状态 ******************************/

int linkg_discovery_channel_get_local_report(linkg_discovery_report_t *report);
int linkg_discovery_channel_build_ap_sync(linkg_discovery_ap_sync_t *sync);

/****************************** Peer状态 ******************************/

int linkg_discovery_channel_handle_peer_report(linkg_link_access_t access, const linkg_discovery_report_t *report, uint64_t now_us);
int linkg_discovery_channel_handle_peer_leave(const linkg_discovery_leave_t *leave, uint64_t now_us);
int linkg_discovery_channel_age_peers(uint64_t now_us);

/****************************** AP同步 ******************************/

int linkg_discovery_channel_handle_ap_sync(linkg_link_access_t access, const linkg_discovery_ap_sync_t *sync, uint64_t now_us);

/****************************** Cellular目标 ******************************/

int linkg_discovery_channel_get_cellular_targets(linkg_path_endpoint_t *targets, uint32_t capacity, uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif
