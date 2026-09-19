/**
 * @file switch_status.c
 * @brief LinkG链路切换运行状态快照实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-19
 */

#include "linkg_switch_status.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "linkg_time.h"

#include "switch_internal.h"
#include "switch_observation.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 复制单个Path Probe观测状态。
 */
static void _linkg_switch_status_copy_probe(linkg_switch_status_probe_t *destination, const linkg_switch_probe_observation_t *source)
{
    destination->valid      = source->valid;
    destination->reachable  = source->reachable;
    destination->rtt_us     = source->rtt_us;
    destination->updated_us = source->updated_us;
}

/**
 * @brief 复制Wi-Fi业务流量观测状态。
 */
static void _linkg_switch_status_copy_traffic(linkg_switch_status_traffic_t *destination, const linkg_switch_traffic_observation_t *source)
{
    destination->valid      = source->valid;
    destination->tx_pps     = source->tx_pps;
    destination->rx_pps     = source->rx_pps;
    destination->tx_bps     = source->tx_bps;
    destination->rx_bps     = source->rx_bps;
    destination->updated_us = source->updated_us;
}

/**
 * @brief 复制单个真实丢包观测状态。
 */
static void _linkg_switch_status_copy_loss(linkg_switch_status_loss_t *destination, const linkg_switch_loss_observation_t *source)
{
    destination->valid          = source->valid;
    destination->loss_permille  = source->loss_permille;
    destination->sample_packets = source->sample_packets;
    destination->updated_us     = source->updated_us;
}

/**
 * @brief 复制STA当前Wi-Fi完整观测状态。
 */
static void _linkg_switch_status_copy_sta_wifi(linkg_switch_sta_wifi_status_t *destination, const linkg_switch_wifi_observation_t *source)
{
    uint32_t index;

    destination->available             = source->available;
    destination->radio_valid           = source->radio.valid;
    destination->connected             = source->radio.connected;
    destination->statistics_valid      = source->radio.statistics_valid;
    destination->rssi_dbm              = source->radio.rssi_dbm;
    destination->noise_valid           = source->radio.noise_valid;
    destination->noise_dbm             = source->radio.noise_dbm;
    destination->tx_phy_kbps           = source->radio.tx_phy_kbps;
    destination->rx_phy_kbps           = source->radio.rx_phy_kbps;
    destination->inactive_ms           = source->radio.inactive_ms;
    destination->work_mode             = source->radio.work_mode;
    destination->bandwidth_mhz         = source->radio.bandwidth_mhz;
    destination->narrow_mode           = source->radio.narrow_mode;
    destination->rate_level_valid      = source->radio.rate_level_valid;
    destination->rate_level            = source->radio.rate_level;
    destination->temperature_valid     = source->radio.temperature_valid;
    destination->temperature_c         = source->radio.temperature_c;
    destination->status_updated_us     = source->radio.status_updated_us;
    destination->statistics_updated_us = source->radio.statistics_updated_us;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        _linkg_switch_status_copy_probe(&destination->probe[index], &source->probe[index]);
    }

    _linkg_switch_status_copy_traffic(&destination->traffic, &source->traffic);

    _linkg_switch_status_copy_loss(&destination->uplink_loss, &source->loss.uplink);

    _linkg_switch_status_copy_loss(&destination->downlink_loss, &source->loss.downlink);
}

/**
 * @brief 复制STA当前Cellular备用观测状态。
 */
static void _linkg_switch_status_copy_sta_cellular(linkg_switch_sta_cellular_status_t *destination, const linkg_switch_cellular_observation_t *source)
{
    destination->available  = source->available;
    destination->updated_us = source->updated_us;
}

/**
 * @brief 生成当前STA角色运行状态，调用方持有Switch锁。
 */
static void _linkg_switch_status_build_sta_locked(linkg_switch_sta_status_t *status)
{
    const linkg_switch_sta_peer_runtime_t *runtime;
    const linkg_switch_peer_runtime_t     *peer;
    uint32_t                               index;

    memset(status, 0, sizeof(*status));

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        runtime = &peer->role.sta;

        status->peer_present           = true;
        status->observation_valid      = runtime->observation.valid;
        status->peer_node_id           = peer->peer_node_id;
        status->plan                   = peer->plan;
        status->observation_updated_us = runtime->observation.collected_us;

        _linkg_switch_status_copy_sta_wifi(&status->wifi, &runtime->observation.wifi);

        _linkg_switch_status_copy_sta_cellular(&status->cellular, &runtime->observation.cellular);

        /**
         * Maintenance Runtime尚未接入当前Switch运行结构。
         * memset后的inactive状态代表当前没有已发布的Maintenance状态。
         */
        break;
    }
}

/**
 * @brief 生成单个AP直接STA状态，调用方持有Switch锁。
 */
static void _linkg_switch_status_build_ap_peer_locked(linkg_switch_ap_peer_status_t *status, const linkg_switch_peer_runtime_t *peer)
{
    const linkg_switch_ap_peer_runtime_t *runtime;

    runtime = &peer->role.ap;

    memset(status, 0, sizeof(*status));

    status->peer_node_id            = peer->peer_node_id;
    status->plan                    = peer->plan;
    status->wifi_path_available     = runtime->wifi_path_available;
    status->cellular_path_available = runtime->cellular_path_available;
    status->path_updated_us         = runtime->path_updated_us;

    _linkg_switch_status_copy_loss(&status->wifi_uplink_loss, &runtime->wifi_uplink_loss);

    /**
     * remote_maintenance后续由Maintenance模块运行状态填充。
     * 当前保持inactive，不影响第一版Web状态查询。
     */
}

/**
 * @brief 生成当前AP角色运行状态，调用方持有Switch锁。
 */
static void _linkg_switch_status_build_ap_locked(linkg_switch_ap_status_t *status)
{
    const linkg_switch_peer_runtime_t *peer;
    uint32_t                           index;
    uint32_t                           peer_count;

    memset(status, 0, sizeof(*status));

    peer_count = 0U;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        peer = &g_switch.peers[index];

        if (!peer->used)
        {
            continue;
        }

        if (peer_count >= LINKG_RESOURCE_NETWORK_STA_MAX)
        {
            break;
        }

        _linkg_switch_status_build_ap_peer_locked(&status->peers[peer_count], peer);

        peer_count++;
    }

    status->peer_count = peer_count;

    /**
     * local_maintenance后续由Maintenance模块运行状态填充。
     * 当前保持inactive，不影响第一版Web状态查询。
     */
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取当前Switch模块只读运行状态快照。
 */
int linkg_switch_get_status(linkg_switch_status_t *status)
{
    uint64_t now_us;
    int      ret;

    if (status == NULL)
    {
        return -EINVAL;
    }

    memset(status, 0, sizeof(*status));

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    status->role = g_switch.role;

    if (g_switch.role == LINKG_DEVICE_ROLE_STA)
    {
        _linkg_switch_status_build_sta_locked(&status->data.sta);
        ret = 0;
    }
    else if (g_switch.role == LINKG_DEVICE_ROLE_AP)
    {
        _linkg_switch_status_build_ap_locked(&status->data.ap);
        ret = 0;
    }
    else
    {
        ret = -EINVAL;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (ret != 0)
    {
        memset(status, 0, sizeof(*status));
        return ret;
    }

    now_us = linkg_time_monotonic_us();

    status->collected_us = now_us;

    return 0;
}
