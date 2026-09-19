/**
 * @file switch_report.c
 * @brief LinkG链路切换Wi-Fi质量上报实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-09-19
 */

#include "switch_report.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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

#define LINKG_SWITCH_REPORT_INTERVAL_US           250000ULL   // AP Wi-Fi质量报告周期
#define LINKG_SWITCH_REPORT_MESSAGE_ID_HALF_RANGE 0x80000000U // 32位消息编号前后关系半区间

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_wifi_rx_stats_t stats;                   // 当前Peer Wi-Fi累计接收统计
    bool                  wifi_path_available;     // 当前Peer Wi-Fi Path是否存在
    bool                  cellular_path_available; // 当前Peer Cellular Path是否存在
    bool                  stats_valid;             // 当前Wi-Fi累计接收统计是否有效
} linkg_switch_report_sample_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断运行状态变化导致的错误是否属于周期上报正常情况。
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
    if (first_error == NULL || error == 0)
    {
        return;
    }

    if (_linkg_switch_report_expected_state_error(error))
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
static uint32_t _linkg_switch_report_u32_saturate(uint64_t value)
{
    return value > UINT32_MAX ? UINT32_MAX : (uint32_t)value;
}

/**
 * @brief 计算当前窗口千分比丢包率。
 */
static uint32_t _linkg_switch_report_loss_permille(uint64_t lost_packets, uint64_t sample_packets)
{
    if (sample_packets == 0U || lost_packets == 0U)
    {
        return 0U;
    }

    if (lost_packets >= sample_packets)
    {
        return 1000U;
    }

    return (uint32_t)((lost_packets * 1000ULL) / sample_packets);
}

/**
 * @brief 判断新消息编号是否晚于已接收编号，并正确处理32位回绕。
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

    return delta != 0U &&
           delta < LINKG_SWITCH_REPORT_MESSAGE_ID_HALF_RANGE;
}

/**
 * @brief 为指定AP Peer分配非零Wi-Fi质量报告消息编号，调用方持有Switch锁。
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
 * @brief 查询指定Peer在目标Link上的Path当前是否存在且Link处于运行态。
 */
static int _linkg_switch_report_collect_path_available(uint8_t peer_node_id, uint32_t link_id, bool *available)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    int                   ret;

    if (available == NULL)
    {
        return -EINVAL;
    }

    *available = false;

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
    if (_linkg_switch_report_expected_state_error(ret))
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    *available = true;

    linkg_path_release(path);

    return 0;
}

/**
 * @brief 读取指定Peer当前Wi-Fi接收统计和Wi-Fi、Cellular Path状态。
 */
static int _linkg_switch_report_collect_sample(uint8_t peer_node_id, uint32_t wifi_link_id, uint32_t cellular_link_id, linkg_switch_report_sample_t *sample)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    int                   ret;

    if (sample == NULL)
    {
        return -EINVAL;
    }

    memset(sample, 0, sizeof(*sample));

    ret = _linkg_switch_report_collect_path_available(peer_node_id,
                                                       cellular_link_id,
                                                       &sample->cellular_path_available);
    if (ret != 0)
    {
        return ret;
    }

    if (wifi_link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    link = linkg_link_manager_get(wifi_link_id);
    if (link == NULL || !linkg_link_is_running(link))
    {
        return 0;
    }

    memset(&endpoint, 0, sizeof(endpoint));
    path = NULL;

    ret = linkg_node_acquire_path(peer_node_id, wifi_link_id, &path, &endpoint);
    if (_linkg_switch_report_expected_state_error(ret))
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    sample->wifi_path_available = true;

    linkg_path_release(path);

    ret = linkg_wifi_link_get_rx_stats(link, peer_node_id, &sample->stats);
    if (ret == -ENOENT)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    sample->stats_valid = true;

    return 0;
}

/**
 * @brief 根据AP累计Wi-Fi接收统计生成本轮STA上行质量报告，调用方持有Switch锁。
 */
static void _linkg_switch_report_build_locked(linkg_switch_ap_peer_runtime_t *runtime, uint32_t wifi_link_id, const linkg_switch_report_sample_t *sample, linkg_switch_wire_wifi_quality_report_t *report)
{
    linkg_switch_loss_sampler_t *sampler;
    uint64_t                     received_delta;
    uint64_t                     lost_delta;
    uint64_t                     sample_packets;

    memset(report, 0, sizeof(*report));
    sampler = &runtime->wifi_uplink_loss_sampler;

    if (!sample->wifi_path_available)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sample->stats_valid)
    {
        memset(sampler, 0, sizeof(*sampler));
        return;
    }

    if (!sampler->initialized ||
        sampler->link_id != wifi_link_id ||
        sample->stats.received_packets < sampler->received_packets ||
        sample->stats.confirmed_lost_packets < sampler->lost_packets)
    {
        sampler->initialized      = true;
        sampler->link_id          = wifi_link_id;
        sampler->received_packets = sample->stats.received_packets;
        sampler->lost_packets     = sample->stats.confirmed_lost_packets;
        return;
    }

    received_delta = sample->stats.received_packets - sampler->received_packets;
    lost_delta     = sample->stats.confirmed_lost_packets - sampler->lost_packets;

    if (UINT64_MAX - received_delta < lost_delta)
    {
        sample_packets = UINT64_MAX;
    }
    else
    {
        sample_packets = received_delta + lost_delta;
    }

    if (sample_packets != 0U)
    {
        report->loss_permille  = _linkg_switch_report_loss_permille(lost_delta, sample_packets);
        report->sample_packets = _linkg_switch_report_u32_saturate(sample_packets);
    }

    sampler->received_packets = sample->stats.received_packets;
    sampler->lost_packets     = sample->stats.confirmed_lost_packets;
}

/**
 * @brief 发布当前AP Peer的Path和Wi-Fi上行丢包状态，调用方持有Switch锁。
 */
static void _linkg_switch_report_publish_locked(linkg_switch_ap_peer_runtime_t *runtime, const linkg_switch_report_sample_t *sample, const linkg_switch_wire_wifi_quality_report_t *report, uint64_t now_us)
{
    runtime->wifi_path_available     = sample->wifi_path_available;
    runtime->cellular_path_available = sample->cellular_path_available;
    runtime->path_updated_us          = now_us;

    runtime->wifi_uplink_loss.valid          = report->sample_packets != 0U;
    runtime->wifi_uplink_loss.loss_permille  = report->sample_packets != 0U ? report->loss_permille : 0U;
    runtime->wifi_uplink_loss.sample_packets = report->sample_packets;
    runtime->wifi_uplink_loss.updated_us     = now_us;
}

/**
 * @brief 处理指定AP Peer的一轮Wi-Fi上行质量报告。
 */
static int _linkg_switch_report_process_peer(uint8_t peer_node_id, uint32_t peer_generation, uint32_t wifi_link_id, uint32_t cellular_link_id, linkg_packet_pool_t *packet_pool, uint64_t now_us)
{
    linkg_switch_wire_wifi_quality_report_t report;
    linkg_switch_report_sample_t            sample;
    linkg_switch_ap_peer_runtime_t         *runtime;
    linkg_switch_peer_runtime_t            *peer;
    uint32_t                                message_id;
    int                                     ret;

    ret = _linkg_switch_report_collect_sample(peer_node_id,
                                               wifi_link_id,
                                               cellular_link_id,
                                               &sample);
    if (ret != 0)
    {
        return ret;
    }

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

    if (!sample.wifi_path_available)
    {
        memset(&runtime->wifi_uplink_loss_sampler, 0, sizeof(runtime->wifi_uplink_loss_sampler));

        _linkg_switch_report_publish_locked(runtime, &sample, &report, now_us);

        pthread_mutex_unlock(&g_switch.lock);

        return 0;
    }

    _linkg_switch_report_build_locked(runtime, wifi_link_id, &sample, &report);

    _linkg_switch_report_publish_locked(runtime, &sample, &report, now_us);

    message_id = _linkg_switch_report_allocate_message_id_locked(runtime);

    pthread_mutex_unlock(&g_switch.lock);

    if (!linkg_switch_peer_generation_current(peer_node_id, peer_generation))
    {
        return -ESTALE;
    }

    return linkg_switch_tx_send_wifi_quality_report(packet_pool, peer_node_id, message_id, &report);
}

/****************************** 周期处理 ******************************/

/**
 * @brief 执行AP本轮全部直接STA的Wi-Fi上行质量报告。
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

    if (g_switch.next_report_us != 0U &&
        now_us < g_switch.next_report_us)
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
 * @brief 获取AP下一轮Wi-Fi质量报告截止时间，调用方持有Switch锁。
 */
uint64_t linkg_switch_report_next_deadline_locked(void)
{
    if (!g_switch.initialized ||
        !g_switch.running ||
        g_switch.role != LINKG_DEVICE_ROLE_AP)
    {
        return UINT64_MAX;
    }

    return g_switch.next_report_us;
}

/****************************** 数据接收 ******************************/

/**
 * @brief 接收AP上报的STA到AP Wi-Fi上行质量结果。
 */
int linkg_switch_report_receive_wifi_quality(uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_wifi_quality_report_t *report)
{
    linkg_switch_sta_peer_runtime_t *runtime;
    linkg_switch_peer_runtime_t     *peer;
    uint64_t                         now_us;

    if (report == NULL)
    {
        return -EINVAL;
    }

    if (peer_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        peer_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    if (message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return -EINVAL;
    }

    if (report->loss_permille > 1000U)
    {
        return -EINVAL;
    }

    if (report->sample_packets == 0U &&
        report->loss_permille != 0U)
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

    if (!_linkg_switch_report_message_id_newer(message_id,
                                                runtime->remote_wifi_uplink_report_id))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    runtime->remote_wifi_uplink_report_id = message_id;

    runtime->remote_wifi_uplink_loss.valid          = report->sample_packets != 0U;
    runtime->remote_wifi_uplink_loss.loss_permille  = report->sample_packets != 0U ? report->loss_permille : 0U;
    runtime->remote_wifi_uplink_loss.sample_packets = report->sample_packets;
    runtime->remote_wifi_uplink_loss.updated_us     = now_us;

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}
