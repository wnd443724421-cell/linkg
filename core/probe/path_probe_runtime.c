/**
 * @file path_probe_runtime.c
 * @brief LinkG Path Probe周期调度、在途管理及多包诊断实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#include "path_probe_internal.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <string.h>

#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_switch.h"
#include "linkg_time.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_path_endpoint_t endpoint; // 本轮Node返回的端点副本
    uint32_t              link_id;  // 本地Link实例
    bool                  active;   // 本轮是否存在活动Path
} linkg_path_probe_observed_path_t;

typedef struct
{
    linkg_path_probe_observed_path_t wifi;         // Wi-Fi观察结果
    linkg_path_probe_observed_path_t cellular;     // Cellular观察结果
    uint8_t                          peer_node_id; // 直接Peer编号
} linkg_path_probe_observed_peer_t;

/****************************** 路径查询 ******************************/

/**
 * @brief 比较端点有效地址，不比较sockaddr填充字节。
 */
bool linkg_path_probe_endpoint_equal(const linkg_path_endpoint_t *left, const linkg_path_endpoint_t *right)
{
    const struct sockaddr_in  *left4;
    const struct sockaddr_in  *right4;
    const struct sockaddr_in6 *left6;
    const struct sockaddr_in6 *right6;

    if (left == NULL || right == NULL || left->length != right->length || left->address.ss_family != right->address.ss_family)
    {
        return false;
    }

    if (left->address.ss_family == AF_INET && left->length == sizeof(struct sockaddr_in))
    {
        left4  = (const struct sockaddr_in *)&left->address;
        right4 = (const struct sockaddr_in *)&right->address;

        return left4->sin_addr.s_addr == right4->sin_addr.s_addr && left4->sin_port == right4->sin_port;
    }

    if (left->address.ss_family == AF_INET6 && left->length == sizeof(struct sockaddr_in6))
    {
        left6  = (const struct sockaddr_in6 *)&left->address;
        right6 = (const struct sockaddr_in6 *)&right->address;

        return memcmp(&left6->sin6_addr, &right6->sin6_addr, sizeof(left6->sin6_addr)) == 0 && left6->sin6_port == right6->sin6_port && left6->sin6_scope_id == right6->sin6_scope_id;
    }

    return false;
}

/**
 * @brief 在Probe锁外校验直接Peer及当前Path，返回端点副本并立即释放Path引用。
 */
int linkg_path_probe_read_target(linkg_device_role_t role, uint8_t local_node_id, uint8_t peer_node_id, uint32_t link_id, linkg_path_endpoint_t *endpoint)
{
    linkg_node_peer_snapshot_t  peer;
    linkg_link_t               *link;
    linkg_path_t               *path;
    int                         ret;

    if (endpoint == NULL || peer_node_id == local_node_id || link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    memset(endpoint, 0, sizeof(*endpoint));

    ret = linkg_node_get_peer_snapshot(peer_node_id, &peer);
    if (ret != 0)
    {
        return ret;
    }

    if ((role == LINKG_DEVICE_ROLE_STA && peer.info.role != LINKG_DEVICE_ROLE_AP) || (role == LINKG_DEVICE_ROLE_AP && peer.info.role != LINKG_DEVICE_ROLE_STA))
    {
        return -EPERM;
    }

    if (role != LINKG_DEVICE_ROLE_STA && role != LINKG_DEVICE_ROLE_AP)
    {
        return -EINVAL;
    }

    link = linkg_link_manager_get(link_id);
    if (link == NULL)
    {
        return -ENOENT;
    }

    if (!linkg_link_is_running(link))
    {
        return -ENETDOWN;
    }

    path = NULL;
    ret  = linkg_node_acquire_path(peer_node_id, link_id, &path, endpoint);
    if (ret != 0)
    {
        return ret;
    }

    linkg_path_release(path);

    return 0;
}

/**
 * @brief 查找直接Peer槽位，调用方持有Probe锁。
 */
static linkg_path_probe_peer_runtime_t *_linkg_path_probe_find_peer_locked(uint8_t peer_node_id)
{
    uint32_t index;

    for (index = 0U; index < LINKG_PATH_PROBE_PEER_MAX; index++)
    {
        if (g_path_probe.peers[index].used && g_path_probe.peers[index].peer_node_id == peer_node_id)
        {
            return &g_path_probe.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 查找已观察到的活动Path，调用方持有Probe锁。
 */
linkg_path_probe_path_runtime_t *linkg_path_probe_find_path_locked(uint8_t peer_node_id, uint32_t link_id)
{
    linkg_path_probe_peer_runtime_t *peer;

    peer = _linkg_path_probe_find_peer_locked(peer_node_id);
    if (peer == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return NULL;
    }

    if (peer->wifi.active && peer->wifi.link_id == link_id)
    {
        return &peer->wifi;
    }

    if (peer->cellular.active && peer->cellular.link_id == link_id)
    {
        return &peer->cellular;
    }

    return NULL;
}

/**
 * @brief 记录经指定Path收到的一条有效Probe帧，调用方持有Probe锁。
 */
void linkg_path_probe_record_rx_locked(uint8_t peer_node_id, uint32_t link_id)
{
    linkg_path_probe_path_runtime_t *path;

    path = linkg_path_probe_find_path_locked(peer_node_id, link_id);
    if (path != NULL && path->probe_rx_packets != UINT64_MAX)
    {
        path->probe_rx_packets++;
    }
}

/**
 * @brief 记录经指定Path成功提交的一条Probe帧，调用方持有Probe锁。
 */
void linkg_path_probe_record_tx_locked(uint8_t peer_node_id, uint32_t link_id)
{
    linkg_path_probe_path_runtime_t *path;

    path = linkg_path_probe_find_path_locked(peer_node_id, link_id);
    if (path != NULL && path->probe_tx_packets != UINT64_MAX)
    {
        path->probe_tx_packets++;
    }
}

/****************************** 诊断汇总 ******************************/

/**
 * @brief 汇总最终样本，成功RTT按发送顺序计算相邻绝对差。
 */
static void _linkg_path_probe_finish_diagnostic_locked(int status, uint64_t now_us)
{
    linkg_path_probe_diagnostic_runtime_t *diagnostic;
    linkg_path_probe_diagnostic_result_t  *result;
    linkg_path_probe_diagnostic_sample_t  *sample;
    uint64_t                               rtt_sum;
    uint64_t                               jitter_sum;
    uint32_t                               previous_rtt;
    uint32_t                               index;

    diagnostic = &g_path_probe.diagnostic;
    if (!diagnostic->active || diagnostic->completed)
    {
        return;
    }

    result = &diagnostic->result;
    memset(result, 0, sizeof(*result));

    result->requested_packets = diagnostic->request.packet_count;
    result->started_us        = diagnostic->started_us;
    result->completed_us      = now_us;
    rtt_sum                   = 0U;
    jitter_sum                = 0U;
    previous_rtt              = 0U;

    for (index = 0U; index < diagnostic->request.packet_count; index++)
    {
        sample = &diagnostic->samples[index];

        if (sample->state == LINKG_PATH_PROBE_SAMPLE_SEND_FAILED)
        {
            result->send_failed_packets++;
            continue;
        }

        if (sample->state == LINKG_PATH_PROBE_SAMPLE_TIMEOUT)
        {
            result->sent_packets++;
            result->lost_packets++;
            continue;
        }

        if (sample->state != LINKG_PATH_PROBE_SAMPLE_RECEIVED)
        {
            continue;
        }

        if (result->received_packets == 0U)
        {
            result->min_rtt_us = sample->rtt_us;
            result->max_rtt_us = sample->rtt_us;
        }
        else
        {
            if (sample->rtt_us < result->min_rtt_us)
            {
                result->min_rtt_us = sample->rtt_us;
            }

            if (sample->rtt_us > result->max_rtt_us)
            {
                result->max_rtt_us = sample->rtt_us;
            }

            jitter_sum += sample->rtt_us >= previous_rtt ? (uint64_t)sample->rtt_us - previous_rtt : (uint64_t)previous_rtt - sample->rtt_us;
        }

        previous_rtt = sample->rtt_us;
        rtt_sum     += sample->rtt_us;
        result->received_packets++;
        result->sent_packets++;
    }

    if (result->sent_packets != 0U)
    {
        result->loss_permille = (uint32_t)(((uint64_t)result->lost_packets * 1000U) / result->sent_packets);
    }

    if (result->received_packets != 0U)
    {
        result->average_rtt_us = (uint32_t)(rtt_sum / result->received_packets);
    }

    if (result->received_packets > 1U)
    {
        result->jitter_us = (uint32_t)(jitter_sum / (result->received_packets - 1U));
    }

    diagnostic->status    = status;
    diagnostic->completed = true;

    pthread_cond_broadcast(&g_path_probe.diagnostic_condition);
}

/**
 * @brief 所有发送尝试及在途样本结束后完成诊断。
 */
static void _linkg_path_probe_try_finish_diagnostic_locked(uint64_t now_us)
{
    linkg_path_probe_diagnostic_runtime_t *diagnostic;

    diagnostic = &g_path_probe.diagnostic;

    if (diagnostic->active && !diagnostic->completed && diagnostic->next_sample_index == diagnostic->request.packet_count && diagnostic->pending_count == 0U)
    {
        _linkg_path_probe_finish_diagnostic_locked(0, now_us);
    }
}

/**
 * @brief 取消主动诊断，保留已完成部分，不将取消样本计为网络丢包。
 */
void linkg_path_probe_cancel_diagnostic_locked(int status, uint64_t now_us)
{
    linkg_path_probe_diagnostic_runtime_t *diagnostic;
    linkg_path_probe_pending_t            *pending;
    uint32_t                               index;

    diagnostic = &g_path_probe.diagnostic;
    if (!diagnostic->active || diagnostic->completed)
    {
        return;
    }

    for (index = 0U; index < LINKG_PATH_PROBE_PENDING_MAX; index++)
    {
        pending = &g_path_probe.pending[index];
        if (pending->state != LINKG_PATH_PROBE_PENDING_FREE && pending->type == LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC && pending->diagnostic_id == diagnostic->id)
        {
            if (pending->diagnostic_index < diagnostic->request.packet_count)
            {
                diagnostic->samples[pending->diagnostic_index].state = LINKG_PATH_PROBE_SAMPLE_CANCELED;
            }

            memset(pending, 0, sizeof(*pending));
        }
    }

    diagnostic->pending_count = 0U;
    _linkg_path_probe_finish_diagnostic_locked(status, now_us);
}

/**
 * @brief 停止全部在途请求并唤醒同步诊断调用者。
 */
void linkg_path_probe_cancel_all_locked(int status, uint64_t now_us)
{
    linkg_path_probe_cancel_diagnostic_locked(status, now_us);
    memset(g_path_probe.pending, 0, sizeof(g_path_probe.pending));
    pthread_cond_broadcast(&g_path_probe.diagnostic_condition);
}

/**
 * @brief 清空一次运行的缓存，不回退序列号、任务编号及回调代际。
 */
void linkg_path_probe_runtime_reset_locked(void)
{
    memset(g_path_probe.peers, 0, sizeof(g_path_probe.peers));
    memset(g_path_probe.pending, 0, sizeof(g_path_probe.pending));
    memset(&g_path_probe.diagnostic, 0, sizeof(g_path_probe.diagnostic));

    g_path_probe.peer_count       = 0U;
    g_path_probe.next_topology_us = 0U;
    g_path_probe.next_policy_us   = 0U;
}

/****************************** 在途完成 ******************************/

/**
 * @brief 完成一条在途记录，调用方持有Probe锁。
 */
static void _linkg_path_probe_complete_pending_locked(linkg_path_probe_pending_t *pending, linkg_path_probe_diagnostic_sample_state_t state, uint64_t now_us)
{
    linkg_path_probe_diagnostic_runtime_t *diagnostic;
    linkg_path_probe_class_runtime_t      *runtime;
    linkg_path_probe_path_runtime_t       *path;
    uint64_t                               elapsed_us;
    uint32_t                               rtt_us;

    elapsed_us = now_us >= pending->sent_us ? now_us - pending->sent_us : 0U;
    rtt_us     = elapsed_us > UINT32_MAX ? UINT32_MAX : (uint32_t)elapsed_us;

    if (pending->type == LINKG_PATH_PROBE_PENDING_TYPE_PERIODIC)
    {
        path = linkg_path_probe_find_path_locked(pending->peer_node_id, pending->link_id);
        if (path != NULL && path->generation == pending->path_generation)
        {
            runtime = &path->classes[pending->traffic_class];

            // 高频探测可乱序完成，旧发送样本不能覆盖较新的公开结果。
            if (pending->sent_us >= runtime->latest_result_sent_us)
            {
                runtime->latest_result_sent_us = pending->sent_us;
                runtime->snapshot.valid        = state == LINKG_PATH_PROBE_SAMPLE_RECEIVED || state == LINKG_PATH_PROBE_SAMPLE_TIMEOUT;
                runtime->snapshot.reachable    = state == LINKG_PATH_PROBE_SAMPLE_RECEIVED;
                runtime->snapshot.updated_us   = now_us;

                if (state == LINKG_PATH_PROBE_SAMPLE_RECEIVED)
                {
                    runtime->snapshot.rtt_us = rtt_us;
                }
            }
        }
    }
    else if (pending->type == LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC)
    {
        diagnostic = &g_path_probe.diagnostic;

        if (diagnostic->active && !diagnostic->completed && diagnostic->id == pending->diagnostic_id && pending->diagnostic_index < diagnostic->request.packet_count)
        {
            diagnostic->samples[pending->diagnostic_index].state   = state;
            diagnostic->samples[pending->diagnostic_index].sent_us = pending->sent_us;
            diagnostic->samples[pending->diagnostic_index].rtt_us  = state == LINKG_PATH_PROBE_SAMPLE_RECEIVED ? rtt_us : 0U;

            if (diagnostic->pending_count != 0U)
            {
                diagnostic->pending_count--;
            }
        }
    }

    memset(pending, 0, sizeof(*pending));
    _linkg_path_probe_try_finish_diagnostic_locked(now_us);
}

/**
 * @brief 校验发送任务仍然拥有原在途槽位。
 */
static linkg_path_probe_pending_t *_linkg_path_probe_task_pending_locked(const linkg_path_probe_tx_task_t *task)
{
    linkg_path_probe_pending_t *pending;

    if (task == NULL || task->pending_index >= LINKG_PATH_PROBE_PENDING_MAX)
    {
        return NULL;
    }

    pending = &g_path_probe.pending[task->pending_index];
    if (pending->state == LINKG_PATH_PROBE_PENDING_FREE || pending->sequence != task->sequence || pending->path_generation != task->path_generation || pending->peer_node_id != task->peer_node_id || pending->link_id != task->link_id || pending->traffic_class != task->traffic_class)
    {
        return NULL;
    }

    return pending;
}

/**
 * @brief 在真正提交Scheduler之前发布发送时间，允许响应早于提交函数返回。
 */
int linkg_path_probe_mark_sending_locked(const linkg_path_probe_tx_task_t *task, uint64_t now_us)
{
    linkg_path_probe_pending_t      *pending;
    linkg_path_probe_path_runtime_t *path;
    uint64_t                         timeout_us;

    pending = _linkg_path_probe_task_pending_locked(task);
    if (!g_path_probe.running || pending == NULL)
    {
        return -ECANCELED;
    }

    path = linkg_path_probe_find_path_locked(task->peer_node_id, task->link_id);
    if (path == NULL || path->generation != task->path_generation)
    {
        return -ESTALE;
    }

    timeout_us = LINKG_PATH_PROBE_DEFAULT_TIMEOUT_US;

    if (pending->type == LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC)
    {
        if (!g_path_probe.diagnostic.active || g_path_probe.diagnostic.completed || g_path_probe.diagnostic.id != pending->diagnostic_id)
        {
            return -ECANCELED;
        }

        timeout_us = (uint64_t)g_path_probe.diagnostic.request.response_timeout_ms * 1000U;
    }

    pending->sent_us     = now_us;
    pending->deadline_us = now_us + timeout_us;
    pending->state       = LINKG_PATH_PROBE_PENDING_SUBMITTING;

    return 0;
}

/**
 * @brief 提交返回后完成本地失败或接纳提前到达的响应。
 */
void linkg_path_probe_finish_send_locked(const linkg_path_probe_tx_task_t *task, int status, uint64_t now_us)
{
    linkg_path_probe_pending_t *pending;
    uint64_t                    reply_us;
    bool                        diagnostic_request;

    pending = _linkg_path_probe_task_pending_locked(task);
    if (pending == NULL)
    {
        return;
    }

    diagnostic_request = pending->type == LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC;

    if (status == 0)
    {
        linkg_path_probe_record_tx_locked(task->peer_node_id, task->link_id);
    }

    if (status != 0)
    {
        if (diagnostic_request && (status == -ENOENT || status == -ENODEV || status == -ENETDOWN || status == -ESTALE))
        {
            g_path_probe.diagnostic.samples[pending->diagnostic_index].state = LINKG_PATH_PROBE_SAMPLE_SEND_FAILED;
            if (g_path_probe.diagnostic.pending_count != 0U)
            {
                g_path_probe.diagnostic.pending_count--;
            }

            memset(pending, 0, sizeof(*pending));
            linkg_path_probe_cancel_diagnostic_locked(status, now_us);
            return;
        }

        _linkg_path_probe_complete_pending_locked(pending, LINKG_PATH_PROBE_SAMPLE_SEND_FAILED, now_us);
        return;
    }

    pending->state = LINKG_PATH_PROBE_PENDING_WAITING;

    if (diagnostic_request)
    {
        g_path_probe.diagnostic.samples[pending->diagnostic_index].state   = LINKG_PATH_PROBE_SAMPLE_PENDING;
        g_path_probe.diagnostic.samples[pending->diagnostic_index].sent_us = pending->sent_us;
    }

    if (pending->early_reply)
    {
        reply_us = pending->early_reply_us;
        _linkg_path_probe_complete_pending_locked(pending, LINKG_PATH_PROBE_SAMPLE_RECEIVED, reply_us);
    }
    else if (now_us >= pending->deadline_us)
    {
        _linkg_path_probe_complete_pending_locked(pending, LINKG_PATH_PROBE_SAMPLE_TIMEOUT, now_us);
    }
}

/**
 * @brief 按序列号、Peer、实际入站Link及Class严格匹配响应。
 */
void linkg_path_probe_accept_reply_locked(uint8_t peer_node_id, uint32_t link_id, linkg_transport_class_t traffic_class, uint32_t sequence, uint64_t now_us)
{
    linkg_path_probe_path_runtime_t *path;
    linkg_path_probe_pending_t      *pending;
    uint32_t                         index;

    for (index = 0U; index < LINKG_PATH_PROBE_PENDING_MAX; index++)
    {
        pending = &g_path_probe.pending[index];
        if (pending->sequence != sequence || pending->peer_node_id != peer_node_id || pending->link_id != link_id || pending->traffic_class != traffic_class)
        {
            continue;
        }

        if (pending->state != LINKG_PATH_PROBE_PENDING_SUBMITTING && pending->state != LINKG_PATH_PROBE_PENDING_WAITING)
        {
            return;
        }

        path = linkg_path_probe_find_path_locked(peer_node_id, link_id);
        if (path == NULL || path->generation != pending->path_generation || now_us < pending->sent_us || now_us > pending->deadline_us)
        {
            return;
        }

        if (pending->state == LINKG_PATH_PROBE_PENDING_SUBMITTING)
        {
            if (!pending->early_reply)
            {
                pending->early_reply    = true;
                pending->early_reply_us = now_us;
            }

            return;
        }

        _linkg_path_probe_complete_pending_locked(pending, LINKG_PATH_PROBE_SAMPLE_RECEIVED, now_us);
        return;
    }
}

/**
 * @brief 扫描已接受请求的响应超时，不等待单包，不阻塞后续发送。
 */
void linkg_path_probe_expire_locked(uint64_t now_us)
{
    linkg_path_probe_pending_t *pending;
    uint32_t                    index;

    for (index = 0U; index < LINKG_PATH_PROBE_PENDING_MAX; index++)
    {
        pending = &g_path_probe.pending[index];
        if (pending->state == LINKG_PATH_PROBE_PENDING_WAITING && now_us >= pending->deadline_us)
        {
            _linkg_path_probe_complete_pending_locked(pending, LINKG_PATH_PROBE_SAMPLE_TIMEOUT, now_us);
        }
    }

    if (g_path_probe.diagnostic.active && !g_path_probe.diagnostic.completed && now_us >= g_path_probe.diagnostic.final_deadline_us)
    {
        linkg_path_probe_cancel_diagnostic_locked(-ETIMEDOUT, now_us);
    }
}

/****************************** 目标同步 ******************************/

/**
 * @brief 失效路径的旧请求全部撤销，避免写回新的Path快照。
 */
static void _linkg_path_probe_remove_path_locked(uint8_t peer_node_id, linkg_path_probe_path_runtime_t *path, uint64_t now_us)
{
    linkg_path_probe_pending_t *pending;
    uint32_t                    index;

    if (g_path_probe.diagnostic.active && !g_path_probe.diagnostic.completed && g_path_probe.diagnostic.request.peer_node_id == peer_node_id && g_path_probe.diagnostic.link_id == path->link_id)
    {
        linkg_path_probe_cancel_diagnostic_locked(-ESTALE, now_us);
    }

    for (index = 0U; index < LINKG_PATH_PROBE_PENDING_MAX; index++)
    {
        pending = &g_path_probe.pending[index];
        if (pending->state != LINKG_PATH_PROBE_PENDING_FREE && pending->peer_node_id == peer_node_id && pending->link_id == path->link_id)
        {
            memset(pending, 0, sizeof(*pending));
        }
    }

    memset(path, 0, sizeof(*path));
}

/**
 * @brief 应用单条Path观察结果，初次发送分散到周期内。
 */
static void _linkg_path_probe_sync_path_locked(uint8_t peer_node_id, uint32_t access_index, linkg_path_probe_path_runtime_t *path, const linkg_path_probe_observed_path_t *observed, uint64_t now_us)
{
    uint64_t phase;
    uint32_t index;

    if (path->active && observed->active && path->link_id == observed->link_id && linkg_path_probe_endpoint_equal(&path->endpoint, &observed->endpoint))
    {
        return;
    }

    if (path->active)
    {
        _linkg_path_probe_remove_path_locked(peer_node_id, path, now_us);
    }

    if (!observed->active)
    {
        return;
    }

    path->active     = true;
    path->link_id    = observed->link_id;
    path->endpoint   = observed->endpoint;
    path->generation = ++g_path_probe.next_path_generation;

    if (path->generation == 0U)
    {
        path->generation = ++g_path_probe.next_path_generation;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if (g_path_probe.role == LINKG_DEVICE_ROLE_AP)
        {
            phase = ((uint64_t)(peer_node_id % LINKG_PATH_PROBE_PEER_MAX) * 6U + access_index * 3U + index) * LINKG_PATH_PROBE_INTERVAL_US / (LINKG_PATH_PROBE_PEER_MAX * 6U);
        }
        else
        {
            phase = ((uint64_t)access_index * 3U + index) * LINKG_PATH_PROBE_INTERVAL_US / 6U;
        }

        path->classes[index].interval_us   = LINKG_PATH_PROBE_INTERVAL_US;
        path->classes[index].next_probe_us = now_us + phase;
    }
}

/**
 * @brief 在Probe锁外获取本轮全部直接Peer及两个接入路径。
 */
static int _linkg_path_probe_collect_targets(linkg_path_probe_observed_peer_t *observed, uint32_t *count)
{
    linkg_node_peer_snapshot_t        peer;
    linkg_path_probe_observed_path_t *path;
    uint32_t                          link_ids[LINKG_NODE_PATH_MAX];
    uint32_t                          node_id;
    uint32_t                          index;
    int                               ret;

    *count      = 0U;
    link_ids[0] = linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    link_ids[1] = linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        if (node_id == g_path_probe.local_node_id)
        {
            continue;
        }

        ret = linkg_node_get_peer_snapshot((uint8_t)node_id, &peer);
        if (ret == -ENOENT)
        {
            continue;
        }

        if (ret != 0)
        {
            return ret;
        }

        if ((g_path_probe.role == LINKG_DEVICE_ROLE_STA && peer.info.role != LINKG_DEVICE_ROLE_AP) || (g_path_probe.role == LINKG_DEVICE_ROLE_AP && peer.info.role != LINKG_DEVICE_ROLE_STA))
        {
            continue;
        }

        if (*count >= LINKG_PATH_PROBE_PEER_MAX)
        {
            return -EOVERFLOW;
        }

        observed[*count].peer_node_id = (uint8_t)node_id;

        for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
        {
            path          = index == 0U ? &observed[*count].wifi : &observed[*count].cellular;
            path->link_id = link_ids[index];

            if (path->link_id == LINKG_LINK_ID_INVALID)
            {
                continue;
            }

            ret = linkg_path_probe_read_target(g_path_probe.role, g_path_probe.local_node_id, (uint8_t)node_id, path->link_id, &path->endpoint);
            if (ret == 0)
            {
                path->active = true;
            }
            else if (ret != -ENOENT && ret != -ENODEV && ret != -ENETDOWN && ret != -EPERM)
            {
                return ret;
            }
        }

        (*count)++;

        // 星型STA只维护一个直接AP，不把AP转发节点放进Probe表。
        if (g_path_probe.role == LINKG_DEVICE_ROLE_STA)
        {
            break;
        }
    }

    return 0;
}

/**
 * @brief 刷新Direct Peer列表，Node及Link查询均不持有Probe锁。
 */
static void _linkg_path_probe_refresh_targets(uint64_t now_us)
{
    linkg_path_probe_observed_peer_t  observed[LINKG_PATH_PROBE_PEER_MAX];
    linkg_path_probe_peer_runtime_t  *peer;
    bool                              seen[LINKG_PATH_PROBE_PEER_MAX];
    uint32_t                          count;
    uint32_t                          index;
    uint32_t                          slot;
    int                               ret;

    memset(observed, 0, sizeof(observed));
    memset(seen, 0, sizeof(seen));
    ret = _linkg_path_probe_collect_targets(observed, &count);

    pthread_mutex_lock(&g_path_probe.lock);
    g_path_probe.next_topology_us = now_us + LINKG_PATH_PROBE_TOPOLOGY_INTERVAL_US;

    if (!g_path_probe.running || ret != 0)
    {
        pthread_mutex_unlock(&g_path_probe.lock);
        return;
    }

    // 先移除已经消失的Peer，保证满表时新Peer仍然有槽位可用。
    for (slot = 0U; slot < LINKG_PATH_PROBE_PEER_MAX; slot++)
    {
        peer = &g_path_probe.peers[slot];
        if (!peer->used)
        {
            continue;
        }

        for (index = 0U; index < count; index++)
        {
            if (observed[index].peer_node_id == peer->peer_node_id)
            {
                break;
            }
        }

        if (index == count)
        {
            _linkg_path_probe_remove_path_locked(peer->peer_node_id, &peer->wifi, now_us);
            _linkg_path_probe_remove_path_locked(peer->peer_node_id, &peer->cellular, now_us);
            memset(peer, 0, sizeof(*peer));
        }
    }

    for (index = 0U; index < count; index++)
    {
        peer = _linkg_path_probe_find_peer_locked(observed[index].peer_node_id);
        if (peer == NULL)
        {
            for (slot = 0U; slot < LINKG_PATH_PROBE_PEER_MAX && g_path_probe.peers[slot].used; slot++)
            {
            }

            if (slot == LINKG_PATH_PROBE_PEER_MAX)
            {
                continue;
            }

            peer               = &g_path_probe.peers[slot];
            peer->used         = true;
            peer->peer_node_id = observed[index].peer_node_id;
        }

        slot       = (uint32_t)(peer - g_path_probe.peers);
        seen[slot] = true;
        _linkg_path_probe_sync_path_locked(peer->peer_node_id, 0U, &peer->wifi, &observed[index].wifi, now_us);
        _linkg_path_probe_sync_path_locked(peer->peer_node_id, 1U, &peer->cellular, &observed[index].cellular, now_us);
    }

    g_path_probe.peer_count = 0U;
    for (slot = 0U; slot < LINKG_PATH_PROBE_PEER_MAX; slot++)
    {
        if (seen[slot])
        {
            g_path_probe.peer_count++;
        }
    }

    pthread_mutex_unlock(&g_path_probe.lock);
}

/**
 * @brief 获取Path累计真实业务包数量。
 *
 * Path基础统计同时包含Probe帧，因此扣除本模块已经观察到的Probe收发数量。
 * 计数异常或底层统计回退时按0处理，避免无符号下溢影响PPS策略。
 */
static uint64_t _linkg_path_probe_business_packets(const linkg_path_probe_path_runtime_t *runtime, const linkg_path_stats_t *stats)
{
    uint64_t total_packets;
    uint64_t probe_packets;

    if (runtime == NULL || stats == NULL)
    {
        return 0U;
    }

    total_packets = stats->tx_packets + stats->rx_packets;
    probe_packets = runtime->probe_tx_packets + runtime->probe_rx_packets;

    return total_packets >= probe_packets ? total_packets - probe_packets : 0U;
}

/**
 * @brief 更新Cellular真实业务PPS，调用方持有Probe锁。
 */
static void _linkg_path_probe_update_pps_locked(linkg_path_probe_path_runtime_t *path, const linkg_path_stats_t *stats, uint64_t now_us)
{
    uint64_t business_packets;
    uint64_t delta_packets;
    uint64_t elapsed_us;
    uint64_t pps;

    business_packets = _linkg_path_probe_business_packets(path, stats);

    if (!path->pps_sample_initialized || business_packets < path->pps_last_business_packets || now_us < path->pps_sample_started_us)
    {
        path->pps_sample_started_us      = now_us;
        path->pps_last_business_packets  = business_packets;
        path->current_pps                = 0U;
        path->pps_sample_initialized     = true;
        return;
    }

    elapsed_us = now_us - path->pps_sample_started_us;
    if (elapsed_us < LINKG_PATH_PROBE_PPS_SAMPLE_INTERVAL_US)
    {
        return;
    }

    delta_packets = business_packets - path->pps_last_business_packets;
    pps           = (delta_packets * 1000000ULL) / elapsed_us;

    path->current_pps               = pps > UINT32_MAX ? UINT32_MAX : (uint32_t)pps;
    path->pps_sample_started_us      = now_us;
    path->pps_last_business_packets  = business_packets;
}

/**
 * @brief STA按主链路和Cellular真实业务PPS更新三类别周期，AP始终保持1秒。
 */
static void _linkg_path_probe_refresh_policy(uint64_t now_us)
{
    linkg_path_probe_peer_runtime_t *peer;
    linkg_path_stats_t                stats;
    linkg_path_endpoint_t             endpoint;
    linkg_path_t                     *path;
    linkg_send_plan_t                plan;
    uint64_t                         generation;
    uint64_t                         interval_us;
    uint32_t                         link_id;
    uint32_t                         current_pps;
    uint32_t                         index;
    uint8_t                          peer_node_id;
    bool                             cellular_primary;
    int                              stats_ret;
    int                              ret;

    pthread_mutex_lock(&g_path_probe.lock);
    g_path_probe.next_policy_us = now_us + LINKG_PATH_PROBE_POLICY_INTERVAL_US;

    if (!g_path_probe.running || g_path_probe.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_path_probe.lock);
        return;
    }

    peer = NULL;
    for (index = 0U; index < LINKG_PATH_PROBE_PEER_MAX; index++)
    {
        if (g_path_probe.peers[index].used && g_path_probe.peers[index].cellular.active)
        {
            peer = &g_path_probe.peers[index];
            break;
        }
    }

    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_path_probe.lock);
        return;
    }

    peer_node_id = peer->peer_node_id;
    link_id      = peer->cellular.link_id;
    generation   = peer->cellular.generation;
    pthread_mutex_unlock(&g_path_probe.lock);

    memset(&stats, 0, sizeof(stats));
    memset(&endpoint, 0, sizeof(endpoint));

    path      = NULL;
    stats_ret = linkg_node_acquire_path(peer_node_id, link_id, &path, &endpoint);
    if (stats_ret == 0)
    {
        stats_ret = linkg_path_get_stats(path, &stats);
        linkg_path_release(path);
    }

    ret              = linkg_switch_get_plan(peer_node_id, &plan);
    cellular_primary = ret == 0 && (plan.mode == LINKG_SEND_MODE_SINGLE || plan.mode == LINKG_SEND_MODE_REDUNDANT) && plan.primary_link_id == link_id;
    interval_us      = LINKG_PATH_PROBE_INTERVAL_US;
    current_pps      = UINT32_MAX;

    pthread_mutex_lock(&g_path_probe.lock);
    peer = _linkg_path_probe_find_peer_locked(peer_node_id);

    if (g_path_probe.running && peer != NULL && peer->cellular.active && peer->cellular.generation == generation)
    {
        if (stats_ret == 0)
        {
            _linkg_path_probe_update_pps_locked(&peer->cellular, &stats, now_us);
            current_pps = peer->cellular.current_pps;
        }

        if (cellular_primary && current_pps <= LINKG_PATH_PROBE_HIGH_PPS_THRESHOLD)
        {
            interval_us = LINKG_PATH_PROBE_CELLULAR_FAST_INTERVAL_US;
        }

        for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
        {
            if (peer->cellular.classes[index].interval_us != interval_us)
            {
                peer->cellular.classes[index].interval_us   = interval_us;
                peer->cellular.classes[index].next_probe_us = now_us + (uint64_t)index * interval_us / LINKG_TRANSPORT_CLASS_COUNT;
            }
        }
    }

    pthread_mutex_unlock(&g_path_probe.lock);
}

/****************************** 请求调度 ******************************/

/**
 * @brief 分配非零Wire序列号，跳过当前仍在使用的编号。
 */
static uint32_t _linkg_path_probe_allocate_sequence_locked(void)
{
    uint32_t sequence;
    uint32_t index;

    do
    {
        sequence = ++g_path_probe.next_sequence;
        if (sequence == LINKG_PATH_PROBE_SEQUENCE_INVALID)
        {
            sequence = ++g_path_probe.next_sequence;
        }

        for (index = 0U; index < LINKG_PATH_PROBE_PENDING_MAX; index++)
        {
            if (g_path_probe.pending[index].state != LINKG_PATH_PROBE_PENDING_FREE && g_path_probe.pending[index].sequence == sequence)
            {
                break;
            }
        }
    }
    while (index != LINKG_PATH_PROBE_PENDING_MAX);

    return sequence;
}

/**
 * @brief 在各自保留区申请记录，诊断不挤占周期保活槽位。
 */
static int _linkg_path_probe_reserve_locked(uint8_t peer_node_id, linkg_path_probe_path_runtime_t *path, linkg_transport_class_t traffic_class, bool diagnostic, uint32_t sample_index, uint64_t now_us, linkg_path_probe_tx_task_t *task)
{
    linkg_path_probe_pending_t *pending;
    uint32_t                    begin;
    uint32_t                    end;
    uint32_t                    index;

    begin = diagnostic ? LINKG_PATH_PROBE_PERIODIC_PENDING_MAX : 0U;
    end   = diagnostic ? LINKG_PATH_PROBE_PENDING_MAX : LINKG_PATH_PROBE_PERIODIC_PENDING_MAX;

    for (index = begin; index < end; index++)
    {
        if (g_path_probe.pending[index].state == LINKG_PATH_PROBE_PENDING_FREE)
        {
            break;
        }
    }

    if (index == end)
    {
        return -ENOSPC;
    }

    pending = &g_path_probe.pending[index];
    memset(pending, 0, sizeof(*pending));

    pending->state            = LINKG_PATH_PROBE_PENDING_RESERVED;
    pending->type             = diagnostic ? LINKG_PATH_PROBE_PENDING_TYPE_DIAGNOSTIC : LINKG_PATH_PROBE_PENDING_TYPE_PERIODIC;
    pending->peer_node_id     = peer_node_id;
    pending->link_id          = path->link_id;
    pending->traffic_class    = traffic_class;
    pending->path_generation  = path->generation;
    pending->sequence         = _linkg_path_probe_allocate_sequence_locked();
    pending->sent_us          = now_us;
    pending->diagnostic_id    = diagnostic ? g_path_probe.diagnostic.id : 0U;
    pending->diagnostic_index = diagnostic ? sample_index : LINKG_PATH_PROBE_DIAGNOSTIC_INDEX_INVALID;

    task->pending_index   = index;
    task->sequence        = pending->sequence;
    task->path_generation = path->generation;
    task->peer_node_id    = peer_node_id;
    task->link_id         = path->link_id;
    task->traffic_class   = traffic_class;
    task->endpoint        = path->endpoint;

    if (diagnostic)
    {
        g_path_probe.diagnostic.pending_count++;
        g_path_probe.diagnostic.samples[sample_index].sequence = pending->sequence;
        g_path_probe.diagnostic.samples[sample_index].state    = LINKG_PATH_PROBE_SAMPLE_RESERVED;
    }

    return 0;
}

/**
 * @brief 选择一个到期任务，返回1可发送，2为处理了失约样本，0为暂无任务。
 */
static int _linkg_path_probe_pick_task_locked(uint64_t now_us, linkg_path_probe_tx_task_t *task)
{
    linkg_path_probe_diagnostic_runtime_t *diagnostic;
    linkg_path_probe_path_runtime_t       *best_path;
    linkg_path_probe_path_runtime_t       *path;
    linkg_path_probe_peer_runtime_t       *peer;
    uint64_t                               best_due;
    uint64_t                               slot_end;
    uint32_t                               best_class;
    uint32_t                               class_index;
    uint32_t                               peer_index;
    uint32_t                               path_index;
    uint32_t                               sample_index;
    uint8_t                                best_peer;
    int                                    ret;

    best_path  = NULL;
    best_due   = UINT64_MAX;
    best_class = 0U;
    best_peer  = 0U;
    diagnostic = &g_path_probe.diagnostic;

    for (peer_index = 0U; peer_index < LINKG_PATH_PROBE_PEER_MAX; peer_index++)
    {
        peer = &g_path_probe.peers[peer_index];
        if (!peer->used)
        {
            continue;
        }

        for (path_index = 0U; path_index < LINKG_NODE_PATH_MAX; path_index++)
        {
            path = path_index == 0U ? &peer->wifi : &peer->cellular;
            if (!path->active)
            {
                continue;
            }

            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                if (path->classes[class_index].next_probe_us <= now_us && path->classes[class_index].next_probe_us < best_due)
                {
                    best_path  = path;
                    best_due   = path->classes[class_index].next_probe_us;
                    best_class = class_index;
                    best_peer  = peer->peer_node_id;
                }
            }
        }
    }

    if (diagnostic->active && !diagnostic->completed && diagnostic->next_sample_index < diagnostic->request.packet_count && diagnostic->next_send_us <= now_us && diagnostic->next_send_us <= best_due)
    {
        path = linkg_path_probe_find_path_locked(diagnostic->request.peer_node_id, diagnostic->link_id);
        if (path == NULL || path->generation != diagnostic->path_generation)
        {
            linkg_path_probe_cancel_diagnostic_locked(-ESTALE, now_us);
            return 2;
        }

        sample_index             = diagnostic->next_sample_index++;
        slot_end                 = diagnostic->started_us + ((uint64_t)(sample_index + 1U) * diagnostic->send_window_us) / diagnostic->request.packet_count;
        diagnostic->next_send_us = slot_end;

        // 多包诊断错过时隙直接计本地失败，不追赶补发形成瞬时突发。
        if (diagnostic->request.packet_count > 1U && now_us >= slot_end)
        {
            diagnostic->samples[sample_index].state = LINKG_PATH_PROBE_SAMPLE_SEND_FAILED;
            _linkg_path_probe_try_finish_diagnostic_locked(now_us);
            return 2;
        }

        ret = _linkg_path_probe_reserve_locked(diagnostic->request.peer_node_id, path, diagnostic->request.traffic_class, true, sample_index, now_us, task);
        if (ret != 0)
        {
            diagnostic->samples[sample_index].state = LINKG_PATH_PROBE_SAMPLE_SEND_FAILED;
            _linkg_path_probe_try_finish_diagnostic_locked(now_us);
            return 2;
        }

        return 1;
    }

    if (best_path == NULL)
    {
        return 0;
    }

    // 周期任务只安排下一次，不补发错过的历史周期。
    best_path->classes[best_class].next_probe_us = now_us + best_path->classes[best_class].interval_us;
    ret                                          = _linkg_path_probe_reserve_locked(best_peer, best_path, (linkg_transport_class_t)best_class, false, 0U, now_us, task);
    if (ret != 0)
    {
        best_path->classes[best_class].latest_result_sent_us = now_us;
        best_path->classes[best_class].snapshot.valid        = false;
        best_path->classes[best_class].snapshot.reachable    = false;
        best_path->classes[best_class].snapshot.updated_us   = now_us;
        return 2;
    }

    return 1;
}

/**
 * @brief 计算最近发送、响应超时或状态同步截止时间。
 */
static int _linkg_path_probe_poll_timeout_locked(uint64_t now_us)
{
    linkg_path_probe_peer_runtime_t *peer;
    linkg_path_probe_path_runtime_t *path;
    uint64_t                         deadline_us;
    uint64_t                         delta_us;
    uint32_t                         peer_index;
    uint32_t                         path_index;
    uint32_t                         class_index;
    uint32_t                         index;

    deadline_us = g_path_probe.next_topology_us;

    if (g_path_probe.role == LINKG_DEVICE_ROLE_STA && g_path_probe.next_policy_us < deadline_us)
    {
        deadline_us = g_path_probe.next_policy_us;
    }

    for (peer_index = 0U; peer_index < LINKG_PATH_PROBE_PEER_MAX; peer_index++)
    {
        peer = &g_path_probe.peers[peer_index];
        if (!peer->used)
        {
            continue;
        }

        for (path_index = 0U; path_index < LINKG_NODE_PATH_MAX; path_index++)
        {
            path = path_index == 0U ? &peer->wifi : &peer->cellular;
            if (!path->active)
            {
                continue;
            }

            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                if (path->classes[class_index].next_probe_us < deadline_us)
                {
                    deadline_us = path->classes[class_index].next_probe_us;
                }
            }
        }
    }

    for (index = 0U; index < LINKG_PATH_PROBE_PENDING_MAX; index++)
    {
        if (g_path_probe.pending[index].state == LINKG_PATH_PROBE_PENDING_WAITING && g_path_probe.pending[index].deadline_us < deadline_us)
        {
            deadline_us = g_path_probe.pending[index].deadline_us;
        }
    }

    if (g_path_probe.diagnostic.active && !g_path_probe.diagnostic.completed)
    {
        if (g_path_probe.diagnostic.next_sample_index < g_path_probe.diagnostic.request.packet_count && g_path_probe.diagnostic.next_send_us < deadline_us)
        {
            deadline_us = g_path_probe.diagnostic.next_send_us;
        }

        if (g_path_probe.diagnostic.final_deadline_us < deadline_us)
        {
            deadline_us = g_path_probe.diagnostic.final_deadline_us;
        }
    }

    if (deadline_us <= now_us)
    {
        return 0;
    }

    delta_us = (deadline_us - now_us + 999U) / 1000U;
    return delta_us > INT_MAX ? INT_MAX : (int)delta_us;
}

/****************************** 工作线程 ******************************/

/**
 * @brief 执行周期Probe、固定节拍诊断及超时扫描，全部发送在Probe锁外完成。
 */
void linkg_path_probe_worker(linkg_thread_t *thread, void *user_data)
{
    linkg_path_probe_tx_task_t task;
    struct pollfd              descriptor;
    uint64_t                   now_us;
    uint32_t                   budget;
    bool                       refresh_targets;
    bool                       refresh_policy;
    int                        timeout_ms;
    int                        selected;
    int                        ret;
    int                        failure;

    (void)user_data;

    pthread_mutex_lock(&g_path_probe.lock);
    g_path_probe.worker_tid       = pthread_self();
    g_path_probe.worker_tid_valid = true;
    pthread_mutex_unlock(&g_path_probe.lock);

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd     = linkg_thread_get_wakeup_fd(thread);
    descriptor.events = POLLIN;
    failure           = 0;

    if (descriptor.fd < 0)
    {
        failure = -ENODEV;
    }

    while (failure == 0 && linkg_thread_is_running(thread))
    {
        now_us = linkg_time_monotonic_us();
        pthread_mutex_lock(&g_path_probe.lock);

        if (!g_path_probe.running)
        {
            pthread_mutex_unlock(&g_path_probe.lock);
            break;
        }

        linkg_path_probe_expire_locked(now_us);
        refresh_targets = now_us >= g_path_probe.next_topology_us;
        refresh_policy  = g_path_probe.role == LINKG_DEVICE_ROLE_STA && now_us >= g_path_probe.next_policy_us;
        pthread_mutex_unlock(&g_path_probe.lock);

        if (refresh_targets)
        {
            _linkg_path_probe_refresh_targets(now_us);
        }

        if (refresh_policy || refresh_targets)
        {
            _linkg_path_probe_refresh_policy(linkg_time_monotonic_us());
        }

        for (budget = 0U; budget < LINKG_PATH_PROBE_WORK_BUDGET && linkg_thread_is_running(thread); budget++)
        {
            now_us = linkg_time_monotonic_us();
            pthread_mutex_lock(&g_path_probe.lock);
            linkg_path_probe_expire_locked(now_us);
            selected = g_path_probe.running ? _linkg_path_probe_pick_task_locked(now_us, &task) : 0;
            pthread_mutex_unlock(&g_path_probe.lock);

            if (selected == 0)
            {
                break;
            }

            if (selected == 1)
            {
                // send_request自行回填结果，下一包发送不等待当前包响应。
                (void)linkg_path_probe_send_request(g_path_probe.packet_pool, g_path_probe.role, g_path_probe.local_node_id, &task);
            }
        }

        pthread_mutex_lock(&g_path_probe.lock);
        timeout_ms = g_path_probe.running ? _linkg_path_probe_poll_timeout_locked(linkg_time_monotonic_us()) : 0;
        pthread_mutex_unlock(&g_path_probe.lock);

        if (budget == LINKG_PATH_PROBE_WORK_BUDGET && timeout_ms == 0)
        {
            timeout_ms = 1;
        }

        descriptor.revents = 0;
        ret                = poll(&descriptor, 1U, timeout_ms);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            failure = -errno;
            break;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            failure = -EIO;
            break;
        }

        if ((descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                failure = ret;
            }
        }
    }

    pthread_mutex_lock(&g_path_probe.lock);
    g_path_probe.running          = false;
    g_path_probe.worker_tid_valid = false;
    linkg_path_probe_cancel_all_locked(failure != 0 ? failure : -ESHUTDOWN, linkg_time_monotonic_us());
    pthread_mutex_unlock(&g_path_probe.lock);

    if (failure != 0)
    {
        LINKG_LOG_ERROR("PATH-PROBE: worker failed, error=%d", failure);
    }
}
