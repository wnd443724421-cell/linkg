/**
 * @file switch_observation.c
 * @brief LinkG链路切换观测快照实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#include "switch_observation.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular.h"
#include "linkg_config.h"
#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_path_probe.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_wifi.h"
#include "linkg_wifi_link.h"

#include "switch_internal.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_wifi_status_snapshot_t     wifi_status;                 // Wi-Fi运行状态快照
    linkg_wifi_config_t              wifi_config;                 // Wi-Fi当前配置
    linkg_path_probe_peer_snapshot_t probe;                       // Peer周期Probe快照
    linkg_path_stats_t               wifi_path_stats;             // Wi-Fi Path累计统计
    linkg_wifi_rx_stats_t            wifi_rx_stats;               // Wi-Fi链路累计接收丢包统计
    uint32_t                         wifi_link_id;                // 当前Wi-Fi Link实例
    uint32_t                         cellular_link_id;            // 当前Cellular Link实例
    uint64_t                         elapsed_ms;                  // 本轮程序运行时间
    bool                             wifi_status_valid;           // Wi-Fi状态快照是否有效
    bool                             wifi_config_valid;           // Wi-Fi配置是否有效
    bool                             probe_valid;                 // Probe快照是否有效
    bool                             wifi_path_available;         // 当前Peer Wi-Fi Path是否存在且Link运行
    bool                             wifi_path_stats_valid;       // Wi-Fi Path累计统计是否有效
    bool                             wifi_rx_stats_valid;         // Wi-Fi接收累计统计是否有效
    bool                             cellular_internet_available; // Cellular公网状态是否可用
    bool                             cellular_path_available;     // 当前Peer Cellular Path是否存在且Link运行
} linkg_switch_observation_raw_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断外部运行状态暂不可用是否属于正常状态变化。
 */
static bool _linkg_switch_observation_expected_state_error(int error)
{
    return error == -ENOENT || error == -ENODEV || error == -ENETDOWN || error == -EAGAIN || error == -ESTALE;
}

/**
 * @brief 记录本轮第一个非预期采集错误。
 */
static void _linkg_switch_observation_record_error(int error, int *first_error)
{
    if (first_error == NULL || error == 0 || _linkg_switch_observation_expected_state_error(error))
    {
        return;
    }

    if (*first_error == 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 将64位数值饱和转换为32位无符号整数。
 */
static uint32_t _linkg_switch_observation_u32_saturate(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/**
 * @brief 计算带比例系数的无符号整数比值，并避免中间乘法溢出。
 */
static uint64_t _linkg_switch_observation_scale_ratio(uint64_t numerator, uint64_t scale, uint64_t denominator)
{
    uint64_t quotient;
    uint64_t remainder;
    uint64_t value;
    uint64_t scaled_remainder;

    if (denominator == 0U)
    {
        return 0U;
    }

    quotient  = numerator / denominator;
    remainder = numerator % denominator;

    if (quotient > UINT64_MAX / scale)
    {
        return UINT64_MAX;
    }

    value = quotient * scale;

    if (remainder == 0U)
    {
        return value;
    }

    if (remainder > UINT64_MAX / scale)
    {
        return UINT64_MAX;
    }

    scaled_remainder = (remainder * scale) / denominator;

    if (UINT64_MAX - value < scaled_remainder)
    {
        return UINT64_MAX;
    }

    return value + scaled_remainder;
}

/**
 * @brief 将linkg_time_elapsed_ms时间戳转换为本轮CLOCK_MONOTONIC微秒时间基准。
 */
static uint64_t _linkg_switch_observation_elapsed_ms_to_monotonic_us(uint64_t updated_ms, uint64_t now_us, uint64_t elapsed_ms)
{
    uint64_t age_ms;
    uint64_t age_us;

    if (updated_ms == 0U || updated_ms > elapsed_ms)
    {
        return 0U;
    }

    age_ms = elapsed_ms - updated_ms;
    if (age_ms > UINT64_MAX / 1000ULL)
    {
        return 0U;
    }

    age_us = age_ms * 1000ULL;

    return now_us >= age_us ? now_us - age_us : 0U;
}

/**
 * @brief 获取指定Peer在指定Link上的活动Path并可选读取累计统计。
 */
static int _linkg_switch_observation_collect_path(uint8_t peer_node_id, uint32_t link_id, bool read_stats, bool *available, linkg_path_stats_t *stats)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    int                   ret;

    if (available == NULL || (read_stats && stats == NULL))
    {
        return -EINVAL;
    }

    *available = false;

    if (stats != NULL)
    {
        memset(stats, 0, sizeof(*stats));
    }

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    link = linkg_link_manager_get(link_id);
    if (link == NULL || !linkg_link_is_running(link))
    {
        return 0;
    }

    memset(&endpoint, 0, sizeof(endpoint));
    path = NULL;

    ret = linkg_node_acquire_path(peer_node_id, link_id, &path, &endpoint);
    if (_linkg_switch_observation_expected_state_error(ret))
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    *available = true;

    if (read_stats)
    {
        ret = linkg_path_get_stats(path, stats);
    }
    else
    {
        ret = 0;
    }

    linkg_path_release(path);

    return ret;
}

/**
 * @brief 读取Wi-Fi运行状态和配置快照。
 */
static void _linkg_switch_observation_collect_wifi_status(linkg_switch_observation_raw_t *raw, int *first_error)
{
    int ret;

    ret = linkg_wifi_get_status(&raw->wifi_status);
    if (ret == 0)
    {
        raw->wifi_status_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }

    ret = linkg_config_get_wifi(&raw->wifi_config);
    if (ret == 0)
    {
        raw->wifi_config_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }
}

/**
 * @brief 读取当前Peer周期Probe快照。
 */
static void _linkg_switch_observation_collect_probe(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw, int *first_error)
{
    int ret;

    ret = linkg_path_probe_get_peer_snapshot(peer_node_id, &raw->probe);
    if (ret == 0)
    {
        raw->probe_valid = true;
        return;
    }

    _linkg_switch_observation_record_error(ret, first_error);
}

/**
 * @brief 读取当前Peer Wi-Fi Path、累计流量及本机接收丢包统计。
 */
static void _linkg_switch_observation_collect_wifi_path(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw, int *first_error)
{
    linkg_link_t *wifi_link;
    int           ret;

    raw->wifi_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);

    ret = _linkg_switch_observation_collect_path(peer_node_id,
                                                  raw->wifi_link_id,
                                                  true,
                                                  &raw->wifi_path_available,
                                                  &raw->wifi_path_stats);
    if (ret == 0 && raw->wifi_path_available)
    {
        raw->wifi_path_stats_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }

    if (!raw->wifi_path_available || raw->wifi_link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    wifi_link = linkg_link_manager_get(raw->wifi_link_id);
    if (wifi_link == NULL || !linkg_link_is_running(wifi_link))
    {
        return;
    }

    ret = linkg_wifi_link_get_rx_stats(wifi_link, peer_node_id, &raw->wifi_rx_stats);
    if (ret == 0)
    {
        raw->wifi_rx_stats_valid = true;
        return;
    }

    _linkg_switch_observation_record_error(ret, first_error);
}

/**
 * @brief 读取Cellular公网可用状态及当前Peer Cellular Path。
 */
static void _linkg_switch_observation_collect_cellular(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw, int *first_error)
{
    bool internet_available;
    int  ret;

    internet_available = false;

    ret = linkg_cellular_get_internet_available(&internet_available);
    if (ret == 0)
    {
        raw->cellular_internet_available = internet_available;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }

    raw->cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    ret = _linkg_switch_observation_collect_path(peer_node_id,
                                                  raw->cellular_link_id,
                                                  false,
                                                  &raw->cellular_path_available,
                                                  NULL);
    _linkg_switch_observation_record_error(ret, first_error);
}

/**
 * @brief 从Wi-Fi状态快照构造STA无线观测。
 */
static void _linkg_switch_observation_build_wifi_radio(const linkg_switch_observation_raw_t *raw, uint64_t now_us, linkg_switch_wifi_radio_observation_t *radio)
{
    const linkg_wifi_peer_status_t *peer;

    memset(radio, 0, sizeof(*radio));
    radio->narrow_mode = LINKG_WIFI_NARROW_MODE_UNKNOWN;

    if (!raw->wifi_status_valid || raw->wifi_status.local.role != LINKG_DEVICE_ROLE_STA)
    {
        return;
    }

    peer = &raw->wifi_status.role.sta.peer;

    radio->valid             = true;
    radio->connected         = raw->wifi_status.local.interface_state == LINKG_WIFI_INTERFACE_STATE_CONNECTED && peer->valid && peer->state == LINKG_WIFI_PEER_STATE_CONNECTED;
    radio->statistics_valid  = peer->valid && peer->statistics_valid;
    radio->noise_valid       = raw->wifi_status.local.radio.noise_valid;
    radio->work_mode         = raw->wifi_status.local.radio.work_mode;
    radio->temperature_valid = raw->wifi_status.local.chip_temperature_valid;
    radio->status_updated_us = _linkg_switch_observation_elapsed_ms_to_monotonic_us(raw->wifi_status.local.updated_ms, now_us, raw->elapsed_ms);

    if (radio->statistics_valid)
    {
        radio->rssi_dbm              = peer->rssi_dbm;
        radio->tx_phy_kbps           = peer->tx_rate_kbps;
        radio->rx_phy_kbps           = peer->rx_rate_kbps;
        radio->inactive_ms           = peer->inactive_ms;
        radio->statistics_updated_us = _linkg_switch_observation_elapsed_ms_to_monotonic_us(peer->statistics_updated_ms, now_us, raw->elapsed_ms);
    }

    if (radio->noise_valid)
    {
        radio->noise_dbm = raw->wifi_status.local.radio.noise_dbm;
    }

    if (radio->temperature_valid)
    {
        radio->temperature_c = raw->wifi_status.local.chip_temperature_c;
    }

    if (radio->work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        radio->bandwidth_mhz = (uint16_t)raw->wifi_status.local.radio.params.wide.bandwidth;
        return;
    }

    if (radio->work_mode != LINKG_WIFI_WORK_MODE_NARROW)
    {
        return;
    }

    radio->narrow_mode = raw->wifi_status.local.radio.params.narrow.mode;

    if (raw->wifi_config_valid)
    {
        radio->bandwidth_mhz = raw->wifi_config.wideband.narrow_params.bandwidth;
    }

    if (raw->wifi_status.local.radio.params.narrow.current_rate_valid)
    {
        radio->rate_level_valid = true;
        radio->rate_level       = raw->wifi_status.local.radio.params.narrow.current_rate_level;
    }
}

/**
 * @brief 从周期Probe快照构造Wi-Fi三业务类别观测。
 */
static void _linkg_switch_observation_build_probe(const linkg_switch_observation_raw_t *raw, linkg_switch_wifi_observation_t *wifi)
{
    const linkg_path_probe_class_snapshot_t *source;
    linkg_switch_probe_observation_t        *destination;
    uint32_t                                 index;

    if (!raw->probe_valid || !raw->probe.wifi.active || raw->probe.wifi.link_id != raw->wifi_link_id)
    {
        return;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        source      = &raw->probe.wifi.classes[index];
        destination = &wifi->probe[index];

        destination->valid      = source->valid;
        destination->reachable  = source->reachable;
        destination->rtt_us     = source->rtt_us;
        destination->updated_us = source->updated_us;
    }
}

/**
 * @brief 按本轮真实时间间隔更新Wi-Fi Path累计差分速率采样，调用方持有Switch锁。
 */
static void _linkg_switch_observation_update_traffic_locked(linkg_switch_sta_peer_runtime_t *runtime, const linkg_switch_observation_raw_t *raw, uint64_t now_us, linkg_switch_traffic_observation_t *traffic)
{
    linkg_switch_traffic_sampler_t *sampler;
    uint64_t                        elapsed_us;
    uint64_t                        tx_packet_delta;
    uint64_t                        rx_packet_delta;
    uint64_t                        tx_byte_delta;
    uint64_t                        rx_byte_delta;
    uint64_t                        tx_pps;
    uint64_t                        rx_pps;

    memset(traffic, 0, sizeof(*traffic));
    sampler = &runtime->wifi_traffic_sampler;

    if (!raw->wifi_path_stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized ||
        sampler->link_id != raw->wifi_link_id ||
        now_us <= sampler->sampled_us ||
        raw->wifi_path_stats.tx_bytes < sampler->tx_bytes ||
        raw->wifi_path_stats.rx_bytes < sampler->rx_bytes ||
        raw->wifi_path_stats.tx_packets < sampler->tx_packets ||
        raw->wifi_path_stats.rx_packets < sampler->rx_packets)
    {
        sampler->initialized = true;
        sampler->link_id     = raw->wifi_link_id;
        sampler->sampled_us  = now_us;
        sampler->tx_bytes    = raw->wifi_path_stats.tx_bytes;
        sampler->rx_bytes    = raw->wifi_path_stats.rx_bytes;
        sampler->tx_packets  = raw->wifi_path_stats.tx_packets;
        sampler->rx_packets  = raw->wifi_path_stats.rx_packets;
        return;
    }

    elapsed_us      = now_us - sampler->sampled_us;
    tx_packet_delta = raw->wifi_path_stats.tx_packets - sampler->tx_packets;
    rx_packet_delta = raw->wifi_path_stats.rx_packets - sampler->rx_packets;
    tx_byte_delta   = raw->wifi_path_stats.tx_bytes - sampler->tx_bytes;
    rx_byte_delta   = raw->wifi_path_stats.rx_bytes - sampler->rx_bytes;

    tx_pps = _linkg_switch_observation_scale_ratio(tx_packet_delta, 1000000ULL, elapsed_us);
    rx_pps = _linkg_switch_observation_scale_ratio(rx_packet_delta, 1000000ULL, elapsed_us);

    traffic->valid      = true;
    traffic->tx_pps     = _linkg_switch_observation_u32_saturate(tx_pps);
    traffic->rx_pps     = _linkg_switch_observation_u32_saturate(rx_pps);
    traffic->tx_bps     = _linkg_switch_observation_scale_ratio(tx_byte_delta, 8000000ULL, elapsed_us);
    traffic->rx_bps     = _linkg_switch_observation_scale_ratio(rx_byte_delta, 8000000ULL, elapsed_us);
    traffic->updated_us = now_us;

    sampler->sampled_us = now_us;
    sampler->tx_bytes   = raw->wifi_path_stats.tx_bytes;
    sampler->rx_bytes   = raw->wifi_path_stats.rx_bytes;
    sampler->tx_packets = raw->wifi_path_stats.tx_packets;
    sampler->rx_packets = raw->wifi_path_stats.rx_packets;
}

/**
 * @brief 更新STA本机统计的AP到STA Wi-Fi下行丢包窗口，调用方持有Switch锁。
 */
static void _linkg_switch_observation_update_downlink_loss_locked(linkg_switch_sta_peer_runtime_t *runtime, const linkg_switch_observation_raw_t *raw, uint64_t now_us, linkg_switch_loss_observation_t *loss)
{
    linkg_switch_loss_sampler_t *sampler;
    uint64_t                     received_delta;
    uint64_t                     lost_delta;
    uint64_t                     total_delta;
    uint64_t                     loss_permille;

    memset(loss, 0, sizeof(*loss));
    sampler = &runtime->wifi_downlink_loss_sampler;

    if (!raw->wifi_rx_stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized ||
        sampler->link_id != raw->wifi_link_id ||
        raw->wifi_rx_stats.received_packets < sampler->received_packets ||
        raw->wifi_rx_stats.confirmed_lost_packets < sampler->lost_packets)
    {
        sampler->initialized      = true;
        sampler->link_id          = raw->wifi_link_id;
        sampler->received_packets = raw->wifi_rx_stats.received_packets;
        sampler->lost_packets     = raw->wifi_rx_stats.confirmed_lost_packets;
        return;
    }

    received_delta = raw->wifi_rx_stats.received_packets - sampler->received_packets;
    lost_delta     = raw->wifi_rx_stats.confirmed_lost_packets - sampler->lost_packets;
    total_delta    = received_delta + lost_delta;

    if (total_delta != 0U)
    {
        loss_permille = _linkg_switch_observation_scale_ratio(lost_delta, 1000ULL, total_delta);

        loss->valid          = true;
        loss->loss_permille  = _linkg_switch_observation_u32_saturate(loss_permille);
        loss->sample_packets = _linkg_switch_observation_u32_saturate(total_delta);
        loss->updated_us     = now_us;
    }

    sampler->received_packets = raw->wifi_rx_stats.received_packets;
    sampler->lost_packets     = raw->wifi_rx_stats.confirmed_lost_packets;
}

/**
 * @brief 发布本轮完整观测快照并更新STA本地差分采样状态。
 */
static int _linkg_switch_observation_publish(uint8_t peer_node_id, uint32_t peer_generation, const linkg_switch_observation_raw_t *raw, uint64_t now_us)
{
    linkg_switch_sta_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t     *peer;
    linkg_switch_observation_t       observation;

    memset(&observation, 0, sizeof(observation));

    observation.valid               = true;
    observation.peer_node_id        = peer_node_id;
    observation.collected_us        = now_us;
    observation.wifi.link_id        = raw->wifi_link_id;
    observation.cellular.link_id    = raw->cellular_link_id;
    observation.cellular.updated_us = now_us;

    _linkg_switch_observation_build_wifi_radio(raw, now_us, &observation.wifi.radio);
    _linkg_switch_observation_build_probe(raw, &observation.wifi);

    observation.wifi.available     = raw->wifi_path_available;
    observation.cellular.available = raw->cellular_internet_available && raw->cellular_path_available;

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (!g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    peer = linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    runtime = &peer->role.sta;

    _linkg_switch_observation_update_traffic_locked(runtime,
                                                     raw,
                                                     now_us,
                                                     &observation.wifi.traffic);

    _linkg_switch_observation_update_downlink_loss_locked(runtime,
                                                           raw,
                                                           now_us,
                                                           &observation.wifi.loss.downlink);

    observation.wifi.loss.uplink = runtime->remote_wifi_uplink_loss;
    runtime->observation         = observation;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/****************************** Observation ******************************/

/**
 * @brief 刷新指定STA直接AP的一轮Switch观测快照。
 */
int linkg_switch_observation_refresh(uint8_t peer_node_id, uint32_t peer_generation, uint64_t now_us)
{
    linkg_switch_observation_raw_t raw;
    int                            first_error;
    int                            ret;

    if (peer_node_id < LINKG_RESOURCE_NODE_ID_MIN || peer_node_id > LINKG_RESOURCE_NODE_ID_MAX ||
        peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID || now_us == 0U)
    {
        return -EINVAL;
    }

    memset(&raw, 0, sizeof(raw));
    raw.wifi_link_id     = LINKG_LINK_ID_INVALID;
    raw.cellular_link_id = LINKG_LINK_ID_INVALID;
    raw.elapsed_ms       = linkg_time_elapsed_ms();
    first_error          = 0;

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (!g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    if (linkg_switch_find_peer_generation_locked(peer_node_id, peer_generation) == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    pthread_mutex_unlock(&g_switch.lock);

    _linkg_switch_observation_collect_wifi_status(&raw, &first_error);
    _linkg_switch_observation_collect_probe(peer_node_id, &raw, &first_error);
    _linkg_switch_observation_collect_wifi_path(peer_node_id, &raw, &first_error);
    _linkg_switch_observation_collect_cellular(peer_node_id, &raw, &first_error);

    ret = _linkg_switch_observation_publish(peer_node_id, peer_generation, &raw, now_us);
    if (ret != 0)
    {
        return ret;
    }

    return first_error;
}

/**
 * @brief 更新AP上报的STA到AP Wi-Fi上行丢包观测。
 */
int linkg_switch_observation_update_remote_uplink_loss(uint8_t peer_node_id, uint32_t loss_permille, uint32_t sample_packets)
{
    linkg_switch_sta_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t     *peer;
    uint64_t now_us;

    if (peer_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        peer_node_id > LINKG_RESOURCE_NODE_ID_MAX ||
        loss_permille > 1000U)
    {
        return -EINVAL;
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (!g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    peer = linkg_switch_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    runtime = &peer->role.sta;

    if (runtime->remote_wifi_uplink_loss.valid && now_us < runtime->remote_wifi_uplink_loss.updated_us)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    runtime->remote_wifi_uplink_loss.valid          = sample_packets != 0U;
    runtime->remote_wifi_uplink_loss.loss_permille  = sample_packets != 0U ? loss_permille : 0U;
    runtime->remote_wifi_uplink_loss.sample_packets = sample_packets;
    runtime->remote_wifi_uplink_loss.updated_us     = now_us;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 清空指定STA Peer的观测快照和本地差分采样状态，调用方持有Switch锁。
 */
void linkg_switch_observation_reset_locked(uint8_t peer_node_id)
{
    linkg_switch_sta_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t     *peer;

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        return;
    }

    peer = linkg_switch_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        return;
    }

    runtime = &peer->role.sta;

    memset(&runtime->observation, 0, sizeof(runtime->observation));
    memset(&runtime->wifi_traffic_sampler, 0, sizeof(runtime->wifi_traffic_sampler));
    memset(&runtime->wifi_downlink_loss_sampler, 0, sizeof(runtime->wifi_downlink_loss_sampler));
    memset(&runtime->remote_wifi_uplink_loss, 0, sizeof(runtime->remote_wifi_uplink_loss));
}
