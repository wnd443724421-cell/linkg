/**
 * @file wifi_status_query.h
 * @brief LinkG Wi-Fi状态快照查询辅助接口
 */

#ifndef WIFI_STATUS_QUERY_H
#define WIFI_STATUS_QUERY_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_wifi_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 对端查询 ******************************/

int wifi_status_query_ap_peer(const linkg_wifi_status_snapshot_t *snapshot, const uint8_t mac[LINKG_WIFI_MAC_LENGTH], linkg_wifi_peer_status_t *peer);
int wifi_status_query_sta_peer(const linkg_wifi_status_snapshot_t *snapshot, linkg_wifi_peer_status_t *peer);

/****************************** 状态判断 ******************************/

bool wifi_status_peer_is_connected(const linkg_wifi_peer_status_t *peer);
bool wifi_status_snapshot_is_fresh(const linkg_wifi_status_snapshot_t *snapshot, uint64_t now_ms, uint64_t max_age_ms);
bool wifi_status_peer_statistics_are_fresh(const linkg_wifi_peer_status_t *peer, uint64_t now_ms, uint64_t max_age_ms);

/****************************** 无线参数查询 ******************************/

int wifi_status_get_frequency_mhz(const linkg_wifi_status_snapshot_t *snapshot, uint32_t *frequency_mhz);

#ifdef __cplusplus
}
#endif

#endif
