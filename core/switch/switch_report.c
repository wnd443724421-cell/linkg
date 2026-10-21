/**
 * @file switch_report.c
 * @brief LinkG链路切换Wi-Fi和Cellular质量上报实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-16
 */

#include "switch_report.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular_link.h"
#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_wifi_link.h"

#include "switch_internal.h"
#include "switch_tx.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_REPORT_INTERVAL_US           250000ULL   // AP质量报告周期
#define LINKG_SWITCH_REPORT_MESSAGE_ID_HALF_RANGE 0x80000000U // 32位消息编号前后关系半区间

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_path_stats_t                 tx_stats;       // 当前Peer Path累计发送统计
    linkg_switch_loss_class_sampler_t  classes[LINKG_TRANSPORT_CLASS_COUNT]; // 三类业务Wire累计计数
    uint64_t                           received;       // 当前Peer Wire累计唯一接收帧数
    uint64_t                           lost;           // 当前Peer Wire累计确认丢失帧数
    bool                               available;      // 对应Link运行且Peer Path存在
    bool                               tx_stats_valid; // Path发送累计统计有效
    bool                               rx_stats_valid; // Wire接收累计统计有效
} linkg_switch_report_path_sample_t;

typedef struct
{
    linkg_switch_report_path_sample_t wifi;     // 当前Peer Wi-Fi统计
    linkg_switch_report_path_sample_t cellular; // 当前Peer Cellular统计
} linkg_switch_report_sample_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断运行状态变化引起的错误是否属于周期上报正常情况。
 */
static bool _linkg_switch_report_expected_state_error(int error)
{
    return error == -ENOENT ||
           error == -ENODEV ||
           error == -ENETDOWN ||
           error == -EAGAIN ||
           error == -ESTALE;
}

/**
 * @brief 记录本轮第一个非预期错误。
 */
static void _linkg_switch_report_record_error(int error, int *first_error)
{
    if (first_error == NULL || error == 0 || _linkg_switch_report_expected_state_error(error))
    {
        return;
    }

    if (*first_error == 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 将64位计数饱和转换为32位无符号整数。
 */
static uint32_t _linkg_switch_report_u32_saturate(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/**
 * @brief 64位无符号饱和加法。
 */
static uint64_t _linkg_switch_report_u64_add(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

/**
 * @brief 校验单项报告，丢包数量不得超过总样本数量。
 */
static bool _linkg_switch_report_loss_valid(const linkg_switch_wire_loss_t *loss)
{
    return loss != NULL &&
           loss->lost_packets <= loss->sample_packets;
}

/**
 * @brief 校验完整质量报告的四项整链路指标及六项分类Wire指标。
 */
static bool _linkg_switch_report_quality_valid(const linkg_switch_wire_quality_report_t *report)
{
    uint32_t class_index;

    if (report == NULL ||
        !_linkg_switch_report_loss_valid(&report->wifi_uplink) ||
        !_linkg_switch_report_loss_valid(&report->wifi_downlink_load) ||
        !_linkg_switch_report_loss_valid(&report->cellular_uplink) ||
        !_linkg_switch_report_loss_valid(&report->cellular_downlink_load))
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (!_linkg_switch_report_loss_valid(&report->wifi_uplink_classes[class_index]) ||
            !_linkg_switch_report_loss_valid(&report->cellular_uplink_classes[class_index]))
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 判断新消息编号是否晚于已接收编号，支持32位回绕。
 */
static bool _linkg_switch_report_message_id_newer(uint32_t message_id, uint32_t current_message_id)
{
    uint32_t delta;

    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return false;
    }

    if (current_message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return true;
    }

    delta = message_id - current_message_id;

    return delta != 0U && delta < LINKG_SWITCH_REPORT_MESSAGE_ID_HALF_RANGE;
}

/**
 * @brief 为指定AP Peer分配非零质量报告编号，调用方持有Switch锁。
 */
static uint32_t _linkg_switch_report_allocate_message_id_locked(linkg_switch_ap_peer_runtime_t *runtime)
{
    uint32_t message_id;

    message_id = ++runtime->report_message_id;
    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        message_id = ++runtime->report_message_id;
    }

    return message_id;
}

/**
 * @brief 收集指定Peer在单条Link上的Path统计及对应Wire接收统计。
 *
 * 不持有Switch锁读取Path和RX统计，避免跨模块锁嵌套。
 * 无Path或暂无RX报文不视为错误，统计有效性分别记录。
 */
static int _linkg_switch_report_collect_path(uint8_t peer_node_id, uint32_t link_id, bool cellular, linkg_switch_report_path_sample_t *sample)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    uint32_t              class_index;
    int                   first_error;
    int                   ret;

    if (sample == NULL)
    {
        return -EINVAL;
    }

    memset(sample, 0, sizeof(*sample));

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    link = linkg_link_manager_get(link_id);
    if (link == NULL || !linkg_link_is_running(link))
    {
        return 0;
    }

    path = NULL;
    memset(&endpoint, 0, sizeof(endpoint));

    ret = linkg_node_acquire_path(peer_node_id, link_id, &path, &endpoint);
    if (_linkg_switch_report_expected_state_error(ret))
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    sample->available = true;

    ret = linkg_path_get_stats(path, &sample->tx_stats);
    if (ret == 0)
    {
        sample->tx_stats_valid = true;
    }

    first_error = 0;
    _linkg_switch_report_record_error(ret, &first_error);
    linkg_path_release(path);

    if (cellular)
    {
        linkg_cellular_rx_stats_t stats;

        ret = linkg_cellular_link_get_rx_stats(link, peer_node_id, &stats);
        if (ret == 0)
        {
            sample->received       = stats.received_packets;
            sample->lost           = stats.confirmed_lost_packets;
            sample->rx_stats_valid = true;

            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                sample->classes[class_index].lost_packets   = stats.classes[class_index].lost_packets;
                sample->classes[class_index].sample_packets = stats.classes[class_index].sample_packets;
            }
        }
    }
    else
    {
        linkg_wifi_rx_stats_t stats;

        ret = linkg_wifi_link_get_rx_stats(link, peer_node_id, &stats);
        if (ret == 0)
        {
            sample->received       = stats.received_packets;
            sample->lost           = stats.confirmed_lost_packets;
            sample->rx_stats_valid = true;

            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                sample->classes[class_index].lost_packets   = stats.classes[class_index].lost_packets;
                sample->classes[class_index].sample_packets = stats.classes[class_index].sample_packets;
            }
        }
    }

    _linkg_switch_report_record_error(ret, &first_error);

    return first_error;
}

/**
 * @brief 对当前直接Peer分别采集Wi-Fi和Cellular统计。
 */
static int _linkg_switch_report_collect_sample(uint8_t peer_node_id, uint32_t wifi_link_id, uint32_t cellular_link_id, linkg_switch_report_sample_t *sample)
{
    int first_error;
    int ret;

    if (sample == NULL)
    {
        return -EINVAL;
    }

    memset(sample, 0, sizeof(*sample));
    first_error = 0;

    ret = _linkg_switch_report_collect_path(peer_node_id, wifi_link_id, false, &sample->wifi);
    _linkg_switch_report_record_error(ret, &first_error);

    ret = _linkg_switch_report_collect_path(peer_node_id, cellular_link_id, true, &sample->cellular);
    _linkg_switch_report_record_error(ret, &first_error);

    return first_error;
}

/**
 * @brief 将有效Wire累计统计转换成当前窗口精确丢包数量。
 *
 * 调用方已经持有Switch锁。
 * 只使用当前窗口差分，不累计历史窗口样本。
 */
static void _linkg_switch_report_build_wire_locked(linkg_switch_loss_sampler_t *sampler, uint32_t link_id, const linkg_switch_report_path_sample_t *sample, linkg_switch_wire_loss_t *loss, linkg_switch_wire_loss_t classes[LINKG_TRANSPORT_CLASS_COUNT])
{
    uint64_t received_delta;
    uint64_t lost_delta;
    uint64_t total_delta;
    uint64_t sample_delta;
    uint64_t class_lost_delta;
    uint32_t class_index;
    bool     rebase;

    memset(loss, 0, sizeof(*loss));
    memset(classes, 0, LINKG_TRANSPORT_CLASS_COUNT * sizeof(classes[0]));

    if (!sample->available || !sample->rx_stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    rebase = !sampler->initialized ||
             sampler->link_id != link_id ||
             sample->received < sampler->received_packets ||
             sample->lost < sampler->lost_packets;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (sample->classes[class_index].lost_packets > sample->classes[class_index].sample_packets ||
            sample->classes[class_index].lost_packets < sampler->classes[class_index].lost_packets ||
            sample->classes[class_index].sample_packets < sampler->classes[class_index].sample_packets)
        {
            rebase = true;
        }
    }

    if (!rebase)
    {
        received_delta = sample->received - sampler->received_packets;
        lost_delta     = sample->lost - sampler->lost_packets;
        total_delta    = _linkg_switch_report_u64_add(received_delta, lost_delta);

        if (total_delta != 0U)
        {
            loss->lost_packets   = _linkg_switch_report_u32_saturate(lost_delta);
            loss->sample_packets = _linkg_switch_report_u32_saturate(total_delta);
        }

        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            class_lost_delta = sample->classes[class_index].lost_packets - sampler->classes[class_index].lost_packets;
            sample_delta     = sample->classes[class_index].sample_packets - sampler->classes[class_index].sample_packets;

            if (sample_delta != 0U)
            {
                classes[class_index].lost_packets   = _linkg_switch_report_u32_saturate(class_lost_delta);
                classes[class_index].sample_packets = _linkg_switch_report_u32_saturate(sample_delta);
            }
        }
    }

    sampler->initialized      = true;
    sampler->link_id          = link_id;
    sampler->received_packets = sample->received;
    sampler->lost_packets     = sample->lost;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        sampler->classes[class_index] = sample->classes[class_index];
    }
}

/**
 * @brief 将发送端Path累计统计转换成当前窗口Load丢包数量。
 *
 * 调用方已经持有Switch锁。
 * Wi-Fi和Cellular统一统计TX Queue Overflow。
 */
static void _linkg_switch_report_build_load_locked(linkg_switch_load_sampler_t *sampler, uint32_t link_id, const linkg_switch_report_path_sample_t *sample, linkg_switch_wire_loss_t *loss)
{
    uint64_t sent_delta;
    uint64_t overflow_delta;
    uint64_t total_delta;

    memset(loss, 0, sizeof(*loss));

    if (!sample->available || !sample->tx_stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized ||
        sampler->link_id != link_id ||
        sample->tx_stats.tx_packets < sampler->sent_packets ||
        sample->tx_stats.tx_queue_overflow_packets < sampler->queue_overflow_packets)
    {
        sampler->initialized            = true;
        sampler->link_id                = link_id;
        sampler->sent_packets           = sample->tx_stats.tx_packets;
        sampler->queue_overflow_packets = sample->tx_stats.tx_queue_overflow_packets;
        return;
    }

    sent_delta     = sample->tx_stats.tx_packets - sampler->sent_packets;
    overflow_delta = sample->tx_stats.tx_queue_overflow_packets - sampler->queue_overflow_packets;
    total_delta    = _linkg_switch_report_u64_add(sent_delta, overflow_delta);

    if (total_delta != 0U)
    {
        loss->lost_packets   = _linkg_switch_report_u32_saturate(overflow_delta);
        loss->sample_packets = _linkg_switch_report_u32_saturate(total_delta);
    }

    sampler->sent_packets           = sample->tx_stats.tx_packets;
    sampler->queue_overflow_packets = sample->tx_stats.tx_queue_overflow_packets;
}

/**
 * @brief 将单项报告复制为本地精确丢包观测缓存。
 *
 * 调用方已经持有Switch锁。
 */
static void _linkg_switch_report_publish_loss_locked(linkg_switch_loss_observation_t *observation, const linkg_switch_wire_loss_t *loss, uint64_t now_us)
{
    observation->valid          = loss->sample_packets != 0U;
    observation->lost_packets   = loss->lost_packets;
    observation->sample_packets = loss->sample_packets;
    observation->updated_us     = now_us;
}

/**
 * @brief 根据整链路及三业务差分基线构建当前Peer质量报告，调用方持有Switch锁。
 */
static void _linkg_switch_report_build_locked(linkg_switch_ap_peer_runtime_t *runtime, uint32_t wifi_link_id, uint32_t cellular_link_id, const linkg_switch_report_sample_t *sample, uint64_t now_us, linkg_switch_wire_quality_report_t *report)
{
    memset(report, 0, sizeof(*report));

    _linkg_switch_report_build_wire_locked(&runtime->wifi_uplink_loss_sampler, wifi_link_id, &sample->wifi, &report->wifi_uplink, report->wifi_uplink_classes);
    _linkg_switch_report_build_load_locked(&runtime->wifi_downlink_load_sampler, wifi_link_id, &sample->wifi, &report->wifi_downlink_load);
    _linkg_switch_report_build_wire_locked(&runtime->cellular_uplink_loss_sampler, cellular_link_id, &sample->cellular, &report->cellular_uplink, report->cellular_uplink_classes);
    _linkg_switch_report_build_load_locked(&runtime->cellular_downlink_load_sampler, cellular_link_id, &sample->cellular, &report->cellular_downlink_load);

    runtime->wifi_path_available     = sample->wifi.available;
    runtime->cellular_path_available = sample->cellular.available;
    runtime->path_updated_us         = now_us;

    _linkg_switch_report_publish_loss_locked(&runtime->wifi_uplink_loss, &report->wifi_uplink, now_us);
    _linkg_switch_report_publish_loss_locked(&runtime->wifi_downlink_load, &report->wifi_downlink_load, now_us);
    _linkg_switch_report_publish_loss_locked(&runtime->cellular_uplink_loss, &report->cellular_uplink, now_us);
    _linkg_switch_report_publish_loss_locked(&runtime->cellular_downlink_load, &report->cellular_downlink_load, now_us);
}

/**
 * @brief 对指定直接STA执行一次完整的质量采集和上报。
 */
static int _linkg_switch_report_process_peer(uint8_t peer_node_id, uint32_t peer_generation, uint32_t wifi_link_id, uint32_t cellular_link_id, linkg_packet_pool_t *packet_pool, uint64_t now_us)
{
    linkg_switch_wire_quality_report_t report;
    linkg_switch_report_sample_t       sample;
    linkg_switch_ap_peer_runtime_t    *runtime;
    linkg_switch_peer_runtime_t       *peer;
    uint32_t                           message_id;
    int                                collect_error;
    int                                ret;

    collect_error = _linkg_switch_report_collect_sample(peer_node_id, wifi_link_id, cellular_link_id, &sample);

    message_id = LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID;
    memset(&report, 0, sizeof(report));

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

    if (g_switch.role != LINKG_DEVICE_ROLE_AP)
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

    runtime = &peer->role.ap;

    _linkg_switch_report_build_locked(runtime, wifi_link_id, cellular_link_id, &sample, now_us, &report);

    if (!sample.wifi.available && !sample.cellular.available)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return collect_error;
    }

    message_id = _linkg_switch_report_allocate_message_id_locked(runtime);

    pthread_mutex_unlock(&g_switch.lock);

    if (!linkg_switch_peer_generation_current(peer_node_id, peer_generation))
    {
        return -ESTALE;
    }

    ret = linkg_switch_tx_send_quality_report(packet_pool, peer_node_id, message_id, &report);
    if (ret != 0)
    {
        return ret;
    }

    return collect_error;
}

/****************************** 周期处理 ******************************/

/**
 * @brief AP每250ms向全部有效直接STA上报各自的Wi-Fi和Cellular质量。
 */
int linkg_switch_report_process(uint64_t now_us)
{
    linkg_packet_pool_t *packet_pool;
    uint8_t              peer_node_ids[LINKG_SWITCH_PEER_MAX];
    uint32_t             peer_generations[LINKG_SWITCH_PEER_MAX];
    uint32_t             cellular_link_id;
    uint32_t             wifi_link_id;
    uint32_t             peer_count;
    uint32_t             index;
    int                  first_error;
    int                  ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    peer_count  = 0U;
    first_error = 0;

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

    if (g_switch.role != LINKG_DEVICE_ROLE_AP)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    if (g_switch.next_report_us != 0U && now_us < g_switch.next_report_us)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    g_switch.next_report_us = now_us + LINKG_SWITCH_REPORT_INTERVAL_US;
    packet_pool             = g_switch.packet_pool;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        if (!g_switch.peers[index].used)
        {
            continue;
        }

        peer_node_ids[peer_count]    = g_switch.peers[index].peer_node_id;
        peer_generations[peer_count] = g_switch.peers[index].generation;
        peer_count++;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    wifi_link_id     = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    cellular_link_id = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    for (index = 0U; index < peer_count; index++)
    {
        ret = _linkg_switch_report_process_peer(peer_node_ids[index],
                                                 peer_generations[index],
                                                 wifi_link_id,
                                                 cellular_link_id,
                                                 packet_pool,
                                                 now_us);
        _linkg_switch_report_record_error(ret, &first_error);
    }

    return first_error;
}

/**
 * @brief 获取AP下一轮质量报告截止时间，调用方持有Switch锁。
 */
uint64_t linkg_switch_report_next_deadline_locked(void)
{
    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_AP)
    {
        return UINT64_MAX;
    }

    return g_switch.next_report_us;
}

/****************************** 数据接收 ******************************/

/**
 * @brief STA接收AP发来的Wi-Fi和Cellular质量报告。
 *
 * 重复或乱序的报告直接忽略，全部质量指标使用同一个Message ID。
 */
int linkg_switch_report_receive_quality(uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_quality_report_t *report)
{
    linkg_switch_sta_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t     *peer;
    uint64_t                         now_us;
    uint32_t                         class_index;

    if (!_linkg_switch_report_quality_valid(report))
    {
        return -EINVAL;
    }

    if (peer_node_id < LINKG_RESOURCE_NODE_ID_MIN || peer_node_id > LINKG_RESOURCE_NODE_ID_MAX ||
        message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
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

    if (!_linkg_switch_report_message_id_newer(message_id, runtime->remote_quality_report_id))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    runtime->remote_quality_report_id = message_id;

    _linkg_switch_report_publish_loss_locked(&runtime->remote_wifi_uplink_loss, &report->wifi_uplink, now_us);
    _linkg_switch_report_publish_loss_locked(&runtime->remote_wifi_downlink_load, &report->wifi_downlink_load, now_us);
    _linkg_switch_report_publish_loss_locked(&runtime->remote_cellular_uplink_loss, &report->cellular_uplink, now_us);
    _linkg_switch_report_publish_loss_locked(&runtime->remote_cellular_downlink_load, &report->cellular_downlink_load, now_us);

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        _linkg_switch_report_publish_loss_locked(&runtime->remote_wifi_uplink_classes[class_index],
                                                  &report->wifi_uplink_classes[class_index],
                                                  now_us);

        _linkg_switch_report_publish_loss_locked(&runtime->remote_cellular_uplink_classes[class_index],
                                                  &report->cellular_uplink_classes[class_index],
                                                  now_us);
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}
