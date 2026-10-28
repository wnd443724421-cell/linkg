/**
 * @file switch_observation.c
 * @brief LinkG Switch统一指标采集、差分及观测快照发布
 * @author Dawn
 * @version 3.0.0
 * @date 2026-10-22
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
#include "wifi_driver_ops.h"

#include "switch_internal.h"
#include "switch_trace.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_OBSERVATION_REMOTE_LOSS_MAX_AGE_US 1000000ULL   // AP质量报告最长有效期，1秒
#define LINKG_SWITCH_OBSERVATION_REMOTE_HCC_MAX_AGE_US  1000000ULL   // AP HCC状态最长有效期，1秒
#define LINKG_SWITCH_OBSERVATION_LOCAL_HCC_MAX_AGE_US   1000000ULL   // STA本机HCC采样最大有效期，1秒
#define LINKG_SWITCH_OBSERVATION_PROBE_MAX_AGE_US       2500000ULL   // Probe最大结果年龄，覆盖1秒周期和300ms超时
#define LINKG_SWITCH_OBSERVATION_WIFI_STATUS_MAX_AGE_US 3000000ULL   // Wi-Fi状态最大有效期，3秒
#define LINKG_SWITCH_OBSERVATION_WIFI_STATS_MAX_AGE_US  3000000ULL   // Wi-Fi Peer无线统计最大有效期，3秒

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_wifi_status_snapshot_t             wifi_status;               // Wi-Fi运行状态快照
    linkg_wifi_config_t                      wifi_config;               // Wi-Fi当前配置
    linkg_path_probe_peer_snapshot_t         probe;                     // Peer周期Probe快照
    linkg_path_stats_t                       wifi_path_stats;           // Wi-Fi Path累计统计
    linkg_path_stats_t                       cellular_path_stats;       // Cellular Path累计统计
    linkg_wifi_rx_stats_t                    wifi_rx_stats;             // Wi-Fi本机Wire接收统计
    linkg_switch_wifi_hcc_observation_t      wifi_hcc;                  // STA本机本轮HCC原始状态
    linkg_cellular_rx_stats_t                cellular_rx_stats;         // Cellular本机Wire接收统计
    linkg_switch_observation_state_t         wifi_path_state;           // Wi-Fi Path本轮查询结果
    linkg_switch_observation_state_t         cellular_path_state;       // Cellular Path本轮查询结果
    uint32_t                                 wifi_link_id;              // 当前Wi-Fi Link实例
    uint32_t                                 cellular_link_id;          // 当前Cellular Link实例
    bool                                     wifi_status_valid;         // Wi-Fi模块状态读取成功
    bool                                     wifi_config_valid;         // Wi-Fi配置读取成功
    bool                                     probe_valid;               // Probe快照读取成功
    bool                                     wifi_path_stats_valid;     // Wi-Fi Path累计统计读取成功
    bool                                     cellular_path_stats_valid; // Cellular Path累计统计读取成功
    bool                                     wifi_rx_stats_valid;       // Wi-Fi Wire累计统计读取成功
    bool                                     cellular_rx_stats_valid;   // Cellular Wire累计统计读取成功
    bool                                     cellular_data_known;       // Cellular数据网络可用性已确认
    bool                                     cellular_data_ready;       // Cellular数据网络可用
    bool                                     cellular_ipv6_known;       // Cellular IPv6公网可达性已确认
    bool                                     cellular_ipv6_reachable;   // Cellular IPv6公网可达
    int                                      collect_error;             // 本轮首个非预期采集错误
} linkg_switch_observation_raw_t;

/****************************** 基础辅助 ******************************/

/**
 * @brief 判断外部模块生命周期变化产生的预期采集错误。
 */
static bool _linkg_switch_observation_expected_error(int error)
{
    return error == -ENOENT || error == -ENODEV || error == -ENETDOWN || error == -EAGAIN || error == -ESTALE;
}

/**
 * @brief 保存首个异常采集错误，不改变其他指标的可用性。
 */
static void _linkg_switch_observation_record_error(int error, int *first_error)
{
    if (first_error != NULL && *first_error == 0 && error != 0 && !_linkg_switch_observation_expected_error(error))
    {
        *first_error = error;
    }
}

/**
 * @brief 将64位非负累计统计饱和转换为32位窗口计数。
 */
static uint32_t _linkg_switch_observation_u32_saturate(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/**
 * @brief 将两个累计差分饱和相加，避免64位整数回绕。
 */
static uint64_t _linkg_switch_observation_u64_add(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

/**
 * @brief 计算放大后的非负比率并防止64位乘法溢出。
 */
static uint64_t _linkg_switch_observation_scale_ratio(uint64_t numerator, uint64_t scale, uint64_t denominator)
{
    uint64_t quotient;
    uint64_t remainder;
    uint64_t value;
    uint64_t fraction;

    if (denominator == 0U || scale == 0U)
    {
        return 0U;
    }

    quotient  = numerator / denominator;
    remainder = numerator % denominator;

    if (quotient > UINT64_MAX / scale || remainder > UINT64_MAX / scale)
    {
        return UINT64_MAX;
    }

    value    = quotient * scale;
    fraction = (remainder * scale) / denominator;

    return UINT64_MAX - value < fraction ? UINT64_MAX : value + fraction;
}

/**
 * @brief 检查采样来源时间是否在当前有效年龄范围内。
 */
static bool _linkg_switch_observation_fresh(uint64_t updated_us, uint64_t now_us, uint64_t max_age_us)
{
    return updated_us != 0U && now_us >= updated_us && now_us - updated_us <= max_age_us;
}

/**
 * @brief 将应用运行毫秒时间转换为CLOCK_MONOTONIC绝对微秒时间。
 *
 * elapsed_ms必须在外部采集完成后读取，与采集完成时的now_us配对。
 */
static uint64_t _linkg_switch_observation_elapsed_to_monotonic(uint64_t updated_ms, uint64_t now_us, uint64_t elapsed_ms)
{
    uint64_t age_ms;

    if (updated_ms == 0U || updated_ms > elapsed_ms)
    {
        return 0U;
    }

    age_ms = elapsed_ms - updated_ms;
    if (age_ms > now_us / 1000ULL)
    {
        return 0U;
    }

    return now_us - age_ms * 1000ULL;
}

/****************************** 原始数据采集 ******************************/

/**
 * @brief 取得指定Peer的活动Path状态和可选累计统计；内部释放Path引用。
 *
 * 当前无Link、Link停止、Peer Path明确不存在属于确认不可用。
 * 临时查询竞争或异常属于未知，不允许直接触发硬故障策略。
 */
static int _linkg_switch_observation_collect_path(uint8_t peer_node_id, uint32_t link_id, linkg_switch_observation_state_t *state, bool *stats_valid, linkg_path_stats_t *stats)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    int                   ret;

    if (state == NULL || stats_valid == NULL || stats == NULL)
    {
        return -EINVAL;
    }

    *state       = LINKG_SWITCH_OBSERVATION_UNKNOWN;
    *stats_valid = false;
    memset(stats, 0, sizeof(*stats));

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        *state = LINKG_SWITCH_OBSERVATION_UNUSABLE;
        return 0;
    }

    link = linkg_link_manager_get(link_id);
    if (link == NULL)
    {
        return 0;
    }

    if (!linkg_link_is_running(link))
    {
        *state = LINKG_SWITCH_OBSERVATION_UNUSABLE;
        return 0;
    }

    path = NULL;
    memset(&endpoint, 0, sizeof(endpoint));

    ret = linkg_node_acquire_path(peer_node_id, link_id, &path, &endpoint);
    if (ret == -ENOENT || ret == -ENETDOWN)
    {
        *state = LINKG_SWITCH_OBSERVATION_UNUSABLE;
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (path == NULL)
    {
        return -EIO;
    }

    *state = LINKG_SWITCH_OBSERVATION_USABLE;

    ret = linkg_path_get_stats(path, stats);
    if (ret == 0)
    {
        *stats_valid = true;
    }

    linkg_path_release(path);

    return ret;
}

/**
 * @brief 读取STA Wi-Fi基础状态及配置；两者独立有效。
 */
static void _linkg_switch_observation_collect_wifi_status(linkg_switch_observation_raw_t *raw)
{
    int ret;

    ret = linkg_wifi_get_status(&raw->wifi_status);
    if (ret == 0)
    {
        raw->wifi_status_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }

    ret = linkg_config_get_wifi(&raw->wifi_config);
    if (ret == 0)
    {
        raw->wifi_config_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }
}

/**
 * @brief 获取本轮指定Peer三业务周期Probe快照。
 */
static void _linkg_switch_observation_collect_probe(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw)
{
    int ret;

    ret = linkg_path_probe_get_peer_snapshot(peer_node_id, &raw->probe);
    if (ret == 0)
    {
        raw->probe_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }
}

/**
 * @brief 读取指定Peer Wi-Fi Path发送统计与本机Wire接收统计。
 */
static void _linkg_switch_observation_collect_wifi_path(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw)
{
    linkg_link_t *link;
    int           ret;

    raw->wifi_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);

    ret = _linkg_switch_observation_collect_path(peer_node_id, raw->wifi_link_id, &raw->wifi_path_state,
                                                  &raw->wifi_path_stats_valid, &raw->wifi_path_stats);
    _linkg_switch_observation_record_error(ret, &raw->collect_error);

    if (raw->wifi_path_state != LINKG_SWITCH_OBSERVATION_USABLE)
    {
        return;
    }

    link = linkg_link_manager_get(raw->wifi_link_id);
    if (link == NULL || !linkg_link_is_running(link))
    {
        return;
    }

    ret = linkg_wifi_link_get_rx_stats(link, peer_node_id, &raw->wifi_rx_stats);
    if (ret == 0)
    {
        raw->wifi_rx_stats_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }
}

/**
 * @brief 从HI1105驱动直接采集STA本机发送侧HCC状态。
 *
 * HCC为Wi-Fi设备级观测；仅在当前Peer Wi-Fi Path有效时读取。
 * 读取在Switch锁外执行，不修改Wi-Fi TX流控基线和发送准入状态。
 * 驱动读取失败仅使本轮HCC无效，不影响其它观测源。
 */
static void _linkg_switch_observation_collect_wifi_hcc(linkg_switch_observation_raw_t *raw)
{
    wal_tx_flowctrl_status_stru status;
    uint64_t                   updated_us;
    int                        ret;

    if (raw->wifi_path_state != LINKG_SWITCH_OBSERVATION_USABLE)
    {
        return;
    }

    ret = wifi_driver_get_tx_flowctrl_status(&status);
    if (ret != 0)
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
        return;
    }

    updated_us = linkg_time_monotonic_us();
    if (updated_us == 0U)
    {
        _linkg_switch_observation_record_error(-EIO, &raw->collect_error);
        return;
    }

    raw->wifi_hcc.vo_queue_length    = status.vo_queue_length;
    raw->wifi_hcc.vi_queue_length    = status.vi_queue_length;
    raw->wifi_hcc.be_queue_length    = status.be_queue_length;
    raw->wifi_hcc.flowctrl_off_count = status.flowctrl_off_count;
    raw->wifi_hcc.tx_allowed         = status.tx_allowed != 0U;
    raw->wifi_hcc.updated_us         = updated_us;
    raw->wifi_hcc.valid              = true;
}

/**
 * @brief 读取Cellular数据/IPv6可用性、Path统计与本机Wire接收统计。
 */
static void _linkg_switch_observation_collect_cellular(uint8_t peer_node_id, linkg_switch_observation_raw_t *raw)
{
    linkg_link_t *link;
    bool          available;
    int           ret;

    available = false;
    ret = linkg_cellular_get_internet_available(LINKG_CELLULAR_AVAILABLE_DATA, &available);
    if (ret == 0)
    {
        raw->cellular_data_known = true;
        raw->cellular_data_ready = available;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }

    available = false;
    ret = linkg_cellular_get_internet_available(LINKG_CELLULAR_AVAILABLE_IPV6, &available);
    if (ret == 0)
    {
        raw->cellular_ipv6_known     = true;
        raw->cellular_ipv6_reachable = available;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }

    raw->cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    ret = _linkg_switch_observation_collect_path(peer_node_id, raw->cellular_link_id, &raw->cellular_path_state,
                                                  &raw->cellular_path_stats_valid, &raw->cellular_path_stats);
    _linkg_switch_observation_record_error(ret, &raw->collect_error);

    if (raw->cellular_path_state != LINKG_SWITCH_OBSERVATION_USABLE)
    {
        return;
    }

    link = linkg_link_manager_get(raw->cellular_link_id);
    if (link == NULL || !linkg_link_is_running(link))
    {
        return;
    }

    ret = linkg_cellular_link_get_rx_stats(link, peer_node_id, &raw->cellular_rx_stats);
    if (ret == 0)
    {
        raw->cellular_rx_stats_valid = true;
    }
    else
    {
        _linkg_switch_observation_record_error(ret, &raw->collect_error);
    }
}

/****************************** Wi-Fi状态构造 ******************************/

/**
 * @brief 构造有时效保护的STA Wi-Fi无线状态；过期值不参与可用性判定。
 */
static void _linkg_switch_observation_build_wifi_radio(const linkg_switch_observation_raw_t *raw, uint64_t now_us, uint64_t elapsed_ms, linkg_switch_wifi_radio_observation_t *radio)
{
    const linkg_wifi_peer_status_t *peer;

    memset(radio, 0, sizeof(*radio));
    radio->narrow_mode = LINKG_WIFI_NARROW_MODE_UNKNOWN;

    if (!raw->wifi_status_valid || raw->wifi_status.local.role != LINKG_DEVICE_ROLE_STA)
    {
        return;
    }

    radio->status_updated_us = _linkg_switch_observation_elapsed_to_monotonic(raw->wifi_status.local.updated_ms, now_us, elapsed_ms);
    if (!_linkg_switch_observation_fresh(radio->status_updated_us, now_us, LINKG_SWITCH_OBSERVATION_WIFI_STATUS_MAX_AGE_US))
    {
        return;
    }

    peer = &raw->wifi_status.role.sta.peer;

    radio->valid             = true;
    radio->connected         = raw->wifi_status.local.interface_state == LINKG_WIFI_INTERFACE_STATE_CONNECTED && peer->valid && peer->state == LINKG_WIFI_PEER_STATE_CONNECTED;
    radio->noise_valid       = raw->wifi_status.local.radio.noise_valid;
    radio->work_mode         = raw->wifi_status.local.radio.work_mode;
    radio->temperature_valid = raw->wifi_status.local.chip_temperature_valid;

    radio->statistics_updated_us = _linkg_switch_observation_elapsed_to_monotonic(peer->statistics_updated_ms, now_us, elapsed_ms);
    radio->statistics_valid      = peer->valid && peer->statistics_valid &&
                                   _linkg_switch_observation_fresh(radio->statistics_updated_us, now_us, LINKG_SWITCH_OBSERVATION_WIFI_STATS_MAX_AGE_US);

    if (radio->statistics_valid)
    {
        radio->rssi_dbm    = peer->rssi_dbm;
        radio->tx_phy_kbps = peer->tx_rate_kbps;
        radio->rx_phy_kbps = peer->rx_rate_kbps;
        radio->inactive_ms = peer->inactive_ms;
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
 * @brief 组合Wi-Fi Path和当前无线关联状态，得到明确的可用/不可用/未知。
 */
static linkg_switch_observation_state_t _linkg_switch_observation_wifi_state(const linkg_switch_observation_raw_t *raw, const linkg_switch_wifi_radio_observation_t *radio)
{
    const linkg_wifi_peer_status_t *peer;

    if (raw->wifi_path_state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
    {
        return LINKG_SWITCH_OBSERVATION_UNUSABLE;
    }

    if (!radio->valid)
    {
        return LINKG_SWITCH_OBSERVATION_UNKNOWN;
    }

    if (raw->wifi_status.local.interface_state == LINKG_WIFI_INTERFACE_STATE_DOWN ||
        raw->wifi_status.local.interface_state == LINKG_WIFI_INTERFACE_STATE_READY)
    {
        return LINKG_SWITCH_OBSERVATION_UNUSABLE;
    }

    if (raw->wifi_status.local.interface_state != LINKG_WIFI_INTERFACE_STATE_CONNECTED)
    {
        return LINKG_SWITCH_OBSERVATION_UNKNOWN;
    }

    peer = &raw->wifi_status.role.sta.peer;
    if (!peer->valid)
    {
        return LINKG_SWITCH_OBSERVATION_UNKNOWN;
    }

    if (peer->state == LINKG_WIFI_PEER_STATE_DISCONNECTED)
    {
        return LINKG_SWITCH_OBSERVATION_UNUSABLE;
    }

    if (peer->state != LINKG_WIFI_PEER_STATE_CONNECTED)
    {
        return LINKG_SWITCH_OBSERVATION_UNKNOWN;
    }

    return raw->wifi_path_state == LINKG_SWITCH_OBSERVATION_USABLE ? LINKG_SWITCH_OBSERVATION_USABLE : LINKG_SWITCH_OBSERVATION_UNKNOWN;
}

/**
 * @brief 组合蜂窝数据、IPv6公网可达及Path事实，得到三态可用性。
 */
static linkg_switch_observation_state_t _linkg_switch_observation_cellular_state(const linkg_switch_observation_raw_t *raw)
{
    if (raw->cellular_path_state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
        (raw->cellular_data_known && !raw->cellular_data_ready) ||
        (raw->cellular_ipv6_known && !raw->cellular_ipv6_reachable))
    {
        return LINKG_SWITCH_OBSERVATION_UNUSABLE;
    }

    if (raw->cellular_path_state == LINKG_SWITCH_OBSERVATION_USABLE &&
        raw->cellular_data_known && raw->cellular_data_ready &&
        raw->cellular_ipv6_known && raw->cellular_ipv6_reachable)
    {
        return LINKG_SWITCH_OBSERVATION_USABLE;
    }

    return LINKG_SWITCH_OBSERVATION_UNKNOWN;
}

/****************************** Probe观测构造 ******************************/

/**
 * @brief 复制与当前Link匹配且未过期的三业务Probe结果。
 */
static void _linkg_switch_observation_build_probe_path(const linkg_path_probe_path_snapshot_t *source, uint32_t link_id, uint64_t now_us, linkg_switch_probe_observation_t dest[LINKG_TRANSPORT_CLASS_COUNT])
{
    uint32_t index;

    if (!source->active || source->link_id != link_id || link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if (!_linkg_switch_observation_fresh(source->classes[index].updated_us, now_us, LINKG_SWITCH_OBSERVATION_PROBE_MAX_AGE_US))
        {
            continue;
        }

        dest[index].valid      = source->classes[index].valid;
        dest[index].reachable  = source->classes[index].valid && source->classes[index].reachable;
        dest[index].rtt_us     = dest[index].reachable ? source->classes[index].rtt_us : 0U;
        dest[index].updated_us = source->classes[index].updated_us;
    }
}

/**
 * @brief 从周期Probe源快照构造两条物理Link的三业务Probe样本。
 */
static void _linkg_switch_observation_build_probe(const linkg_switch_observation_raw_t *raw, uint64_t now_us, linkg_switch_observation_t *observation)
{
    if (!raw->probe_valid)
    {
        return;
    }

    _linkg_switch_observation_build_probe_path(&raw->probe.wifi, raw->wifi_link_id, now_us, observation->wifi.probe);
    _linkg_switch_observation_build_probe_path(&raw->probe.cellular, raw->cellular_link_id, now_us, observation->cellular.probe);
}

/****************************** 差分采样 ******************************/

/**
 * @brief 将Wi-Fi Path累计收发字节与报文数量转换为当前窗口速率。
 *
 * 调用方已经持有Switch锁，采样源无效、计数回退或Link变化时仅重建基线。
 */
static void _linkg_switch_observation_update_traffic_locked(linkg_switch_sta_peer_runtime_t *runtime, const linkg_switch_observation_raw_t *raw, uint64_t now_us, linkg_switch_traffic_observation_t *traffic)
{
    linkg_switch_traffic_sampler_t *sampler;
    uint64_t                        elapsed_us;
    uint64_t                        tx_bytes_delta;
    uint64_t                        rx_bytes_delta;
    uint64_t                        tx_packets_delta;
    uint64_t                        rx_packets_delta;

    memset(traffic, 0, sizeof(*traffic));
    sampler = &runtime->wifi_traffic_sampler;

    if (!raw->wifi_path_stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized || sampler->link_id != raw->wifi_link_id ||
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

    elapsed_us       = now_us - sampler->sampled_us;
    tx_bytes_delta   = raw->wifi_path_stats.tx_bytes - sampler->tx_bytes;
    rx_bytes_delta   = raw->wifi_path_stats.rx_bytes - sampler->rx_bytes;
    tx_packets_delta = raw->wifi_path_stats.tx_packets - sampler->tx_packets;
    rx_packets_delta = raw->wifi_path_stats.rx_packets - sampler->rx_packets;

    traffic->valid      = true;
    traffic->tx_pps     = _linkg_switch_observation_u32_saturate(_linkg_switch_observation_scale_ratio(tx_packets_delta, 1000000ULL, elapsed_us));
    traffic->rx_pps     = _linkg_switch_observation_u32_saturate(_linkg_switch_observation_scale_ratio(rx_packets_delta, 1000000ULL, elapsed_us));
    traffic->tx_bps     = _linkg_switch_observation_scale_ratio(tx_bytes_delta, 8000000ULL, elapsed_us);
    traffic->rx_bps     = _linkg_switch_observation_scale_ratio(rx_bytes_delta, 8000000ULL, elapsed_us);
    traffic->updated_us = now_us;

    sampler->sampled_us  = now_us;
    sampler->tx_bytes    = raw->wifi_path_stats.tx_bytes;
    sampler->rx_bytes    = raw->wifi_path_stats.rx_bytes;
    sampler->tx_packets  = raw->wifi_path_stats.tx_packets;
    sampler->rx_packets  = raw->wifi_path_stats.rx_packets;
}

/**
 * @brief 将本机Wire接收累计计数转换为整链路及三业务独立窗口样本。
 *
 * 调用方持有Switch锁；不读取或修改Policy积分。
 */
static void _linkg_switch_observation_update_wire_locked(linkg_switch_loss_sampler_t *sampler, uint32_t link_id, bool stats_valid, uint64_t received, uint64_t lost, const linkg_switch_loss_class_sampler_t classes[LINKG_TRANSPORT_CLASS_COUNT], uint64_t now_us, linkg_switch_loss_observation_t *observation, linkg_switch_loss_observation_t class_observations[LINKG_TRANSPORT_CLASS_COUNT])
{
    uint64_t received_delta;
    uint64_t lost_delta;
    uint64_t total_delta;
    uint64_t class_lost_delta;
    uint64_t sample_delta;
    uint32_t index;
    bool     rebase;

    memset(observation, 0, sizeof(*observation));
    memset(class_observations, 0, LINKG_TRANSPORT_CLASS_COUNT * sizeof(class_observations[0]));

    if (!stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    rebase = !sampler->initialized || sampler->link_id != link_id ||
             received < sampler->received_packets || lost < sampler->lost_packets;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if (classes[index].lost_packets > classes[index].sample_packets ||
            classes[index].lost_packets < sampler->classes[index].lost_packets ||
            classes[index].sample_packets < sampler->classes[index].sample_packets)
        {
            rebase = true;
        }
    }

    if (!rebase)
    {
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

        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            class_lost_delta = classes[index].lost_packets - sampler->classes[index].lost_packets;
            sample_delta     = classes[index].sample_packets - sampler->classes[index].sample_packets;

            if (sample_delta == 0U || class_lost_delta > sample_delta)
            {
                continue;
            }

            class_observations[index].valid          = true;
            class_observations[index].lost_packets   = _linkg_switch_observation_u32_saturate(class_lost_delta);
            class_observations[index].sample_packets = _linkg_switch_observation_u32_saturate(sample_delta);
            class_observations[index].updated_us     = now_us;
        }
    }

    sampler->initialized      = true;
    sampler->link_id          = link_id;
    sampler->received_packets = received;
    sampler->lost_packets     = lost;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        sampler->classes[index] = classes[index];
    }
}

/**
 * @brief 将当前TX成功提交与队列溢出累计计数转换成单窗口Load样本。
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

    if (!sampler->initialized || sampler->link_id != link_id ||
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
 * @brief 合并AP最近报告的一个窗口，过期不再发布为有效样本。
 *
 * AP报告在STA接收时已标记本地时间，复制不改动该源时间。
 */
static void _linkg_switch_observation_copy_remote_locked(const linkg_switch_loss_observation_t *source, uint64_t now_us, linkg_switch_loss_observation_t *destination)
{
    memset(destination, 0, sizeof(*destination));

    if (_linkg_switch_observation_fresh(source->updated_us, now_us, LINKG_SWITCH_OBSERVATION_REMOTE_LOSS_MAX_AGE_US))
    {
        *destination = *source;
    }
}

/**
 * @brief 合并AP上报的HCC设备级状态，并过滤无效或过期数据。
 *
 * 调用方已经持有Switch锁；保留STA接收AP报告时记录的原始时间戳。
 */
static void _linkg_switch_observation_copy_remote_hcc_locked(const linkg_switch_wifi_hcc_observation_t *source, uint64_t now_us, linkg_switch_wifi_hcc_observation_t *destination)
{
    memset(destination, 0, sizeof(*destination));

    if (!source->valid ||
        !_linkg_switch_observation_fresh(source->updated_us, now_us, LINKG_SWITCH_OBSERVATION_REMOTE_HCC_MAX_AGE_US))
    {
        return;
    }

    *destination = *source;
}

/****************************** 快照发布 ******************************/

/**
 * @brief 在Switch锁内计算本地窗口、合并远端指标并原子发布完整快照。
 *
 * 不调用Policy/Plan/Maintenance；Peer代际变化时拒绝写入旧采样。
 * collected_us使用采集结束时间；本地差分窗口继续使用Worker提供的周期时间，
 * 避免同一轮Policy认为新样本来自未来而忽略它。
 */
static int _linkg_switch_observation_publish(uint8_t peer_node_id, uint32_t peer_generation, const linkg_switch_observation_raw_t *raw, uint64_t now_us, uint64_t window_us, uint64_t elapsed_ms)
{
    linkg_switch_sta_peer_runtime_t  *runtime;
    linkg_switch_peer_runtime_t      *peer;
    linkg_switch_observation_t        observation;
    linkg_switch_loss_class_sampler_t wifi_classes[LINKG_TRANSPORT_CLASS_COUNT];
    linkg_switch_loss_class_sampler_t cellular_classes[LINKG_TRANSPORT_CLASS_COUNT];
    uint32_t                          index;

    memset(&observation, 0, sizeof(observation));
    memset(wifi_classes, 0, sizeof(wifi_classes));
    memset(cellular_classes, 0, sizeof(cellular_classes));

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        wifi_classes[index].lost_packets       = raw->wifi_rx_stats.classes[index].lost_packets;
        wifi_classes[index].sample_packets     = raw->wifi_rx_stats.classes[index].sample_packets;
        cellular_classes[index].lost_packets   = raw->cellular_rx_stats.classes[index].lost_packets;
        cellular_classes[index].sample_packets = raw->cellular_rx_stats.classes[index].sample_packets;
    }

    observation.valid               = true;
    observation.peer_node_id        = peer_node_id;
    observation.collected_us        = now_us;
    observation.collection_error    = raw->collect_error;
    observation.wifi.link_id        = raw->wifi_link_id;
    observation.cellular.link_id    = raw->cellular_link_id;
    observation.cellular.updated_us = now_us;

    _linkg_switch_observation_build_wifi_radio(raw, now_us, elapsed_ms, &observation.wifi.radio);
    _linkg_switch_observation_build_probe(raw, now_us, &observation);

    // STA本机TX：STA到AP的HCC发送侧状态，使用驱动查询结束时的采样时间。
    if (raw->wifi_hcc.valid &&
        _linkg_switch_observation_fresh(raw->wifi_hcc.updated_us, now_us, LINKG_SWITCH_OBSERVATION_LOCAL_HCC_MAX_AGE_US))
    {
        observation.wifi.uplink_hcc = raw->wifi_hcc;
    }

    observation.wifi.state     = _linkg_switch_observation_wifi_state(raw, &observation.wifi.radio);
    observation.cellular.state = _linkg_switch_observation_cellular_state(raw);

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

    // 防止并发刷新时，较早完成的采样被更晚取得锁的旧快照覆盖。
    if (runtime->observation.valid && runtime->observation.collected_us > now_us)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    _linkg_switch_observation_update_traffic_locked(runtime, raw, window_us, &observation.wifi.traffic);

    // STA本地RX：AP到STA Wire窗口。
    _linkg_switch_observation_update_wire_locked(&runtime->wifi_downlink_loss_sampler, raw->wifi_link_id,
                                                   raw->wifi_rx_stats_valid && raw->wifi_path_state == LINKG_SWITCH_OBSERVATION_USABLE,
                                                   raw->wifi_rx_stats.received_packets, raw->wifi_rx_stats.confirmed_lost_packets,
                                                   wifi_classes, window_us, &observation.wifi.loss.downlink, observation.wifi.loss.downlink_classes);

    _linkg_switch_observation_update_wire_locked(&runtime->cellular_downlink_loss_sampler, raw->cellular_link_id,
                                                   raw->cellular_rx_stats_valid && raw->cellular_path_state == LINKG_SWITCH_OBSERVATION_USABLE,
                                                   raw->cellular_rx_stats.received_packets, raw->cellular_rx_stats.confirmed_lost_packets,
                                                   cellular_classes, window_us, &observation.cellular.loss.downlink, observation.cellular.loss.downlink_classes);

    // STA本地TX：STA到AP Queue Overflow窗口。
    _linkg_switch_observation_update_load_locked(&runtime->wifi_uplink_load_sampler, raw->wifi_link_id,
                                                   raw->wifi_path_stats_valid, &raw->wifi_path_stats, window_us, &observation.wifi.loss.uplink_load);

    _linkg_switch_observation_update_load_locked(&runtime->cellular_uplink_load_sampler, raw->cellular_link_id,
                                                   raw->cellular_path_stats_valid, &raw->cellular_path_stats, window_us, &observation.cellular.loss.uplink_load);

    // AP远端RX：STA到AP Wire窗口。
    _linkg_switch_observation_copy_remote_locked(&runtime->remote_wifi_uplink_loss, now_us, &observation.wifi.loss.uplink);
    _linkg_switch_observation_copy_remote_locked(&runtime->remote_cellular_uplink_loss, now_us, &observation.cellular.loss.uplink);

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        _linkg_switch_observation_copy_remote_locked(&runtime->remote_wifi_uplink_classes[index], now_us, &observation.wifi.loss.uplink_classes[index]);
        _linkg_switch_observation_copy_remote_locked(&runtime->remote_cellular_uplink_classes[index], now_us, &observation.cellular.loss.uplink_classes[index]);
    }

    // AP远端TX：AP到STA Queue Overflow窗口。
    _linkg_switch_observation_copy_remote_locked(&runtime->remote_wifi_downlink_load, now_us, &observation.wifi.loss.downlink_load);
    _linkg_switch_observation_copy_remote_locked(&runtime->remote_cellular_downlink_load, now_us, &observation.cellular.loss.downlink_load);

    // AP远端TX：AP到STA的HCC发送侧状态，过期或无效样本不参与发布。
    _linkg_switch_observation_copy_remote_hcc_locked(&runtime->remote_wifi_downlink_hcc, now_us, &observation.wifi.downlink_hcc);

    runtime->observation = observation;

    pthread_mutex_unlock(&g_switch.lock);

    // 测试专用CSV写入：只在STA成功发布快照后、Switch锁外执行。
    linkg_switch_trace_record(&observation);

    return 0;
}

/****************************** 观测操作 ******************************/

/**
 * @brief 刷新指定STA Peer完整质量快照。
 *
 * 单个数据源失败只导致对应状态UNKNOWN或样本valid=false，不阻止其它来源发布。
 * 返回0表示快照已经发布，不表示所有采集项有效；真实生命周期错误仍返回负errno。
 */
int linkg_switch_observation_refresh(uint8_t peer_node_id, uint32_t peer_generation, uint64_t now_us)
{
    linkg_switch_observation_raw_t raw;
    uint64_t                       sampled_us;
    uint64_t                       elapsed_ms;
    int                            ret;

    if (peer_node_id < LINKG_RESOURCE_NODE_ID_MIN || peer_node_id > LINKG_RESOURCE_NODE_ID_MAX ||
        peer_generation == LINKG_SWITCH_PEER_GENERATION_INVALID || now_us == 0U)
    {
        return -EINVAL;
    }

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

    memset(&raw, 0, sizeof(raw));
    raw.wifi_link_id     = LINKG_LINK_ID_INVALID;
    raw.cellular_link_id = LINKG_LINK_ID_INVALID;

    _linkg_switch_observation_collect_wifi_status(&raw);
    _linkg_switch_observation_collect_probe(peer_node_id, &raw);
    _linkg_switch_observation_collect_wifi_path(peer_node_id, &raw);
    _linkg_switch_observation_collect_wifi_hcc(&raw);
    _linkg_switch_observation_collect_cellular(peer_node_id, &raw);

    // 时间戳必须与本轮外部查询结束时对应，而不是使用Worker进入函数前的时间。
    elapsed_ms = linkg_time_elapsed_ms();
    sampled_us = linkg_time_monotonic_us();
    if (sampled_us == 0U)
    {
        return -EIO;
    }

    ret = _linkg_switch_observation_publish(peer_node_id, peer_generation, &raw, sampled_us, now_us, elapsed_ms);
    return ret;
}

/**
 * @brief 仅清理STA Peer观测快照、本地采样基线和远端质量缓存。
 *
 * 调用方已经持有Switch锁；不重置Plan、Policy检测器或Operation。
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
    memset(&runtime->remote_wifi_uplink_classes, 0, sizeof(runtime->remote_wifi_uplink_classes));
    memset(&runtime->remote_wifi_downlink_load, 0, sizeof(runtime->remote_wifi_downlink_load));
    memset(&runtime->remote_wifi_downlink_hcc, 0, sizeof(runtime->remote_wifi_downlink_hcc));
    memset(&runtime->remote_cellular_uplink_loss, 0, sizeof(runtime->remote_cellular_uplink_loss));
    memset(&runtime->remote_cellular_uplink_classes, 0, sizeof(runtime->remote_cellular_uplink_classes));
    memset(&runtime->remote_cellular_downlink_load, 0, sizeof(runtime->remote_cellular_downlink_load));
}

/****************************** Probe状态转换 ******************************/

/**
 * @brief 根据周期Probe本轮有效性与可达性返回三态结果，不做策略质量判决。
 */
linkg_switch_observation_state_t linkg_switch_observation_probe_state(const linkg_switch_probe_observation_t *probe)
{
    if (probe == NULL || !probe->valid)
    {
        return LINKG_SWITCH_OBSERVATION_UNKNOWN;
    }

    return probe->reachable ? LINKG_SWITCH_OBSERVATION_USABLE : LINKG_SWITCH_OBSERVATION_UNUSABLE;
}
