/**
 * @file wifi_status.c
 * @brief LinkG Wi-Fi运行状态采集实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-08-26
 */

#include "wifi_status.h"

#include <errno.h>
#include <poll.h>
#include <stddef.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "wifi_driver_ops.h"
#include "wifi_nb_report.h"

/****************************** 模块常量 ******************************/

#define WIFI_STATUS_THREAD_NAME             "wifi-status"                                                           // 状态采集线程名称
#define WIFI_STATUS_REFRESH_INTERVAL_MS     800U                                                                    // 状态采集周期，单位毫秒
#define WIFI_STATUS_LOG_TAG                 "WIFI-STATUS"                                                           // 状态模块日志标签
#define WIFI_STATUS_DEBUG(fmt, ...)         LINKG_LOG_DEBUG("%s: " fmt, WIFI_STATUS_LOG_TAG, ##__VA_ARGS__)         // 调试日志
#define WIFI_STATUS_INFO(fmt, ...)          LINKG_LOG_INFO("%s: " fmt, WIFI_STATUS_LOG_TAG, ##__VA_ARGS__)          // 信息日志
#define WIFI_STATUS_WARN(fmt, ...)          LINKG_LOG_WARN("%s: " fmt, WIFI_STATUS_LOG_TAG, ##__VA_ARGS__)          // 警告日志
#define WIFI_STATUS_ERROR(fmt, ...)         LINKG_LOG_ERROR("%s: " fmt, WIFI_STATUS_LOG_TAG, ##__VA_ARGS__)         // 错误日志

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_device_role_t      role;                  // 当前设备角色
    linkg_wifi_work_mode_t   work_mode;             // 当前宽窄带工作模式
    linkg_wifi_narrow_mode_t narrow_mode;           // 窄带速率控制模式
    uint16_t                 configured_rate_level; // 固定模式配置速率档位
} wifi_status_runtime_config_t;

typedef struct
{
    pthread_mutex_t              lock;           // 状态上下文保护锁
    linkg_thread_t               thread;         // 状态采集线程
    wifi_status_runtime_config_t config;         // 状态采集所需运行配置
    wifi_status_info_t           info;           // 当前状态信息
    bool                         initialized;    // 模块是否已经初始化
    bool                         nb_initialized; // 窄带附加状态模块是否初始化
    bool                         nb_started;     // 窄带附加状态模块是否启动
} wifi_status_context_t;

/****************************** 全局上下文 ******************************/

static wifi_status_context_t g_wifi_status =
{
    .lock = PTHREAD_MUTEX_INITIALIZER // 状态上下文保护锁
};

/****************************** 上下文辅助 ******************************/

/**
 * @brief 从Wi-Fi配置提取状态采集所需运行参数。
 */
static int _wifi_status_runtime_config_set(wifi_status_runtime_config_t *runtime, linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    if (runtime == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    if (config->wideband.work_mode != LINKG_WIFI_WORK_MODE_NARROW &&
        config->wideband.work_mode != LINKG_WIFI_WORK_MODE_WIDE)
    {
        return -EINVAL;
    }

    memset(runtime, 0, sizeof(*runtime));

    runtime->role      = role;
    runtime->work_mode = config->wideband.work_mode;

    if (runtime->work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        if (config->wideband.narrow_params.mode != LINKG_WIFI_NARROW_MODE_FIXED &&
            config->wideband.narrow_params.mode != LINKG_WIFI_NARROW_MODE_ADAPTIVE)
        {
            return -EINVAL;
        }

        runtime->narrow_mode           = config->wideband.narrow_params.mode;
        runtime->configured_rate_level = config->wideband.narrow_params.manual_rate;
    }

    return 0;
}

/**
 * @brief 清空状态模块运行上下文。
 *
 * @note 调用方必须持有状态上下文锁，且必须先释放线程及窄带模块资源。
 */
static void _wifi_status_reset_context_locked(void)
{
    memset(&g_wifi_status.thread, 0, sizeof(g_wifi_status.thread));
    memset(&g_wifi_status.config, 0, sizeof(g_wifi_status.config));
    memset(&g_wifi_status.info, 0, sizeof(g_wifi_status.info));

    g_wifi_status.config.role    = LINKG_DEVICE_ROLE_UNKNOWN;
    g_wifi_status.initialized    = false;
    g_wifi_status.nb_initialized = false;
    g_wifi_status.nb_started     = false;
}

/**
 * @brief 获取当前状态采集运行配置副本。
 */
static int _wifi_status_get_runtime_config(wifi_status_runtime_config_t *runtime)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_wifi_status.lock);

    if (!g_wifi_status.initialized)
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        return -ENODEV;
    }

    *runtime = g_wifi_status.config;

    pthread_mutex_unlock(&g_wifi_status.lock);

    return 0;
}

/****************************** 状态转换 ******************************/

/**
 * @brief 获取无线接口状态名称。
 */
static const char *_wifi_status_interface_state_name(linkg_wifi_interface_state_t state)
{
    switch (state)
    {
        case LINKG_WIFI_INTERFACE_STATE_DOWN:
            return "DOWN";

        case LINKG_WIFI_INTERFACE_STATE_READY:
            return "READY";

        case LINKG_WIFI_INTERFACE_STATE_CONNECTED:
            return "CONNECTED";

        default:
            return "UNKNOWN";
    }
}

/**
 * @brief 将驱动接口状态转换为LinkG Wi-Fi接口状态。
 */
static int _wifi_status_convert_interface_state(uint8_t raw_state, linkg_wifi_interface_state_t *state)
{
    if (state == NULL)
    {
        return -EINVAL;
    }

    switch (raw_state)
    {
        case WAL_RADIO_STATE_DOWN:
            *state = LINKG_WIFI_INTERFACE_STATE_DOWN;
            return 0;

        case WAL_RADIO_STATE_READY:
            *state = LINKG_WIFI_INTERFACE_STATE_READY;
            return 0;

        case WAL_RADIO_STATE_CONNECTED:
            *state = LINKG_WIFI_INTERFACE_STATE_CONNECTED;
            return 0;

        default:
            return -EPROTO;
    }
}

/**
 * @brief 检查驱动角色是否与当前Wi-Fi角色及接口状态一致。
 */
static bool _wifi_status_role_matches(linkg_device_role_t role, linkg_wifi_interface_state_t interface_state, uint8_t raw_role)
{
    if (raw_role == WAL_RADIO_ROLE_UNKNOWN)
    {
        return interface_state == LINKG_WIFI_INTERFACE_STATE_DOWN;
    }

    if (role == LINKG_DEVICE_ROLE_AP)
    {
        return raw_role == WAL_RADIO_ROLE_AP;
    }

    if (role == LINKG_DEVICE_ROLE_STA)
    {
        return raw_role == WAL_RADIO_ROLE_STA;
    }

    return false;
}

/**
 * @brief 将驱动宽带带宽转换为LinkG宽带带宽。
 */
static linkg_wifi_wide_bandwidth_t _wifi_status_convert_wide_bandwidth(uint16_t bandwidth_mhz)
{
    switch (bandwidth_mhz)
    {
        case LINKG_WIFI_WIDE_BANDWIDTH_20_MHZ:
            return LINKG_WIFI_WIDE_BANDWIDTH_20_MHZ;

        case LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ:
            return LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ;

        case LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ:
            return LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ;

        default:
            return LINKG_WIFI_WIDE_BANDWIDTH_UNKNOWN;
    }
}

/****************************** 快照构建 ******************************/

/**
 * @brief 初始化Wi-Fi状态快照中的固定配置字段。
 */
static void _wifi_status_reset_snapshot(linkg_wifi_status_snapshot_t *snapshot, const wifi_status_runtime_config_t *runtime)
{
    if (snapshot == NULL || runtime == NULL)
    {
        return;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    snapshot->local.role            = runtime->role;
    snapshot->local.interface_state = LINKG_WIFI_INTERFACE_STATE_DOWN;
    snapshot->local.radio.work_mode = runtime->work_mode;

    if (runtime->work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        snapshot->local.radio.params.narrow.mode                  = runtime->narrow_mode;
        snapshot->local.radio.params.narrow.configured_rate_level = runtime->configured_rate_level;
        return;
    }

    snapshot->local.radio.params.wide.bandwidth = LINKG_WIFI_WIDE_BANDWIDTH_UNKNOWN;
}

/**
 * @brief 将驱动对端状态转换为Wi-Fi对端状态。
 *
 * @note statistics_allowed为false或驱动详细查询失败时，仅保留MAC、连接状态、
 *       连接时长和基础更新时间，避免向上层暴露不可置信的PHY及累计统计。
 */
static bool _wifi_status_build_peer(const wal_radio_peer_status_stru *raw_peer, uint64_t updated_ms, bool statistics_allowed, linkg_wifi_peer_status_t *peer)
{
    if (raw_peer == NULL || peer == NULL)
    {
        return false;
    }

    memset(peer, 0, sizeof(*peer));

    if ((raw_peer->flags & WAL_RADIO_PEER_FLAG_VALID) == 0U)
    {
        return false;
    }

    peer->valid            = true;
    peer->state            = LINKG_WIFI_PEER_STATE_CONNECTED;
    peer->connected_time_s = raw_peer->connected_time_s;
    peer->updated_ms       = updated_ms;

    memcpy(peer->mac, raw_peer->mac, sizeof(peer->mac));

    if (!statistics_allowed || (raw_peer->flags & WAL_RADIO_PEER_FLAG_QUERY_FAILED) != 0U)
    {
        return true;
    }

    peer->statistics_valid      = true;
    peer->rssi_dbm              = raw_peer->rssi_dbm;
    peer->tx_rate_kbps          = raw_peer->tx_rate_kbps;
    peer->rx_rate_kbps          = raw_peer->rx_rate_kbps;
    peer->driver_tx_bytes       = raw_peer->driver_tx_bytes;
    peer->driver_rx_bytes       = raw_peer->driver_rx_bytes;
    peer->driver_tx_packets     = raw_peer->driver_tx_packets;
    peer->driver_rx_packets     = raw_peer->driver_rx_packets;
    peer->driver_tx_failed      = raw_peer->driver_tx_failed;
    peer->inactive_ms           = raw_peer->inactive_ms;
    peer->statistics_updated_ms = updated_ms;

    return true;
}

/**
 * @brief 获取驱动返回的对端遍历上限。
 */
static size_t _wifi_status_get_raw_peer_count(const wal_radio_status_stru *status)
{
    size_t peer_count;

    if (status == NULL)
    {
        return 0U;
    }

    peer_count = status->peer_count;
    if (peer_count > WAL_RADIO_STATUS_MAX_PEERS)
    {
        peer_count = WAL_RADIO_STATUS_MAX_PEERS;
    }

    return peer_count;
}

/**
 * @brief 构建AP模式对端状态。
 */
static void _wifi_status_build_ap_peers(const wal_radio_status_stru *raw_status, uint64_t updated_ms, bool statistics_allowed, linkg_wifi_status_snapshot_t *snapshot)
{
    linkg_wifi_peer_status_t peer;
    size_t                   raw_index;
    size_t                   peer_index;
    size_t                   peer_count;

    if (raw_status == NULL || snapshot == NULL)
    {
        return;
    }

    peer_count = _wifi_status_get_raw_peer_count(raw_status);
    peer_index = 0U;

    snapshot->role.ap.peers_truncated = (raw_status->flags & WAL_RADIO_FLAG_PEERS_TRUNCATED) != 0U;

    for (raw_index = 0U; raw_index < peer_count && peer_index < LINKG_WIFI_AP_PEER_MAX; raw_index++)
    {
        if (!_wifi_status_build_peer(&raw_status->peers[raw_index], updated_ms, statistics_allowed, &peer))
        {
            snapshot->partial = true;
            continue;
        }

        snapshot->role.ap.peers[peer_index] = peer;
        peer_index++;
    }

    snapshot->role.ap.peer_count      = (uint8_t)peer_index;
    snapshot->role.ap.connected_count = (uint8_t)peer_index;
}

/**
 * @brief 构建STA模式当前关联AP状态。
 */
static void _wifi_status_build_sta_peer(const wal_radio_status_stru *raw_status, uint64_t updated_ms, linkg_wifi_status_snapshot_t *snapshot)
{
    linkg_wifi_peer_status_t peer;
    size_t                   raw_index;
    size_t                   peer_count;

    if (raw_status == NULL || snapshot == NULL)
    {
        return;
    }

    peer_count = _wifi_status_get_raw_peer_count(raw_status);
    if (peer_count > 1U)
    {
        snapshot->partial = true;
    }

    for (raw_index = 0U; raw_index < peer_count; raw_index++)
    {
        if (!_wifi_status_build_peer(&raw_status->peers[raw_index], updated_ms, true, &peer))
        {
            snapshot->partial = true;
            continue;
        }

        snapshot->role.sta.peer = peer;
        return;
    }
}

/**
 * @brief 将窄带附加状态合并到统一Wi-Fi状态快照。
 */
static void _wifi_status_merge_nb_report(linkg_wifi_status_snapshot_t *snapshot, const wifi_nb_report_status_t *report, bool report_valid)
{
    if (snapshot == NULL || report == NULL)
    {
        return;
    }

    if (!report_valid)
    {
        snapshot->partial = true;
        return;
    }

    snapshot->local.radio.params.narrow.current_rate_level = report->rate_level;
    snapshot->local.radio.params.narrow.current_rate_valid = true;
    snapshot->local.chip_temperature_c                     = (int32_t)report->chip_temperature_c;
    snapshot->local.chip_temperature_valid                 = true;
}

/**
 * @brief 根据驱动状态构建统一Wi-Fi状态快照。
 */
static int _wifi_status_build_snapshot(const wifi_status_runtime_config_t *runtime,
                                       linkg_wifi_interface_state_t interface_state,
                                       const wal_radio_status_stru *raw_status,
                                       uint64_t updated_ms,
                                       bool ap_statistics_duplicated,
                                       linkg_wifi_status_snapshot_t *snapshot)
{
    if (runtime == NULL || raw_status == NULL || snapshot == NULL)
    {
        return -EINVAL;
    }

    _wifi_status_reset_snapshot(snapshot, runtime);

    snapshot->partial                    = (raw_status->flags & WAL_RADIO_FLAG_PARTIAL) != 0U;
    snapshot->local.interface_state      = interface_state;
    snapshot->local.radio.frequency_mhz  = raw_status->frequency_mhz;
    snapshot->local.radio.channel        = raw_status->channel;
    snapshot->local.radio.noise_dbm      = raw_status->noise_dbm;
    snapshot->local.radio.noise_valid    = (raw_status->flags & WAL_RADIO_FLAG_NOISE_VALID) != 0U;
    snapshot->local.updated_ms           = updated_ms;

    memcpy(snapshot->local.mac, raw_status->local_mac, sizeof(snapshot->local.mac));

    if (runtime->work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        snapshot->local.radio.params.wide.bandwidth = _wifi_status_convert_wide_bandwidth(raw_status->bandwidth_mhz);
    }

    if (runtime->role == LINKG_DEVICE_ROLE_AP)
    {
        if (ap_statistics_duplicated)
        {
            snapshot->partial = true;
        }

        _wifi_status_build_ap_peers(raw_status, updated_ms, !ap_statistics_duplicated, snapshot);
        return 0;
    }

    if (runtime->role == LINKG_DEVICE_ROLE_STA)
    {
        _wifi_status_build_sta_peer(raw_status, updated_ms, snapshot);
        return 0;
    }

    return -EINVAL;
}

/****************************** 驱动数据校验 ******************************/

/**
 * @brief 判断驱动对端详细统计是否可用于重复缓存检测。
 */
static bool _wifi_status_peer_statistics_valid(const wal_radio_peer_status_stru *peer)
{
    if (peer == NULL)
    {
        return false;
    }

    if ((peer->flags & WAL_RADIO_PEER_FLAG_VALID) == 0U)
    {
        return false;
    }

    return (peer->flags & WAL_RADIO_PEER_FLAG_QUERY_FAILED) == 0U;
}

/**
 * @brief 比较两个对端是否包含相同的共享station_info统计。
 *
 * @note inactive_ms和connected_time_s由每个hmac_user独立计算，
 *       不属于共享缓存统计字段。
 */
static bool _wifi_status_peer_statistics_equal(const wal_radio_peer_status_stru *left, const wal_radio_peer_status_stru *right)
{
    if (left == NULL || right == NULL)
    {
        return false;
    }

    return left->rssi_dbm == right->rssi_dbm &&
           left->tx_rate_kbps == right->tx_rate_kbps &&
           left->rx_rate_kbps == right->rx_rate_kbps &&
           left->driver_tx_bytes == right->driver_tx_bytes &&
           left->driver_rx_bytes == right->driver_rx_bytes &&
           left->driver_tx_packets == right->driver_tx_packets &&
           left->driver_rx_packets == right->driver_rx_packets &&
           left->driver_tx_failed == right->driver_tx_failed;
}

/**
 * @brief 判断对端统计是否已经存在实际收发活动。
 */
static bool _wifi_status_peer_has_activity(const wal_radio_peer_status_stru *peer)
{
    if (peer == NULL)
    {
        return false;
    }

    return peer->driver_tx_packets != 0U ||
           peer->driver_rx_packets != 0U ||
           peer->driver_tx_failed != 0U;
}

/**
 * @brief 检测AP多对端统计是否疑似复用了同一份VAP级station_info缓存。
 */
static bool _wifi_status_ap_statistics_duplicated(const wal_radio_status_stru *status)
{
    const wal_radio_peer_status_stru *first;
    const wal_radio_peer_status_stru *peer;
    bool                              inactive_equal;
    bool                              has_activity;
    size_t                            index;
    size_t                            peer_count;

    if (status == NULL)
    {
        return false;
    }

    peer_count = _wifi_status_get_raw_peer_count(status);
    if (peer_count < 2U)
    {
        return false;
    }

    first = &status->peers[0];
    if (!_wifi_status_peer_statistics_valid(first))
    {
        return false;
    }

    inactive_equal = true;
    has_activity    = _wifi_status_peer_has_activity(first);

    for (index = 1U; index < peer_count; index++)
    {
        peer = &status->peers[index];

        if (!_wifi_status_peer_statistics_valid(peer))
        {
            return false;
        }

        if (memcmp(first->mac, peer->mac, WAL_RADIO_STATUS_MAC_LENGTH) == 0)
        {
            return false;
        }

        if (!_wifi_status_peer_statistics_equal(first, peer))
        {
            return false;
        }

        if (first->inactive_ms != peer->inactive_ms)
        {
            inactive_equal = false;
        }

        if (_wifi_status_peer_has_activity(peer))
        {
            has_activity = true;
        }
    }

    /**
     * 无流量的新连接可能合法地拥有相同RSSI和速率。
     * inactive_ms也完全相同，或者累计统计已经非零时，
     * 才将完全一致的多Peer统计判定为共享缓存污染。
     */
    return inactive_equal || has_activity;
}

/****************************** 快照保存 ******************************/

/**
 * @brief 合并新采集的Wi-Fi状态快照。
 *
 * @note STA真正进入READY或DOWN后保留最近连接AP身份及历史统计，
 *       但将其标记为DISCONNECTED；如果接口仍为CONNECTED而本轮未获得
 *       有效Peer，则不沿用旧MAC，避免把旧AP误认为当前连接对象。
 */
static void _wifi_status_merge_snapshot(linkg_wifi_status_snapshot_t *destination, const linkg_wifi_status_snapshot_t *source, linkg_device_role_t role)
{
    linkg_wifi_peer_status_t previous_peer;

    if (destination == NULL || source == NULL)
    {
        return;
    }

    if (role != LINKG_DEVICE_ROLE_STA)
    {
        *destination = *source;
        return;
    }

    previous_peer = destination->role.sta.peer;
    *destination  = *source;

    if (!source->role.sta.peer.valid &&
        previous_peer.valid &&
        source->local.interface_state != LINKG_WIFI_INTERFACE_STATE_CONNECTED)
    {
        destination->role.sta.peer                  = previous_peer;
        destination->role.sta.peer.state            = LINKG_WIFI_PEER_STATE_DISCONNECTED;
        destination->role.sta.peer.statistics_valid = false;
        destination->role.sta.peer.updated_ms       = source->local.updated_ms;
    }
}

/**
 * @brief 记录一次状态采集失败。
 */
static void _wifi_status_record_failure(int error, uint64_t attempt_ms)
{
    int previous_error;

    pthread_mutex_lock(&g_wifi_status.lock);

    previous_error                     = g_wifi_status.info.last_error;
    g_wifi_status.info.last_attempt_ms = attempt_ms;
    g_wifi_status.info.last_error      = error;

    pthread_mutex_unlock(&g_wifi_status.lock);

    if (previous_error != error)
    {
        WIFI_STATUS_WARN("status collection failed, error=%d", error);
        return;
    }

    WIFI_STATUS_DEBUG("status collection still failing, error=%d", error);
}

/**
 * @brief 保存一次成功采集的Wi-Fi状态快照。
 */
static void _wifi_status_record_success(const linkg_wifi_status_snapshot_t *snapshot, uint64_t updated_ms)
{
    linkg_wifi_interface_state_t previous_state;
    linkg_wifi_interface_state_t current_state;
    uint64_t                     generation;
    int                          previous_error;
    bool                         previous_valid;

    if (snapshot == NULL)
    {
        return;
    }

    current_state = snapshot->local.interface_state;

    pthread_mutex_lock(&g_wifi_status.lock);

    previous_valid = g_wifi_status.info.valid;
    previous_state = g_wifi_status.info.snapshot.local.interface_state;
    previous_error = g_wifi_status.info.last_error;

    _wifi_status_merge_snapshot(&g_wifi_status.info.snapshot, snapshot, g_wifi_status.config.role);

    g_wifi_status.info.valid           = true;
    g_wifi_status.info.generation++;
    g_wifi_status.info.last_attempt_ms = updated_ms;
    g_wifi_status.info.last_success_ms = updated_ms;
    g_wifi_status.info.last_error      = 0;
    generation                         = g_wifi_status.info.generation;

    pthread_mutex_unlock(&g_wifi_status.lock);

    if (!previous_valid)
    {
        WIFI_STATUS_DEBUG("initial status collected, state=%s, generation=%llu",
                          _wifi_status_interface_state_name(current_state),
                          (unsigned long long)generation);
        return;
    }

    if (previous_error != 0)
    {
        WIFI_STATUS_INFO("status collection recovered, state=%s, generation=%llu",
                         _wifi_status_interface_state_name(current_state),
                         (unsigned long long)generation);
    }

    if (previous_state != current_state)
    {
        WIFI_STATUS_DEBUG("interface state changed, old=%s, new=%s",
                          _wifi_status_interface_state_name(previous_state),
                          _wifi_status_interface_state_name(current_state));
    }
}

/****************************** 状态采集 ******************************/

/**
 * @brief 从Wi-Fi平台层采集一次状态并更新内部快照。
 */
static int _wifi_status_refresh(void)
{
    wifi_status_runtime_config_t  runtime;
    wal_radio_status_stru         raw_status;
    linkg_wifi_status_snapshot_t  snapshot;
    linkg_wifi_interface_state_t  interface_state;
    wifi_nb_report_status_t       nb_report;
    uint64_t                      updated_ms;
    bool                          ap_statistics_duplicated;
    bool                          nb_report_valid;
    bool                          query_nb_report;
    int                           nb_ret;
    int                           ret;

    ret = _wifi_status_get_runtime_config(&runtime);
    if (ret != 0)
    {
        return ret;
    }

    memset(&raw_status, 0, sizeof(raw_status));
    memset(&nb_report, 0, sizeof(nb_report));

    raw_status.version     = WAL_RADIO_STATUS_ABI_VERSION;
    raw_status.struct_size = (uint16_t)sizeof(raw_status);

    ret = wifi_driver_get_radio_status(&raw_status);
    if (ret != 0)
    {
        goto failure;
    }

    ret = _wifi_status_convert_interface_state(raw_status.state, &interface_state);
    if (ret != 0)
    {
        goto failure;
    }

    if (!_wifi_status_role_matches(runtime.role, interface_state, raw_status.role))
    {
        ret = -EPROTO;
        goto failure;
    }

    ap_statistics_duplicated = runtime.role == LINKG_DEVICE_ROLE_AP &&
                               _wifi_status_ap_statistics_duplicated(&raw_status);

    if (ap_statistics_duplicated)
    {
        WIFI_STATUS_DEBUG("AP peer statistics suspected duplicated, peers=%u", raw_status.peer_count);
    }

    query_nb_report = runtime.work_mode == LINKG_WIFI_WORK_MODE_NARROW &&
                      interface_state != LINKG_WIFI_INTERFACE_STATE_DOWN;
    nb_report_valid = false;

    if (query_nb_report)
    {
        nb_ret = wifi_nb_report_get_status(&nb_report);
        nb_report_valid = nb_ret == 0;

        if (nb_ret != 0)
        {
            WIFI_STATUS_DEBUG("narrow report unavailable, error=%d", nb_ret);
        }
    }

    updated_ms = linkg_time_elapsed_ms();

    ret = _wifi_status_build_snapshot(&runtime,
                                      interface_state,
                                      &raw_status,
                                      updated_ms,
                                      ap_statistics_duplicated,
                                      &snapshot);
    if (ret != 0)
    {
        goto failure_at_time;
    }

    if (query_nb_report)
    {
        _wifi_status_merge_nb_report(&snapshot, &nb_report, nb_report_valid);
    }

    _wifi_status_record_success(&snapshot, updated_ms);

    return 0;

failure:
    updated_ms = linkg_time_elapsed_ms();

failure_at_time:
    _wifi_status_record_failure(ret, updated_ms);

    return ret;
}

/**
 * @brief 等待下一次状态采集周期或线程停止唤醒。
 */
static int _wifi_status_wait(linkg_thread_t *thread)
{
    struct pollfd descriptor;
    int           wakeup_fd;
    int           ret;

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    memset(&descriptor, 0, sizeof(descriptor));

    descriptor.fd     = wakeup_fd;
    descriptor.events = POLLIN;

    do
    {
        ret = poll(&descriptor, 1U, (int)WIFI_STATUS_REFRESH_INTERVAL_MS);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 0)
    {
        return 0;
    }

    if ((descriptor.revents & POLLIN) != 0)
    {
        return linkg_thread_clear_wakeup(thread);
    }

    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief Wi-Fi状态采集线程。
 */
static void _wifi_status_thread(linkg_thread_t *thread, void *user_data)
{
    int ret;

    (void)user_data;

    WIFI_STATUS_DEBUG("status collection thread entered");

    while (linkg_thread_is_running(thread))
    {
        (void)_wifi_status_refresh();

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        ret = _wifi_status_wait(thread);
        if (ret == 0)
        {
            continue;
        }

        if (linkg_thread_is_running(thread))
        {
            WIFI_STATUS_ERROR("status collection wait failed, error=%d", ret);
        }

        break;
    }

    pthread_mutex_lock(&g_wifi_status.lock);
    g_wifi_status.info.running = false;
    pthread_mutex_unlock(&g_wifi_status.lock);

    WIFI_STATUS_DEBUG("status collection thread exited");
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi状态采集模块。
 *
 * @note role和config在本次生命周期内保持固定；运行期切换宽窄带模式时，
 *       应由Wi-Fi顶层生命周期停止并重新初始化本模块。
 */
int wifi_status_init(linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    wifi_status_runtime_config_t runtime;
    int                          ret;

    ret = _wifi_status_runtime_config_set(&runtime, role, config);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_wifi_status.lock);

    if (g_wifi_status.initialized)
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        return -EALREADY;
    }

    _wifi_status_reset_context_locked();

    g_wifi_status.config          = runtime;
    g_wifi_status.info.last_error = -EAGAIN;

    _wifi_status_reset_snapshot(&g_wifi_status.info.snapshot, &runtime);

    ret = linkg_thread_init(&g_wifi_status.thread, WIFI_STATUS_THREAD_NAME, _wifi_status_thread, NULL);
    if (ret != 0)
    {
        _wifi_status_reset_context_locked();
        pthread_mutex_unlock(&g_wifi_status.lock);
        return ret;
    }

    if (runtime.work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        ret = wifi_nb_report_init();
        if (ret != 0)
        {
            linkg_thread_deinit(&g_wifi_status.thread);
            _wifi_status_reset_context_locked();
            pthread_mutex_unlock(&g_wifi_status.lock);
            return ret;
        }

        g_wifi_status.nb_initialized = true;
    }

    g_wifi_status.initialized = true;

    pthread_mutex_unlock(&g_wifi_status.lock);

    WIFI_STATUS_DEBUG("status module initialized, role=%d, interval_ms=%u",
                      role,
                      WIFI_STATUS_REFRESH_INTERVAL_MS);

    return 0;
}

/**
 * @brief 启动Wi-Fi状态采集模块。
 */
int wifi_status_start(void)
{
    int rollback_ret;
    int ret;

    pthread_mutex_lock(&g_wifi_status.lock);

    if (!g_wifi_status.initialized)
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        return -ENODEV;
    }

    if (linkg_thread_is_started(&g_wifi_status.thread))
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        return -EALREADY;
    }

    if (g_wifi_status.nb_initialized)
    {
        ret = wifi_nb_report_start();
        if (ret != 0)
        {
            pthread_mutex_unlock(&g_wifi_status.lock);
            return ret;
        }

        g_wifi_status.nb_started = true;
    }

    g_wifi_status.info.running = true;

    ret = linkg_thread_start(&g_wifi_status.thread);
    if (ret == 0)
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        WIFI_STATUS_DEBUG("status module started");
        return 0;
    }

    g_wifi_status.info.running = false;

    if (g_wifi_status.nb_started)
    {
        rollback_ret = wifi_nb_report_stop();
        g_wifi_status.nb_started = false;

        if (rollback_ret != 0)
        {
            WIFI_STATUS_WARN("rollback narrow report start failed, error=%d", rollback_ret);
        }
    }

    pthread_mutex_unlock(&g_wifi_status.lock);

    return ret;
}

/**
 * @brief 停止Wi-Fi状态采集模块。
 *
 * @note 必须先停止采集线程，再停止窄带Netlink状态源，避免采集线程并发使用已关闭fd。
 */
int wifi_status_stop(void)
{
    bool initialized;
    bool started;
    bool nb_started;
    int  ret;

    pthread_mutex_lock(&g_wifi_status.lock);

    initialized = g_wifi_status.initialized;
    started     = initialized && linkg_thread_is_started(&g_wifi_status.thread);
    nb_started  = initialized && g_wifi_status.nb_started;

    pthread_mutex_unlock(&g_wifi_status.lock);

    if (!initialized)
    {
        return 0;
    }

    if (started)
    {
        ret = linkg_thread_stop(&g_wifi_status.thread);
        if (ret != 0)
        {
            if (linkg_thread_is_started(&g_wifi_status.thread))
            {
                return ret;
            }

            WIFI_STATUS_WARN("status thread stopped with cleanup error, error=%d", ret);
        }
    }

    pthread_mutex_lock(&g_wifi_status.lock);
    g_wifi_status.info.running = false;
    pthread_mutex_unlock(&g_wifi_status.lock);

    if (nb_started)
    {
        ret = wifi_nb_report_stop();

        pthread_mutex_lock(&g_wifi_status.lock);
        g_wifi_status.nb_started = false;
        pthread_mutex_unlock(&g_wifi_status.lock);

        if (ret != 0)
        {
            return ret;
        }
    }

    WIFI_STATUS_DEBUG("status module stopped");

    return 0;
}

/**
 * @brief 反初始化Wi-Fi状态采集模块。
 */
int wifi_status_deinit(void)
{
    int ret;

    ret = wifi_status_stop();
    if (ret != 0)
    {
        WIFI_STATUS_ERROR("stop status module before deinit failed, error=%d", ret);
        return ret;
    }

    pthread_mutex_lock(&g_wifi_status.lock);

    if (!g_wifi_status.initialized)
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        return 0;
    }

    linkg_thread_deinit(&g_wifi_status.thread);

    if (g_wifi_status.nb_initialized)
    {
        wifi_nb_report_deinit();
    }

    _wifi_status_reset_context_locked();

    pthread_mutex_unlock(&g_wifi_status.lock);

    WIFI_STATUS_DEBUG("status module deinitialized");

    return 0;
}

/****************************** 状态读取 ******************************/

/**
 * @brief 获取当前Wi-Fi状态信息副本。
 */
int wifi_status_get_info(wifi_status_info_t *info)
{
    if (info == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_wifi_status.lock);

    if (!g_wifi_status.initialized)
    {
        pthread_mutex_unlock(&g_wifi_status.lock);
        return -ENODEV;
    }

    *info = g_wifi_status.info;

    pthread_mutex_unlock(&g_wifi_status.lock);

    return 0;
}
