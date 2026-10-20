/**
 * @file switch_observation.c
 * @brief LinkG链路切换观测快照实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-17
 */

#include "switch_observation.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular.h"
#include "linkg_cellular_link.h"
#include "linkg_config.h"
#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_path_probe.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_wifi.h"
#include "linkg_wifi_link.h"

#include "switch_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_OBSERVATION_REMOTE_LOSS_MAX_AGE_US 1000000ULL // AP质量报告最长有效时间：1秒

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_wifi_status_snapshot_t     wifi_status;              // Wi-Fi运行状态快照
    linkg_wifi_config_t              wifi_config;              // Wi-Fi当前配置
    linkg_path_probe_peer_snapshot_t probe;                    // Peer周期Probe快照
    linkg_path_stats_t               wifi_path_stats;          // Wi-Fi Path发送及接收累计统计
    linkg_path_stats_t               cellular_path_stats;      // Cellular Path发送及接收累计统计
    linkg_wifi_rx_stats_t            wifi_rx_stats;            // Wi-Fi Wire累计接收统计
    linkg_cellular_rx_stats_t        cellular_rx_stats;        // Cellular Wire累计接收统计
    uint32_t                         wifi_link_id;             // 当前Wi-Fi Link实例
    uint32_t                         cellular_link_id;         // 当前Cellular Link实例
    uint64_t                         elapsed_ms;               // 当前程序运行时间
    bool                             wifi_status_valid;        // Wi-Fi运行状态有效
    bool                             wifi_config_valid;        // Wi-Fi配置有效
    bool                             probe_valid;              // Probe快照有效
    bool                             wifi_path_available;      // Wi-Fi Peer Path存在且Link运行
    bool                             wifi_path_stats_valid;    // Wi-Fi Path统计有效
    bool                             wifi_rx_stats_valid;      // Wi-Fi Wire累计统计有效
    bool                             cellular_data_ready;      // Cellular IPv6数据通道就绪
    bool                             cellular_ipv6_reachable;  // Cellular IPv6公网可达
    bool                             cellular_path_available;  // Cellular Peer Path存在且Link运行
    bool                             cellular_path_stats_valid;// Cellular Path统计有效
    bool                             cellular_rx_stats_valid;  // Cellular Wire累计统计有效
} linkg_switch_observation_raw_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断运行状态变化带来的预期采集错误。
 */
static bool _linkg_switch_observation_expected_state_error(int error)
{
    return error == -ENOENT || error == -ENODEV || error == -ENETDOWN || error == -EAGAIN || error == -ESTALE;
}

/**
 * @brief 保存本轮第一个非预期采集错误。
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
 * @brief 64位累计计数饱和转换为32位样本数。
 */
static uint32_t _linkg_switch_observation_u32_saturate(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/**
 * @brief 64位无符号饱和加法，避免统计窗口计数溢出。
 */
static uint64_t _linkg_switch_observation_u64_add(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

/**
 * @brief 计算放大scale倍的无符号比率，避免64位乘法溢出。
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
 * @brief 将elapsed_ms时间转换为当前CLOCK_MONOTONIC微秒时间基准。
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
 * @brief 获取指定Peer在指定Link上的活动Path，并读取可选的累计统计。
 *
 * 不持有Switch锁访问其他模块，Path引用由本函数释放。
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
 * @brief 读取指定Peer的周期Probe快照。
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
 * @brief 读取STA Wi-Fi Path统计及本机Wi-Fi Wire接收统计。
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
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }
}

/**
 * @brief 读取Cellular数据状态、Peer Path TX统计及本机Wire RX统计。
 */
static void _linkg_switch_observation_collect_cellular(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw, int *first_error)
{
    linkg_link_t *cellular_link;
    bool          data_ready;
    bool          ipv6_internet_available;
    int           ret;

    data_ready              = false;
    ipv6_internet_available = false;

    ret = linkg_cellular_get_internet_available(LINKG_CELLULAR_AVAILABLE_DATA, &data_ready);
    if (ret == 0)
    {
        raw->cellular_data_ready = data_ready;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }

    ret = linkg_cellular_get_internet_available(LINKG_CELLULAR_AVAILABLE_IPV6, &ipv6_internet_available);
    if (ret == 0)
    {
        raw->cellular_ipv6_reachable = ipv6_internet_available;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }

    raw->cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    ret = _linkg_switch_observation_collect_path(peer_node_id,
                                                  raw->cellular_link_id,
                                                  true,
                                                  &raw->cellular_path_available,
                                                  &raw->cellular_path_stats);
    if (ret == 0 && raw->cellular_path_available)
    {
        raw->cellular_path_stats_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, first_error);
    }

    if (!raw->cellular_path_available || raw->cellular_link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    cellular_link = linkg_link_manager_get(raw->cellular_link_id);
    if (cellular_link == NULL || !linkg_link_is_running(cellular_link))
    {
        return;
    }

    ret = linkg_cellular_link_get_rx_stats(cellular_link, peer_node_id, &raw->cellular_rx_stats);
    if (ret == 0)
    {
        raw->cellular_rx_stats_valid = true;
    }
    else
    {
        // 来源尚未出现时通常为-ENOENT，本轮Wire样本必须保持无效。
        _linkg_switch_observation_record_error(ret, first_error);
    }
}

/****************************** 观测构造 ******************************/

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
 * @brief 将单条Path的三个业务类别周期Probe复制到Switch观测。
 */
static void _linkg_switch_observation_build_probe_path(const linkg_path_probe_path_snapshot_t *source, uint32_t link_id, linkg_switch_probe_observation_t *destination)
{
    uint32_t index;

    if (!source->active || source->link_id != link_id)
    {
        return;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        destination[index].valid      = source->classes[index].valid;
        destination[index].reachable  = source->classes[index].reachable;
        destination[index].rtt_us     = source->classes[index].rtt_us;
        destination[index].updated_us = source->classes[index].updated_us;
    }
}

/**
 * @brief 从周期Probe快照构造Wi-Fi和Cellular三业务类别观测。
 */
static void _linkg_switch_observation_build_probe(const linkg_switch_observation_raw_t *raw, linkg_switch_observation_t *observation)
{
    if (!raw->probe_valid)
    {
        return;
    }

    _linkg_switch_observation_build_probe_path(&raw->probe.wifi,
                                                 raw->wifi_link_id,
                                                 observation->wifi.probe);

    _linkg_switch_observation_build_probe_path(&raw->probe.cellular,
                                                 raw->cellular_link_id,
                                                 observation->cellular.probe);
}

/****************************** 差分采样 ******************************/

/**
 * @brief 计算Wi-Fi Path双向实际发送/接收速率，调用方持有Switch锁。
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
 * @brief 将本机Wire接收累计统计转换成当前窗口精确丢包数量。
 *
 * 调用方已经持有Switch锁。
 */
static void _linkg_switch_observation_update_wire_locked(linkg_switch_loss_sampler_t *sampler, uint32_t link_id, bool stats_valid, uint64_t received, uint64_t lost, uint64_t now_us, linkg_switch_loss_observation_t *observation)
{
    uint64_t received_delta;
    uint64_t lost_delta;
    uint64_t total_delta;

    memset(observation, 0, sizeof(*observation));

    if (!stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized ||
        sampler->link_id != link_id ||
        received < sampler->received_packets ||
        lost < sampler->lost_packets)
    {
        sampler->initialized      = true;
        sampler->link_id          = link_id;
        sampler->received_packets = received;
        sampler->lost_packets     = lost;
        return;
    }

    received_delta = received - sampler->received_packets;
    lost_delta     = lost - sampler->lost_packets;
    total_delta    = _linkg_switch_observation_u64_add(received_delta, lost_delta);

    if (total_delta != 0U)
    {
        observation->valid          = true;
        observation->lost_packets   = _linkg_switch_observation_u32_saturate(lost_delta);
        observation->sample_packets = _linkg_switch_observation_u32_saturate(total_delta);
        observation->updated_us     = now_us;
    }

    sampler->received_packets = received;
    sampler->lost_packets     = lost;
}

/**
 * @brief 将本机TX累计Queue Overflow转换成当前窗口精确Load丢包数量。
 *
 * 调用方已经持有Switch锁。
 * Wi-Fi和Cellular统一采用success/(success+overflow)的统计口径。
 */
static void _linkg_switch_observation_update_load_locked(linkg_switch_load_sampler_t *sampler, uint32_t link_id, bool stats_valid, const linkg_path_stats_t *stats, uint64_t now_us, linkg_switch_loss_observation_t *observation)
{
    uint64_t sent_delta;
    uint64_t overflow_delta;
    uint64_t total_delta;

    memset(observation, 0, sizeof(*observation));

    if (!stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized ||
        sampler->link_id != link_id ||
        stats->tx_packets < sampler->sent_packets ||
        stats->tx_queue_overflow_packets < sampler->queue_overflow_packets)
    {
        sampler->initialized            = true;
        sampler->link_id                = link_id;
        sampler->sent_packets           = stats->tx_packets;
        sampler->queue_overflow_packets = stats->tx_queue_overflow_packets;
        return;
    }

    sent_delta     = stats->tx_packets - sampler->sent_packets;
    overflow_delta = stats->tx_queue_overflow_packets - sampler->queue_overflow_packets;
    total_delta    = _linkg_switch_observation_u64_add(sent_delta, overflow_delta);

    if (total_delta != 0U)
    {
        observation->valid          = true;
        observation->lost_packets   = _linkg_switch_observation_u32_saturate(overflow_delta);
        observation->sample_packets = _linkg_switch_observation_u32_saturate(total_delta);
        observation->updated_us     = now_us;
    }

    sampler->sent_packets           = stats->tx_packets;
    sampler->queue_overflow_packets = stats->tx_queue_overflow_packets;
}

/**
 * @brief 将AP远端观测合并到本轮快照；过期值标记无效。
 *
 * updated_us由STA收到Quality Report时生成，不能与AP本机时钟直接比较。
 */
static void _linkg_switch_observation_copy_remote_locked(const linkg_switch_loss_observation_t *source, uint64_t now_us, linkg_switch_loss_observation_t *destination)
{
    memset(destination, 0, sizeof(*destination));

    if (source->updated_us == 0U ||
        now_us < source->updated_us ||
        now_us - source->updated_us > LINKG_SWITCH_OBSERVATION_REMOTE_LOSS_MAX_AGE_US)
    {
        return;
    }

    *destination = *source;
}

/****************************** 快照发布 ******************************/

/**
 * @brief 更新STA本地四项窗口统计并合并AP远端四项报告。
 *
 * 外部采集已完成；持有Switch锁期间只处理预先取得的快照与差分基线。
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
    _linkg_switch_observation_build_probe(raw, &observation);

    observation.wifi.available     = raw->wifi_path_available;
    observation.cellular.available = raw->cellular_data_ready && raw->cellular_ipv6_reachable && raw->cellular_path_available;

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

    // Wi-Fi实际收发速率，保留原有Path统计差分语义。
    _linkg_switch_observation_update_traffic_locked(runtime, raw, now_us, &observation.wifi.traffic);

    // STA本机接收端：AP -> STA，两条链路各自独立Wire窗口。
    _linkg_switch_observation_update_wire_locked(&runtime->wifi_downlink_loss_sampler,
                                                   raw->wifi_link_id,
                                                   raw->wifi_path_available && raw->wifi_rx_stats_valid,
                                                   raw->wifi_rx_stats.received_packets,
                                                   raw->wifi_rx_stats.confirmed_lost_packets,
                                                   now_us,
                                                   &observation.wifi.loss.downlink);

    _linkg_switch_observation_update_wire_locked(&runtime->cellular_downlink_loss_sampler,
                                                   raw->cellular_link_id,
                                                   raw->cellular_path_available && raw->cellular_rx_stats_valid,
                                                   raw->cellular_rx_stats.received_packets,
                                                   raw->cellular_rx_stats.confirmed_lost_packets,
                                                   now_us,
                                                   &observation.cellular.loss.downlink);

    // STA本机发送端：STA -> AP，两条链路各自独立Queue Overflow窗口。
    _linkg_switch_observation_update_load_locked(&runtime->wifi_uplink_load_sampler,
                                                   raw->wifi_link_id,
                                                   raw->wifi_path_available && raw->wifi_path_stats_valid,
                                                   &raw->wifi_path_stats,
                                                   now_us,
                                                   &observation.wifi.loss.uplink_load);

    _linkg_switch_observation_update_load_locked(&runtime->cellular_uplink_load_sampler,
                                                   raw->cellular_link_id,
                                                   raw->cellular_path_available && raw->cellular_path_stats_valid,
                                                   &raw->cellular_path_stats,
                                                   now_us,
                                                   &observation.cellular.loss.uplink_load);

    // AP远端接收端：STA -> AP，两条链路各自Wire质量。
    _linkg_switch_observation_copy_remote_locked(&runtime->remote_wifi_uplink_loss,
                                                   now_us,
                                                   &observation.wifi.loss.uplink);

    _linkg_switch_observation_copy_remote_locked(&runtime->remote_cellular_uplink_loss,
                                                   now_us,
                                                   &observation.cellular.loss.uplink);

    // AP远端发送端：AP -> STA，两条链路各自Queue Overflow质量。
    _linkg_switch_observation_copy_remote_locked(&runtime->remote_wifi_downlink_load,
                                                   now_us,
                                                   &observation.wifi.loss.downlink_load);

    _linkg_switch_observation_copy_remote_locked(&runtime->remote_cellular_downlink_load,
                                                   now_us,
                                                   &observation.cellular.loss.downlink_load);

    runtime->observation = observation;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/****************************** Observation ******************************/

/**
 * @brief 刷新指定STA直接AP的一轮完整Switch观测快照。
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
 * @brief 更新指定STA Peer最近一次AP上报的Wi-Fi上行精确丢包观测。
 */
int linkg_switch_observation_update_remote_uplink_loss(uint8_t peer_node_id, uint32_t lost_packets, uint32_t sample_packets)
{
    linkg_switch_sta_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t     *peer;
    uint64_t                         now_us;

    if (peer_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        peer_node_id > LINKG_RESOURCE_NODE_ID_MAX ||
        lost_packets > sample_packets)
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

    if (runtime->remote_wifi_uplink_loss.valid &&
        now_us < runtime->remote_wifi_uplink_loss.updated_us)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    runtime->remote_wifi_uplink_loss.valid          = sample_packets != 0U;
    runtime->remote_wifi_uplink_loss.lost_packets   = lost_packets;
    runtime->remote_wifi_uplink_loss.sample_packets = sample_packets;
    runtime->remote_wifi_uplink_loss.updated_us     = now_us;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/**
 * @brief 清空指定STA Peer观测及采样基线，调用方已经持有Switch锁。
 *
 * 不清除Plan Sync及消息编号；Peer生命周期变更由上层统一管理。
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
    memset(&runtime->wifi_uplink_load_sampler, 0, sizeof(runtime->wifi_uplink_load_sampler));
    memset(&runtime->cellular_downlink_loss_sampler, 0, sizeof(runtime->cellular_downlink_loss_sampler));
    memset(&runtime->cellular_uplink_load_sampler, 0, sizeof(runtime->cellular_uplink_load_sampler));

    memset(&runtime->remote_wifi_uplink_loss, 0, sizeof(runtime->remote_wifi_uplink_loss));
    memset(&runtime->remote_wifi_downlink_load, 0, sizeof(runtime->remote_wifi_downlink_load));
    memset(&runtime->remote_cellular_uplink_loss, 0, sizeof(runtime->remote_cellular_uplink_loss));
    memset(&runtime->remote_cellular_downlink_load, 0, sizeof(runtime->remote_cellular_downlink_load));
    memset(&runtime->wifi_loss_policy, 0, sizeof(runtime->wifi_loss_policy));
    memset(&runtime->wifi_load_policy, 0, sizeof(runtime->wifi_load_policy));
}
