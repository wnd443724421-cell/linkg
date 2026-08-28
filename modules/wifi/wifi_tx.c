/**
 * @file wifi_tx.c
 * @brief LinkG Wi-Fi发送模块实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#define _GNU_SOURCE

#include "wifi_tx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_packet_pool.h"
#include "linkg_time.h"
#include "wifi_flowctrl.h"
#include "wifi_traffic.h"
#include "wifi_tx_queue.h"

/****************************** 发送参数 ******************************/

#define LINKG_WIFI_TX_IPV4_HEADER_SIZE         20U                           // IPv4最小头部长度
#define LINKG_WIFI_TX_UDP_HEADER_SIZE          8U                            // UDP头部长度
#define LINKG_WIFI_TX_MTU                      1500U                         // Wi-Fi接口MTU
#define LINKG_WIFI_TX_UDP_PAYLOAD_MAX          (LINKG_WIFI_TX_MTU - LINKG_WIFI_TX_IPV4_HEADER_SIZE - LINKG_WIFI_TX_UDP_HEADER_SIZE) // 单个UDP报文最大负载
#define LINKG_WIFI_TX_NORMAL_SEND_MAX          32U                           // Normal单轮需求及临时数组最大包数
#define LINKG_WIFI_TX_VIDEO_MAX_AGE_US         100000ULL                     // VIDEO等待队列最大驻留时间
#define LINKG_WIFI_TX_TELEMETRY_INTERVAL_US    250000ULL                     // 流控调试遥测最小输出间隔

#ifndef LINKG_WIFI_TX_VIDEO_QUEUE_CAPACITY
#define LINKG_WIFI_TX_VIDEO_QUEUE_CAPACITY     8U                            // VIDEO等待队列容量
#endif

#ifndef LINKG_WIFI_TX_DATA_QUEUE_CAPACITY
#define LINKG_WIFI_TX_DATA_QUEUE_CAPACITY      8U                            // DATA等待队列容量
#endif

#ifndef LINKG_WIFI_TX_DATA_MAX_AGE_US
#define LINKG_WIFI_TX_DATA_MAX_AGE_US          100000ULL                     // DATA等待队列最大驻留时间
#endif

#ifndef LINKG_WIFI_TX_SENDMMSG_CHUNK_MAX
#define LINKG_WIFI_TX_SENDMMSG_CHUNK_MAX       LINKG_WIFI_TX_NORMAL_SEND_MAX // Normal单次sendmmsg最大消息数量
#endif

/****************************** 内部类型 ******************************/

struct linkg_wifi_tx
{
    pthread_mutex_t        normal_lock;                           // VIDEO/DATA控制路径串行锁
    pthread_mutex_t        send_lock;                             // sendmmsg共享资源和发送telemetry计数保护锁
    linkg_wifi_flowctrl_t *flowctrl;                              // HI1105普通业务流控控制器
    linkg_wifi_tx_queue_t *video_queue;                           // VI等待队列
    linkg_wifi_tx_queue_t *data_queue;                            // BE等待队列
    struct mmsghdr        *messages;                              // sendmmsg消息数组
    struct iovec          *iovecs;                                // sendmmsg缓冲区数组
    struct sockaddr_in    *destinations;                          // sendmmsg目标地址数组
    int                   *socket_fds;                            // 借用Wi-Fi Link业务Socket数组
    const uint16_t        *service_ports;                         // 借用Wi-Fi Link业务端口数组
    uint32_t               capacity;                              // 发送scratch容量
    uint32_t               video_pending_peak;                    // 当前telemetry窗口VI pending峰值
    uint32_t               data_pending_peak;                     // 当前telemetry窗口BE pending峰值
    uint64_t               normal_sent_packets;                   // Normal真实发送累计包数
    uint64_t               normal_sent_bytes;                     // Normal真实发送累计字节数
    uint64_t               pending_drop_packets;                  // pending最终丢弃累计包数
    uint64_t               telemetry_last_us;                     // 上次telemetry时间
    uint64_t               telemetry_last_sent_packets;           // 上次telemetry发送累计包数
    uint64_t               telemetry_last_sent_bytes;             // 上次telemetry发送累计字节数
    uint64_t               telemetry_last_pending_drop;           // 上次telemetry pending丢弃累计包数
    _Atomic bool           started;                               // 发送模块运行状态
};

/****************************** 业务映射 ******************************/

/**
 * @brief 将Link发送类别映射为Wi-Fi内部业务类别。
 */
static int _linkg_wifi_tx_class_to_traffic(linkg_link_tx_class_t tx_class, linkg_wifi_traffic_class_t *traffic_class)
{
    if (traffic_class == NULL)
    {
        return -EINVAL;
    }

    switch (tx_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            *traffic_class = LINKG_WIFI_TRAFFIC_REALTIME;
            return 0;

        case LINKG_LINK_TX_CLASS_VIDEO:
            *traffic_class = LINKG_WIFI_TRAFFIC_VIDEO;
            return 0;

        case LINKG_LINK_TX_CLASS_DATA:
            *traffic_class = LINKG_WIFI_TRAFFIC_DATA;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 获取VIDEO或DATA对应的等待队列。
 */
static linkg_wifi_tx_queue_t *_linkg_wifi_tx_select_queue(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class)
{
    if (traffic_class == LINKG_WIFI_TRAFFIC_VIDEO)
    {
        return tx->video_queue;
    }

    if (traffic_class == LINKG_WIFI_TRAFFIC_DATA)
    {
        return tx->data_queue;
    }

    return NULL;
}

/**
 * @brief 获取指定普通业务等待队列的最大驻留时间。
 */
static uint64_t _linkg_wifi_tx_queue_max_age(linkg_wifi_traffic_class_t traffic_class)
{
    if (traffic_class == LINKG_WIFI_TRAFFIC_VIDEO)
    {
        return LINKG_WIFI_TX_VIDEO_MAX_AGE_US;
    }

    return LINKG_WIFI_TX_DATA_MAX_AGE_US;
}


/****************************** 原子发送组 ******************************/

/**
 * @brief 判断Packet是否为Transport原子发送组首包。
 */
static bool _linkg_wifi_tx_group_first(const linkg_packet_t *packet)
{
    return packet != NULL &&
           (packet->flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U;
}

/**
 * @brief 获取不会在Transport原子发送组首包处截断的最大前缀长度。
 */
static uint32_t _linkg_wifi_tx_complete_prefix(linkg_packet_t *const *packets,
                                               uint32_t count,
                                               uint32_t limit)
{
    uint32_t prefix;

    if (packets == NULL || count == 0U || limit == 0U)
    {
        return 0U;
    }

    prefix = count;
    if (prefix > limit)
    {
        prefix = limit;
    }

    if (prefix > 0U && _linkg_wifi_tx_group_first(packets[prefix - 1U]))
    {
        prefix--;
    }

    return prefix;
}

/**
 * @brief 获取等待队列中不会截断Transport原子发送组的前缀长度。
 */
static uint32_t _linkg_wifi_tx_complete_queue_prefix(
    const linkg_wifi_tx_queue_item_t *items, uint32_t count)
{
    return items != NULL && count > 0U &&
           _linkg_wifi_tx_group_first(items[count - 1U].packet) ? count - 1U : count;
}
/****************************** 参数校验 ******************************/

/**
 * @brief 校验Wi-Fi下一跳Endpoint是否合法。
 */
static int _linkg_wifi_tx_validate_destination(const linkg_wifi_tx_t *tx, const linkg_path_endpoint_t *destination)
{
    const struct sockaddr_in *target;

    if (tx == NULL || destination == NULL)
    {
        return -EINVAL;
    }

    if (destination->length != sizeof(struct sockaddr_in))
    {
        return -EDESTADDRREQ;
    }

    if (destination->address.ss_family != AF_INET)
    {
        return -EAFNOSUPPORT;
    }

    target = (const struct sockaddr_in *)&destination->address;

    if (!linkg_network_ipv4_address_valid(&target->sin_addr))
    {
        return -EDESTADDRREQ;
    }

    if (target->sin_port != htons(tx->service_ports[LINKG_WIFI_TRAFFIC_DATA]))
    {
        return -EDESTADDRREQ;
    }

    return 0;
}

/**
 * @brief 校验单个待发送Packet是否合法。
 */
static int _linkg_wifi_tx_validate_packet(const linkg_packet_t *packet)
{
    if (packet == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length == 0U)
    {
        return -EINVAL;
    }

    if (packet->data_length > LINKG_WIFI_TX_UDP_PAYLOAD_MAX)
    {
        return -EMSGSIZE;
    }

    return 0;
}

/**
 * @brief 判断底层发送错误是否允许通过等待队列重试。
 */
static bool _linkg_wifi_tx_error_retryable(int error)
{
    return error == -EAGAIN || error == -EWOULDBLOCK || error == -ENOBUFS;
}

/****************************** 遥测统计 ******************************/

/**
 * @brief 获取当前流控采样对应的调试状态文本。
 */
static const char *_linkg_wifi_tx_controller_state(const linkg_wifi_flowctrl_sample_t *sample, int flowctrl_error)
{
    if (flowctrl_error != 0 || !sample->status_valid)
    {
        return "UNAVAILABLE";
    }

    if (sample->tx_allowed == 0U)
    {
        return "BLOCKED";
    }

    if (sample->waterline_blocked)
    {
        return "HCC_BLOCKED";
    }

    if (sample->backed_off)
    {
        return "BACKOFF";
    }

    return "RUNNING";
}

/**
 * @brief 输出VIDEO/DATA发送流控调试遥测。
 *
 * @note 调用方必须持有normal_lock。
 */
static void _linkg_wifi_tx_report(linkg_wifi_tx_t *tx, const linkg_wifi_flowctrl_sample_t *sample, int flowctrl_error, uint64_t now_us)
{
    const char *state;
    uint64_t elapsed_us;
    uint64_t sent_packets;
    uint64_t sent_bytes;
    uint64_t sent_packet_delta;
    uint64_t sent_byte_delta;
    uint64_t pending_drop_delta;
    uint64_t normal_rate_bps;
    uint32_t video_pending;
    uint32_t data_pending;

    if (tx->telemetry_last_us == 0U)
    {
        tx->telemetry_last_us = now_us;

        pthread_mutex_lock(&tx->send_lock);
        tx->telemetry_last_sent_packets = tx->normal_sent_packets;
        tx->telemetry_last_sent_bytes = tx->normal_sent_bytes;
        pthread_mutex_unlock(&tx->send_lock);

        tx->telemetry_last_pending_drop = tx->pending_drop_packets;
        return;
    }

    if (now_us < tx->telemetry_last_us)
    {
        tx->telemetry_last_us = now_us;
        return;
    }

    elapsed_us = now_us - tx->telemetry_last_us;
    if (elapsed_us < LINKG_WIFI_TX_TELEMETRY_INTERVAL_US)
    {
        return;
    }

    pthread_mutex_lock(&tx->send_lock);
    sent_packets = tx->normal_sent_packets;
    sent_bytes = tx->normal_sent_bytes;
    pthread_mutex_unlock(&tx->send_lock);

    sent_packet_delta = sent_packets - tx->telemetry_last_sent_packets;
    sent_byte_delta = sent_bytes - tx->telemetry_last_sent_bytes;
    pending_drop_delta = tx->pending_drop_packets - tx->telemetry_last_pending_drop;
    normal_rate_bps = (sent_byte_delta * 8ULL * 1000000ULL) / elapsed_us;

    video_pending = linkg_wifi_tx_queue_count(tx->video_queue);
    data_pending = linkg_wifi_tx_queue_count(tx->data_queue);
    state = _linkg_wifi_tx_controller_state(sample, flowctrl_error);

    LINKG_LOG_DEBUG("WIFI: TX flowctrl sample, be=%u, vi=%u, vo=%u, tx_allowed=%u, off=%u, state=%s, normal_limit=%u, requested=%u, admitted=%u, rate_bps=%llu, vi_pending=%u, vi_peak=%u, be_pending=%u, be_peak=%u, sent_packets=%llu, sent_bytes=%llu, pending_drop=%llu, read_us=%llu, error=%d",
                    sample->be_queue_length, sample->vi_queue_length, sample->vo_queue_length,
                    sample->tx_allowed, sample->flowctrl_off_count, state, sample->normal_batch_limit,
                    sample->requested_count, sample->allowed_count, (unsigned long long)normal_rate_bps,
                    video_pending, tx->video_pending_peak, data_pending, tx->data_pending_peak,
                    (unsigned long long)sent_packet_delta, (unsigned long long)sent_byte_delta,
                    (unsigned long long)pending_drop_delta, (unsigned long long)sample->read_us, flowctrl_error);

    tx->telemetry_last_us = now_us;
    tx->telemetry_last_sent_packets = sent_packets;
    tx->telemetry_last_sent_bytes = sent_bytes;
    tx->telemetry_last_pending_drop = tx->pending_drop_packets;
    tx->video_pending_peak = video_pending;
    tx->data_pending_peak = data_pending;
}

/****************************** Path统计 ******************************/

/**
 * @brief 记录单个Packet的最终发送结果。
 *
 * @note failed、dropped、expired三类统计互斥，只记录其中一种。
 */
static void _linkg_wifi_tx_record_terminal(linkg_path_t *path, const linkg_packet_t *packet, bool dropped, bool expired)
{
    uint64_t bytes;

    if (path == NULL || packet == NULL)
    {
        return;
    }

    bytes = packet->data_length;

    if (expired)
    {
        linkg_path_record_tx_expired(path, bytes, 1U);
        return;
    }

    if (dropped)
    {
        linkg_path_record_tx_dropped(path, bytes, 1U);
        return;
    }

    linkg_path_record_tx_failed(path, bytes, 1U);
}

/**
 * @brief 批量记录当前同步提交Packet的最终发送结果。
 */
static void _linkg_wifi_tx_record_current(linkg_path_t *path, linkg_packet_t *const *packets, uint32_t count, bool dropped)
{
    uint32_t index;

    if (path == NULL || packets == NULL || count == 0U)
    {
        return;
    }

    for (index = 0U; index < count; index++)
    {
        _linkg_wifi_tx_record_terminal(path, packets[index], dropped, false);
    }
}

/**
 * @brief 批量记录等待队列Packet的最终处理结果。
 *
 * @note lower_layer_failed为true时全部记录为底层最终发送失败；
 *       否则过期元素记录expired，其余元素记录本地主动dropped。
 */
static void _linkg_wifi_tx_record_queue(const linkg_wifi_tx_queue_item_t *items, uint32_t count, uint64_t now_us, uint64_t max_age_us, bool lower_layer_failed)
{
    uint64_t elapsed_us;
    uint32_t index;
    bool expired;

    if (items == NULL || count == 0U)
    {
        return;
    }

    for (index = 0U; index < count; index++)
    {
        if (lower_layer_failed)
        {
            _linkg_wifi_tx_record_terminal(items[index].path, items[index].packet, false, false);
            continue;
        }

        expired = false;

        if (max_age_us > 0U && now_us >= items[index].enqueue_us)
        {
            elapsed_us = now_us - items[index].enqueue_us;
            expired = elapsed_us >= max_age_us;
        }

        _linkg_wifi_tx_record_terminal(items[index].path, items[index].packet, !expired, expired);
    }
}

/****************************** 底层发送 ******************************/

/**
 * @brief 使用指定业务Socket同步批量发送当前提交Packet。
 *
 * @note 本函数借用Packet、Path和destination，仅在调用期间使用。
 */
static int _linkg_wifi_tx_send_current(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, uint32_t *sent_count)
{
    const struct sockaddr_in *target;
    uint64_t sent_bytes;
    uint32_t chunk_count;
    uint32_t index;
    uint32_t offset;
    int socket_fd;
    int ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || sent_count == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > tx->capacity)
    {
        return -EOVERFLOW;
    }

    *sent_count = 0U;
    socket_fd = tx->socket_fds[traffic_class];

    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    target = (const struct sockaddr_in *)&destination->address;

    pthread_mutex_lock(&tx->send_lock);

    for (index = 0U; index < count; index++)
    {
        tx->destinations[index] = *target;
        tx->destinations[index].sin_port = htons(tx->service_ports[traffic_class]);

        tx->iovecs[index].iov_base = (void *)linkg_packet_const_data(packets[index]);
        tx->iovecs[index].iov_len = packets[index]->data_length;

        memset(&tx->messages[index], 0, sizeof(tx->messages[index]));
        tx->messages[index].msg_hdr.msg_name = &tx->destinations[index];
        tx->messages[index].msg_hdr.msg_namelen = sizeof(tx->destinations[index]);
        tx->messages[index].msg_hdr.msg_iov = &tx->iovecs[index];
        tx->messages[index].msg_hdr.msg_iovlen = 1U;
    }

    offset = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (traffic_class != LINKG_WIFI_TRAFFIC_REALTIME &&
            chunk_count > LINKG_WIFI_TX_SENDMMSG_CHUNK_MAX)
        {
            chunk_count = LINKG_WIFI_TX_SENDMMSG_CHUNK_MAX;
        }

        do
        {
            ret = sendmmsg(socket_fd, &tx->messages[offset], chunk_count,
                           MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        while (ret < 0 && errno == EINTR);

        if (ret < 0)
        {
            if (offset == 0U)
            {
                ret = -errno;
                pthread_mutex_unlock(&tx->send_lock);
                return ret;
            }

            break;
        }

        if ((uint32_t)ret > chunk_count)
        {
            pthread_mutex_unlock(&tx->send_lock);
            return -EIO;
        }

        offset += (uint32_t)ret;

        if ((uint32_t)ret < chunk_count)
        {
            break;
        }

        if (traffic_class != LINKG_WIFI_TRAFFIC_REALTIME && offset < count)
        {
            sched_yield();
        }
    }

    *sent_count = offset;
    sent_bytes = 0U;

    for (index = 0U; index < *sent_count; index++)
    {
        sent_bytes += packets[index]->data_length;

        if (traffic_class != LINKG_WIFI_TRAFFIC_REALTIME)
        {
            tx->normal_sent_packets++;
            tx->normal_sent_bytes += packets[index]->data_length;
        }
    }

    pthread_mutex_unlock(&tx->send_lock);

    if (*sent_count > 0U)
    {
        linkg_path_record_tx_success(path, sent_bytes, *sent_count);
    }

    return 0;
}

/**
 * @brief 使用指定业务Socket同步批量发送等待队列Packet。
 *
 * @note items中的Packet和Path均借用Queue当前持有的引用。
 */
static int _linkg_wifi_tx_send_pending(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, const linkg_wifi_tx_queue_item_t *items, uint32_t count, uint32_t *sent_count)
{
    const struct sockaddr_in *target;
    uint32_t chunk_count;
    uint32_t index;
    uint32_t offset;
    int socket_fd;
    int ret;

    if (tx == NULL || items == NULL || sent_count == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > tx->capacity)
    {
        return -EOVERFLOW;
    }

    *sent_count = 0U;
    socket_fd = tx->socket_fds[traffic_class];

    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&tx->send_lock);

    for (index = 0U; index < count; index++)
    {
        target = (const struct sockaddr_in *)&items[index].destination.address;

        tx->destinations[index] = *target;
        tx->destinations[index].sin_port = htons(tx->service_ports[traffic_class]);

        tx->iovecs[index].iov_base = (void *)linkg_packet_const_data(items[index].packet);
        tx->iovecs[index].iov_len = items[index].packet->data_length;

        memset(&tx->messages[index], 0, sizeof(tx->messages[index]));
        tx->messages[index].msg_hdr.msg_name = &tx->destinations[index];
        tx->messages[index].msg_hdr.msg_namelen = sizeof(tx->destinations[index]);
        tx->messages[index].msg_hdr.msg_iov = &tx->iovecs[index];
        tx->messages[index].msg_hdr.msg_iovlen = 1U;
    }

    offset = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_WIFI_TX_SENDMMSG_CHUNK_MAX)
        {
            chunk_count = LINKG_WIFI_TX_SENDMMSG_CHUNK_MAX;
        }

        do
        {
            ret = sendmmsg(socket_fd, &tx->messages[offset], chunk_count,
                           MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        while (ret < 0 && errno == EINTR);

        if (ret < 0)
        {
            if (offset == 0U)
            {
                ret = -errno;
                pthread_mutex_unlock(&tx->send_lock);
                return ret;
            }

            break;
        }

        if ((uint32_t)ret > chunk_count)
        {
            pthread_mutex_unlock(&tx->send_lock);
            return -EIO;
        }

        offset += (uint32_t)ret;

        if ((uint32_t)ret < chunk_count)
        {
            break;
        }

        if (offset < count)
        {
            sched_yield();
        }
    }

    *sent_count = offset;

    for (index = 0U; index < *sent_count; index++)
    {
        tx->normal_sent_packets++;
        tx->normal_sent_bytes += items[index].packet->data_length;
    }

    pthread_mutex_unlock(&tx->send_lock);

    for (index = 0U; index < *sent_count; index++)
    {
        linkg_path_record_tx_success(items[index].path, items[index].packet->data_length, 1U);
    }

    return 0;
}

/****************************** 等待队列维护 ******************************/

/**
 * @brief 判断等待队列元素是否已经超过最大驻留时间。
 */
static bool _linkg_wifi_tx_queue_item_expired(const linkg_wifi_tx_queue_item_t *item, uint64_t now_us, uint64_t max_age_us)
{
    if (item == NULL || max_age_us == 0U)
    {
        return false;
    }

    if (now_us < item->enqueue_us)
    {
        return false;
    }

    return now_us - item->enqueue_us >= max_age_us;
}

/**
 * @brief 获取当前等待队列中可以安全连续发送的队首前缀长度。
 *
 * @note 返回前缀不会跨越失效Path、过期元素或Transport原子发送组边界。
 */
static uint32_t _linkg_wifi_tx_queue_sendable_prefix(const linkg_wifi_tx_queue_item_t *items, uint32_t count, uint64_t now_us, uint64_t max_age_us)
{
    uint32_t prefix;
    uint32_t index;

    if (items == NULL || count == 0U)
    {
        return 0U;
    }

    prefix = count;

    for (index = 0U; index < count; index++)
    {
        if (_linkg_wifi_tx_queue_item_expired(&items[index], now_us, max_age_us))
        {
            prefix = index;
            break;
        }

        if (!linkg_path_is_active(items[index].path))
        {
            prefix = index;
            break;
        }
    }

    return _linkg_wifi_tx_complete_queue_prefix(items, prefix);
}

/**
 * @brief 清理等待队首连续的过期或失效Path元素。
 *
 * @note 调用方必须持有normal_lock。
 */
static void _linkg_wifi_tx_purge_queue(linkg_wifi_tx_t *tx, linkg_wifi_tx_queue_t *queue, linkg_wifi_traffic_class_t traffic_class, uint64_t now_us)
{
    linkg_wifi_tx_queue_item_t items[LINKG_WIFI_TX_NORMAL_SEND_MAX];
    uint64_t max_age_us;
    uint32_t peek_count;
    uint32_t drop_count;
    uint32_t index;

    if (tx == NULL || queue == NULL)
    {
        return;
    }

    max_age_us = _linkg_wifi_tx_queue_max_age(traffic_class);

    while (linkg_wifi_tx_queue_count(queue) > 0U)
    {
        peek_count = linkg_wifi_tx_queue_peek_batch(queue, items, LINKG_WIFI_TX_NORMAL_SEND_MAX);
        if (peek_count == 0U)
        {
            return;
        }

        drop_count = 0U;

        for (index = 0U; index < peek_count; index++)
        {
            if (!_linkg_wifi_tx_queue_item_expired(&items[index], now_us, max_age_us) && linkg_path_is_active(items[index].path))
            {
                break;
            }

            drop_count++;
        }

        if (drop_count == 0U)
        {
            return;
        }

        _linkg_wifi_tx_record_queue(items, drop_count, now_us, max_age_us, false);
        tx->pending_drop_packets += drop_count;
        linkg_wifi_tx_queue_discard_batch(queue, drop_count);

        if (drop_count < peek_count)
        {
            return;
        }
    }
}

/**
 * @brief 丢弃等待队列中的全部元素并记录发送失败。
 *
 * @note 调用方必须持有normal_lock。
 */
static void _linkg_wifi_tx_flush_queue(linkg_wifi_tx_t *tx, linkg_wifi_tx_queue_t *queue)
{
    linkg_wifi_tx_queue_item_t items[LINKG_WIFI_TX_NORMAL_SEND_MAX];
    uint32_t peek_count;

    if (tx == NULL || queue == NULL)
    {
        return;
    }

    while (linkg_wifi_tx_queue_count(queue) > 0U)
    {
        peek_count = linkg_wifi_tx_queue_peek_batch(queue, items, LINKG_WIFI_TX_NORMAL_SEND_MAX);
        if (peek_count == 0U)
        {
            return;
        }

        _linkg_wifi_tx_record_queue(items, peek_count, 0U, 0U, false);
        linkg_wifi_tx_queue_discard_batch(queue, peek_count);
    }
}

/**
 * @brief 在当前流控预算内优先发送指定等待队列。
 *
 * @note 调用方必须持有normal_lock。remaining表示本轮剩余普通业务发送预算。
 */
static void _linkg_wifi_tx_drain_queue(linkg_wifi_tx_t *tx, linkg_wifi_tx_queue_t *queue, linkg_wifi_traffic_class_t traffic_class, uint64_t now_us, uint32_t *remaining, bool *blocked)
{
    linkg_wifi_tx_queue_item_t items[LINKG_WIFI_TX_NORMAL_SEND_MAX];
    uint64_t max_age_us;
    uint32_t peek_capacity;
    uint32_t peek_count;
    uint32_t send_count;
    uint32_t sent_count;
    int ret;

    if (tx == NULL || queue == NULL || remaining == NULL || blocked == NULL)
    {
        return;
    }

    if (*remaining == 0U || *blocked)
    {
        return;
    }

    max_age_us = _linkg_wifi_tx_queue_max_age(traffic_class);

    while (*remaining > 0U && !*blocked)
    {
        _linkg_wifi_tx_purge_queue(tx, queue, traffic_class, now_us);

        peek_capacity = *remaining;

        if (peek_capacity > tx->capacity)
        {
            peek_capacity = tx->capacity;
        }

        if (peek_capacity > LINKG_WIFI_TX_NORMAL_SEND_MAX)
        {
            peek_capacity = LINKG_WIFI_TX_NORMAL_SEND_MAX;
        }

        peek_count = linkg_wifi_tx_queue_peek_batch(queue, items, peek_capacity);
        if (peek_count == 0U)
        {
            return;
        }

        send_count = _linkg_wifi_tx_queue_sendable_prefix(items, peek_count, now_us, max_age_us);
        if (send_count == 0U)
        {
            *blocked = true;
            return;
        }

        sent_count = 0U;
        ret = _linkg_wifi_tx_send_pending(tx, traffic_class, items, send_count, &sent_count);
        if (ret != 0)
        {
            if (_linkg_wifi_tx_error_retryable(ret))
            {
                *blocked = true;
                return;
            }

            _linkg_wifi_tx_record_queue(items, send_count, 0U, 0U, true);
            tx->pending_drop_packets += send_count;
            linkg_wifi_tx_queue_discard_batch(queue, send_count);
            *blocked = true;
            return;
        }

        if (sent_count > 0U)
        {
            linkg_wifi_tx_queue_discard_batch(queue, sent_count);
            *remaining -= sent_count;
        }

        if (sent_count < send_count)
        {
            *blocked = true;
        }
    }
}

/****************************** 当前批次处理 ******************************/

/**
 * @brief 将当前未发送的VIDEO/DATA Packet批量加入等待队列。
 *
 * @note 调用方必须持有normal_lock。成功入队后Queue接管异步发送责任。
 */
static uint32_t _linkg_wifi_tx_enqueue_current(linkg_wifi_tx_t *tx, linkg_wifi_tx_queue_t *queue, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, const uint32_t *indices, uint32_t count, uint64_t enqueue_us, int *results)
{
    uint32_t pushed_count;
    uint32_t index;
    int ret;

    if (count == 0U)
    {
        return 0U;
    }

    pushed_count = 0U;
    ret = linkg_wifi_tx_queue_push_batch(queue, packets, count, path, destination, enqueue_us, &pushed_count);
    if (ret != 0)
    {
        for (index = 0U; index < count; index++)
        {
            results[indices[index]] = ret;
        }

        _linkg_wifi_tx_record_current(path, packets, count, true);
        return 0U;
    }

    for (index = 0U; index < pushed_count; index++)
    {
        results[indices[index]] = 0;
    }

    if (pushed_count < count)
    {
        for (index = pushed_count; index < count; index++)
        {
            results[indices[index]] = -ENOBUFS;
        }

        _linkg_wifi_tx_record_current(path, &packets[pushed_count], count - pushed_count, true);
        tx->pending_drop_packets += count - pushed_count;
    }

    return pushed_count;
}

/**
 * @brief 在当前流控预算内处理本次VIDEO/DATA同步提交。
 *
 * @note 调用方必须持有normal_lock。无法立即发送的Packet优先进入对应等待队列。
 */
static uint32_t _linkg_wifi_tx_send_current_normal(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, linkg_wifi_tx_queue_t *queue, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, const uint32_t *indices, uint32_t count, uint32_t *remaining, uint64_t now_us, int *results, bool *blocked)
{
    uint32_t attempt_count;
    uint32_t queued_count;
    uint32_t sent_count;
    uint32_t accepted_count;
    uint32_t index;
    int ret;

    if (count == 0U)
    {
        return 0U;
    }

    if (*remaining == 0U || *blocked)
    {
        return _linkg_wifi_tx_enqueue_current(tx, queue, path, destination, packets, indices, count, now_us, results);
    }

    attempt_count = count;

    if (attempt_count > *remaining)
    {
        attempt_count = *remaining;
    }
    attempt_count = _linkg_wifi_tx_complete_prefix(packets, count, attempt_count);
    if (attempt_count == 0U)
    {
        return _linkg_wifi_tx_enqueue_current(tx, queue, path, destination,
                                               packets, indices, count,
                                               now_us, results);
    }

    sent_count = 0U;
    ret = _linkg_wifi_tx_send_current(tx, traffic_class, path, destination, packets, attempt_count, &sent_count);

    if (ret != 0)
    {
        if (_linkg_wifi_tx_error_retryable(ret))
        {
            *blocked = true;
            return _linkg_wifi_tx_enqueue_current(tx, queue, path, destination, packets, indices, count, now_us, results);
        }

        for (index = 0U; index < attempt_count; index++)
        {
            results[indices[index]] = ret;
        }

        _linkg_wifi_tx_record_current(path, packets, attempt_count, false);
        accepted_count = 0U;

        if (attempt_count < count)
        {
            queued_count = _linkg_wifi_tx_enqueue_current(tx, queue, path, destination, &packets[attempt_count], &indices[attempt_count], count - attempt_count, now_us, results);
            accepted_count += queued_count;
        }

        *blocked = true;
        return accepted_count;
    }

    accepted_count = sent_count;

    for (index = 0U; index < sent_count; index++)
    {
        results[indices[index]] = 0;
    }

    *remaining -= sent_count;

    if (sent_count < attempt_count)
    {
        queued_count = _linkg_wifi_tx_enqueue_current(tx, queue, path, destination, &packets[sent_count], &indices[sent_count], count - sent_count, now_us, results);
        accepted_count += queued_count;
        *blocked = true;
        return accepted_count;
    }

    if (attempt_count < count)
    {
        queued_count = _linkg_wifi_tx_enqueue_current(tx, queue, path, destination, &packets[attempt_count], &indices[attempt_count], count - attempt_count, now_us, results);
        accepted_count += queued_count;
    }

    return accepted_count;
}

/****************************** REALTIME发送 ******************************/

/**
 * @brief 同步提交REALTIME Packet并直接尝试发送。
 *
 * @note REALTIME不经过流控控制器，也不进入等待队列。
 */
static int _linkg_wifi_tx_submit_realtime(linkg_wifi_tx_t *tx, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_packet_t *valid_packets[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t valid_indices[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t valid_count;
    uint32_t sent_count;
    uint32_t index;
    int ret;

    valid_count = 0U;

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;
        ret = _linkg_wifi_tx_validate_packet(packets[index]);
        if (ret != 0)
        {
            results[index] = ret;
            continue;
        }

        valid_packets[valid_count] = packets[index];
        valid_indices[valid_count] = index;
        valid_count++;
    }

    if (valid_count == 0U)
    {
        return 0;
    }

    if (!linkg_path_is_active(path))
    {
        for (index = 0U; index < valid_count; index++)
        {
            results[valid_indices[index]] = -ENODEV;
        }

        _linkg_wifi_tx_record_current(path, valid_packets, valid_count, true);
        return 0;
    }

    sent_count = 0U;
    ret = _linkg_wifi_tx_send_current(tx, LINKG_WIFI_TRAFFIC_REALTIME, path, destination, valid_packets, valid_count, &sent_count);
    if (ret != 0)
    {
        for (index = 0U; index < valid_count; index++)
        {
            results[valid_indices[index]] = ret;
        }

        _linkg_wifi_tx_record_current(path, valid_packets, valid_count, false);
        return 0;
    }

    for (index = 0U; index < sent_count; index++)
    {
        results[valid_indices[index]] = 0;
    }

    for (index = sent_count; index < valid_count; index++)
    {
        results[valid_indices[index]] = -EAGAIN;
    }

    if (sent_count < valid_count)
    {
        _linkg_wifi_tx_record_current(path, &valid_packets[sent_count], valid_count - sent_count, false);
    }

    return (int)sent_count;
}

/****************************** VIDEO/DATA发送 ******************************/

/**
 * @brief 提交VIDEO或DATA Packet并执行流控、等待队列和发送调度。
 */
static int _linkg_wifi_tx_submit_normal(linkg_wifi_tx_t *tx, linkg_path_t *path, linkg_wifi_traffic_class_t traffic_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_packet_t *valid_packets[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t valid_indices[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    linkg_wifi_tx_queue_t *current_queue;
    linkg_wifi_flowctrl_sample_t flowctrl_sample;
    uint64_t now_us;
    uint32_t accepted_count;
    uint32_t allowed_count;
    uint32_t demand_count;
    uint32_t requested_count;
    uint32_t valid_count;
    uint32_t remaining;
    uint32_t index;
    bool blocked;
    int ret;

    valid_count = 0U;

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;
        ret = _linkg_wifi_tx_validate_packet(packets[index]);
        if (ret != 0)
        {
            results[index] = ret;
            continue;
        }

        valid_packets[valid_count] = packets[index];
        valid_indices[valid_count] = index;
        valid_count++;
    }

    if (valid_count == 0U)
    {
        return 0;
    }

    current_queue = _linkg_wifi_tx_select_queue(tx, traffic_class);
    if (current_queue == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&tx->normal_lock);

    if (!atomic_load(&tx->started))
    {
        pthread_mutex_unlock(&tx->normal_lock);
        return -ENODEV;
    }

    if (!linkg_path_is_active(path))
    {
        for (index = 0U; index < valid_count; index++)
        {
            results[valid_indices[index]] = -ENODEV;
        }

        _linkg_wifi_tx_record_current(path, valid_packets, valid_count, true);
        pthread_mutex_unlock(&tx->normal_lock);
        return 0;
    }

    now_us = linkg_time_monotonic_us();

    _linkg_wifi_tx_purge_queue(tx, tx->video_queue, LINKG_WIFI_TRAFFIC_VIDEO, now_us);
    _linkg_wifi_tx_purge_queue(tx, tx->data_queue, LINKG_WIFI_TRAFFIC_DATA, now_us);

    demand_count = linkg_wifi_tx_queue_count(tx->video_queue) + linkg_wifi_tx_queue_count(tx->data_queue) + valid_count;
    requested_count = demand_count;

    if (requested_count > LINKG_WIFI_TX_NORMAL_SEND_MAX)
    {
        requested_count = LINKG_WIFI_TX_NORMAL_SEND_MAX;
    }

    memset(&flowctrl_sample, 0, sizeof(flowctrl_sample));

    allowed_count = requested_count;
    ret = linkg_wifi_flowctrl_admit(tx->flowctrl, requested_count, &allowed_count, &flowctrl_sample);
    if (ret != 0)
    {
        allowed_count = requested_count;
    }

    remaining = allowed_count;
    blocked = false;
    accepted_count = 0U;

    _linkg_wifi_tx_drain_queue(tx, tx->video_queue, LINKG_WIFI_TRAFFIC_VIDEO, now_us, &remaining, &blocked);

    if (traffic_class == LINKG_WIFI_TRAFFIC_VIDEO)
    {
        accepted_count += _linkg_wifi_tx_send_current_normal(tx, traffic_class, current_queue, path, destination, valid_packets, valid_indices, valid_count, &remaining, now_us, results, &blocked);

        if (!blocked && remaining > 0U)
        {
            _linkg_wifi_tx_drain_queue(tx, tx->data_queue, LINKG_WIFI_TRAFFIC_DATA, now_us, &remaining, &blocked);
        }
    }
    else
    {
        if (!blocked && remaining > 0U)
        {
            _linkg_wifi_tx_drain_queue(tx, tx->data_queue, LINKG_WIFI_TRAFFIC_DATA, now_us, &remaining, &blocked);
        }

        accepted_count += _linkg_wifi_tx_send_current_normal(tx, traffic_class, current_queue, path, destination, valid_packets, valid_indices, valid_count, &remaining, now_us, results, &blocked);
    }

    demand_count = linkg_wifi_tx_queue_count(tx->video_queue);
    if (demand_count > tx->video_pending_peak)
    {
        tx->video_pending_peak = demand_count;
    }

    demand_count = linkg_wifi_tx_queue_count(tx->data_queue);
    if (demand_count > tx->data_pending_peak)
    {
        tx->data_pending_peak = demand_count;
    }

    _linkg_wifi_tx_report(tx, &flowctrl_sample, ret, now_us);

    pthread_mutex_unlock(&tx->normal_lock);

    return (int)accepted_count;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi发送模块。
 *
 * @note socket_fds和service_ports仅借用，生命周期由Wi-Fi Link保证。
 */
linkg_wifi_tx_t *linkg_wifi_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports)
{
    linkg_wifi_tx_t *tx;
    int ret;

    if (capacity == 0U || socket_fds == NULL || service_ports == NULL)
    {
        return NULL;
    }

    tx = calloc(1, sizeof(*tx));
    if (tx == NULL)
    {
        return NULL;
    }

    tx->capacity = capacity;
    tx->socket_fds = socket_fds;
    tx->service_ports = service_ports;
    atomic_store(&tx->started, false);

    ret = pthread_mutex_init(&tx->normal_lock, NULL);
    if (ret != 0)
    {
        goto fail_tx;
    }

    ret = pthread_mutex_init(&tx->send_lock, NULL);
    if (ret != 0)
    {
        goto fail_normal_lock;
    }

    tx->flowctrl = linkg_wifi_flowctrl_create();
    if (tx->flowctrl == NULL)
    {
        goto fail_send_lock;
    }

    tx->video_queue = linkg_wifi_tx_queue_create(LINKG_WIFI_TX_VIDEO_QUEUE_CAPACITY);
    if (tx->video_queue == NULL)
    {
        goto fail_flowctrl;
    }

    tx->data_queue = linkg_wifi_tx_queue_create(LINKG_WIFI_TX_DATA_QUEUE_CAPACITY);
    if (tx->data_queue == NULL)
    {
        goto fail_video_queue;
    }

    tx->messages = calloc(capacity, sizeof(*tx->messages));
    if (tx->messages == NULL)
    {
        goto fail_data_queue;
    }

    tx->iovecs = calloc(capacity, sizeof(*tx->iovecs));
    if (tx->iovecs == NULL)
    {
        goto fail_messages;
    }

    tx->destinations = calloc(capacity, sizeof(*tx->destinations));
    if (tx->destinations == NULL)
    {
        goto fail_iovecs;
    }

    return tx;

fail_iovecs:
    free(tx->iovecs);

fail_messages:
    free(tx->messages);

fail_data_queue:
    linkg_wifi_tx_queue_destroy(tx->data_queue);

fail_video_queue:
    linkg_wifi_tx_queue_destroy(tx->video_queue);

fail_flowctrl:
    linkg_wifi_flowctrl_destroy(tx->flowctrl);

fail_send_lock:
    pthread_mutex_destroy(&tx->send_lock);

fail_normal_lock:
    pthread_mutex_destroy(&tx->normal_lock);

fail_tx:
    free(tx);
    return NULL;
}

/**
 * @brief 销毁Wi-Fi发送模块。
 *
 * @note 调用前不能再有其他线程访问tx。
 */
void linkg_wifi_tx_destroy(linkg_wifi_tx_t *tx)
{
    if (tx == NULL)
    {
        return;
    }

    linkg_wifi_tx_stop(tx);

    free(tx->destinations);
    free(tx->iovecs);
    free(tx->messages);

    linkg_wifi_tx_queue_destroy(tx->data_queue);
    linkg_wifi_tx_queue_destroy(tx->video_queue);
    linkg_wifi_flowctrl_destroy(tx->flowctrl);

    pthread_mutex_destroy(&tx->send_lock);
    pthread_mutex_destroy(&tx->normal_lock);

    free(tx);
}

/**
 * @brief 启动Wi-Fi发送模块。
 */
int linkg_wifi_tx_start(linkg_wifi_tx_t *tx)
{
    uint32_t index;
    int ret;

    if (tx == NULL)
    {
        return -EINVAL;
    }

    if (atomic_load(&tx->started))
    {
        return -EALREADY;
    }

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
    {
        if (tx->socket_fds[index] < 0)
        {
            return -ENODEV;
        }
    }

    ret = linkg_wifi_flowctrl_start(tx->flowctrl);
    if (ret != 0)
    {
        LINKG_LOG_WARN("WIFI: TX flowctrl unavailable, error=%d, action=fail_open", ret);
    }

    atomic_store(&tx->started, true);

    return 0;
}

/**
 * @brief 停止Wi-Fi发送模块并清空普通业务等待队列。
 */
void linkg_wifi_tx_stop(linkg_wifi_tx_t *tx)
{
    if (tx == NULL)
    {
        return;
    }

    if (!atomic_exchange(&tx->started, false))
    {
        return;
    }

    pthread_mutex_lock(&tx->normal_lock);

    linkg_wifi_flowctrl_stop(tx->flowctrl);
    _linkg_wifi_tx_flush_queue(tx, tx->video_queue);
    _linkg_wifi_tx_flush_queue(tx, tx->data_queue);

    pthread_mutex_unlock(&tx->normal_lock);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 提交同一业务类别的数据包。
 *
 * results[index]为0表示Wi-Fi已经接受该Packet的发送责任；负数表示本次拒绝。
 */
int linkg_wifi_tx_submit(linkg_wifi_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_wifi_traffic_class_t traffic_class;
    int ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > tx->capacity || count > LINKG_LINK_TX_BATCH_SIZE_DEFAULT)
    {
        return -EOVERFLOW;
    }

    if (!atomic_load(&tx->started))
    {
        return -ENODEV;
    }

    ret = _linkg_wifi_tx_class_to_traffic(tx_class, &traffic_class);
    if (ret != 0)
    {
        return ret;
    }

    if (tx->socket_fds[traffic_class] < 0)
    {
        return -ENODEV;
    }

    ret = _linkg_wifi_tx_validate_destination(tx, destination);
    if (ret != 0)
    {
        return ret;
    }

    if (traffic_class == LINKG_WIFI_TRAFFIC_REALTIME)
    {
        return _linkg_wifi_tx_submit_realtime(tx, path, destination, packets, count, results);
    }

    return _linkg_wifi_tx_submit_normal(tx, path, traffic_class, destination, packets, count, results);
}
