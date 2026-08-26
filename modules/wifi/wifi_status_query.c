/**
 * @file wifi_status_query.c
 * @brief LinkG Wi-Fi状态快照查询辅助实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#include "wifi_status_query.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

/****************************** 时间辅助 ******************************/

/**
 * @brief 判断时间戳是否仍在允许的有效时间内。
 */
static bool _wifi_status_timestamp_is_fresh(uint64_t updated_ms, uint64_t now_ms, uint64_t max_age_ms)
{
    if (updated_ms == 0U || now_ms < updated_ms)
    {
        return false;
    }

    return now_ms - updated_ms <= max_age_ms;
}

/****************************** 对端查询 ******************************/

/**
 * @brief 根据MAC地址获取AP模式下的对端STA状态。
 */
int wifi_status_query_ap_peer(const linkg_wifi_status_snapshot_t *snapshot, const uint8_t mac[LINKG_WIFI_MAC_LENGTH], linkg_wifi_peer_status_t *peer)
{
    const linkg_wifi_peer_status_t *current_peer;
    size_t                          peer_count;
    size_t                          index;

    if (snapshot == NULL || mac == NULL || peer == NULL)
    {
        return -EINVAL;
    }

    if (snapshot->local.role != LINKG_DEVICE_ROLE_AP)
    {
        return -EINVAL;
    }

    peer_count = snapshot->role.ap.peer_count;
    if (peer_count > LINKG_WIFI_AP_PEER_MAX)
    {
        return -EPROTO;
    }

    for (index = 0U; index < peer_count; index++)
    {
        current_peer = &snapshot->role.ap.peers[index];

        if (!current_peer->valid)
        {
            continue;
        }

        if (memcmp(current_peer->mac, mac, LINKG_WIFI_MAC_LENGTH) != 0)
        {
            continue;
        }

        *peer = *current_peer;

        return 0;
    }

    return -ENOENT;
}

/**
 * @brief 获取STA模式下当前或最近连接的AP状态。
 *
 * @note 返回的AP状态可能已经断开，调用方应通过state判断当前连接状态。
 */
int wifi_status_query_sta_peer(const linkg_wifi_status_snapshot_t *snapshot, linkg_wifi_peer_status_t *peer)
{
    if (snapshot == NULL || peer == NULL)
    {
        return -EINVAL;
    }

    if (snapshot->local.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    if (!snapshot->role.sta.peer.valid)
    {
        return -ENOENT;
    }

    *peer = snapshot->role.sta.peer;

    return 0;
}

/****************************** 状态判断 ******************************/

/**
 * @brief 判断对端当前是否处于连接状态。
 */
bool wifi_status_peer_is_connected(const linkg_wifi_peer_status_t *peer)
{
    return peer != NULL &&
           peer->valid &&
           peer->state == LINKG_WIFI_PEER_STATE_CONNECTED;
}

/**
 * @brief 判断Wi-Fi状态快照是否仍在允许的有效时间内。
 */
bool wifi_status_snapshot_is_fresh(const linkg_wifi_status_snapshot_t *snapshot, uint64_t now_ms, uint64_t max_age_ms)
{
    if (snapshot == NULL)
    {
        return false;
    }

    return _wifi_status_timestamp_is_fresh(snapshot->local.updated_ms, now_ms, max_age_ms);
}

/**
 * @brief 判断对端详细统计是否有效且未过期。
 */
bool wifi_status_peer_statistics_are_fresh(const linkg_wifi_peer_status_t *peer, uint64_t now_ms, uint64_t max_age_ms)
{
    if (peer == NULL || !peer->valid || !peer->statistics_valid)
    {
        return false;
    }

    return _wifi_status_timestamp_is_fresh(peer->statistics_updated_ms, now_ms, max_age_ms);
}

/****************************** 无线参数查询 ******************************/

/**
 * @brief 获取状态快照中的当前工作频率。
 */
int wifi_status_get_frequency_mhz(const linkg_wifi_status_snapshot_t *snapshot, uint32_t *frequency_mhz)
{
    if (snapshot == NULL || frequency_mhz == NULL)
    {
        return -EINVAL;
    }

    if (snapshot->local.updated_ms == 0U || snapshot->local.radio.frequency_mhz == 0U)
    {
        return -EAGAIN;
    }

    *frequency_mhz = snapshot->local.radio.frequency_mhz;

    return 0;
}
