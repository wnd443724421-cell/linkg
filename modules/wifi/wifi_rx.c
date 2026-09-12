/**
 * @file wifi_rx.c
 * @brief LinkG Wi-Fi接收模块实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-12
 */

#define _GNU_SOURCE

#include "wifi_rx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_network_ops.h"
#include "linkg_time.h"

#include "wifi_internal.h"
#include "wifi_rx_queue.h"
#include "wifi_traffic.h"
#include "wifi_wire.h"

/****************************** 模块常量 ******************************/

#define LINKG_WIFI_RX_IPV4_HEADER_SIZE          20U                                                                                  // IPv4最小头部长度
#define LINKG_WIFI_RX_UDP_HEADER_SIZE           8U                                                                                   // UDP头部长度
#define LINKG_WIFI_RX_MTU                       1500U                                                                                // Wi-Fi接口MTU
#define LINKG_WIFI_RX_UDP_PAYLOAD_MAX           (LINKG_WIFI_RX_MTU - LINKG_WIFI_RX_IPV4_HEADER_SIZE - LINKG_WIFI_RX_UDP_HEADER_SIZE) // 单个UDP报文最大负载
#define LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX     (LINKG_WIFI_RX_UDP_PAYLOAD_MAX - LINKG_WIFI_WIRE_HEADER_SIZE)                        // Wi-Fi私有头之后的最大Transport报文长度
#define LINKG_WIFI_RX_BATCH_SIZE_MAX            32U                                                                                  // 单次Socket接收和上层交付最大Packet数量
#define LINKG_WIFI_RX_QUEUE_BATCH_COUNT         5U                                                                                   // 单业务接收队列最多缓存批次数
#define LINKG_WIFI_RX_EPOLL_EVENT_COUNT         (LINKG_WIFI_TRAFFIC_COUNT + 1U)                                                      // 三业务Socket和Worker唤醒描述符数量
#define LINKG_WIFI_RX_WORKER_WAKEUP_EVENT       LINKG_WIFI_TRAFFIC_COUNT                                                             // Worker唤醒事件索引
#define LINKG_WIFI_RX_WORKER_CPU_CORE           1                                                                                    // RX生产线程绑定CPU1
#define LINKG_WIFI_RX_RETRY_MS                  1U                                                                                   // Queue或Packet Pool暂不可用时重试间隔
#define LINKG_WIFI_NODE_SLOT_COUNT              256U                                                                                 // uint8_t Node ID直接索引空间
#define LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE      64U                                                                                  // Wi-Fi链路Sequence乱序窗口大小

_Static_assert(LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE == 64U, "Wi-Fi RX sequence window must be 64");

/****************************** 内部类型 ******************************/

/**
 * @brief 单个Wi-Fi对端接收Session、Sequence窗口与累计统计状态。
 *
 * session_id由发送端Wi-Fi TX对象创建时生成，整个TX生命周期内保持不变。
 * 接收端发现新的session_id时直接重建Sequence窗口，但累计统计保持单调递增。
 * previous_session_id只用于丢弃上一代Session的迟到报文，避免窗口回切。
 */
typedef struct
{
    uint32_t session_id;             // 当前对端Wi-Fi TX Session ID
    uint32_t previous_session_id;    // 上一个已经退役的Wi-Fi TX Session ID
    uint32_t highest_sequence;       // 当前Session窗口最高Sequence
    uint32_t window_span;            // 当前Sequence窗口有效跨度
    uint64_t received_bitmap;        // bit0表示highest_sequence，向高位表示更旧Sequence
    uint64_t received_packets;       // 累计唯一成功接收Wi-Fi Packet数量
    uint64_t confirmed_lost_packets; // 累计64窗口确认丢失Packet数量
    bool     initialized;            // 当前节点是否已经建立Session窗口
} linkg_wifi_rx_peer_stats_t;

/**
 * @brief Wi-Fi接收生产线程预分配Scratch。
 */
typedef struct
{
    linkg_wifi_rx_queue_item_t items[LINKG_WIFI_RX_BATCH_SIZE_MAX];          // 当前接收完成Queue元素
    linkg_packet_t            *packets[LINKG_WIFI_RX_BATCH_SIZE_MAX];        // 当前从Packet Pool申请的Packet
    linkg_wifi_wire_header_t   headers[LINKG_WIFI_RX_BATCH_SIZE_MAX];        // 当前批次Wi-Fi Wire头
    uint32_t                   session_ids[LINKG_WIFI_RX_BATCH_SIZE_MAX];    // 当前有效Packet的Host字节序Session ID
    uint32_t                   sequences[LINKG_WIFI_RX_BATCH_SIZE_MAX];      // 当前有效Packet的Host字节序Sequence
    uint8_t                    peer_node_ids[LINKG_WIFI_RX_BATCH_SIZE_MAX];  // 当前有效Packet对应Node ID
    struct mmsghdr             messages[LINKG_WIFI_RX_BATCH_SIZE_MAX];       // recvmmsg消息数组
    struct iovec               iovecs[LINKG_WIFI_RX_BATCH_SIZE_MAX][2];      // Wi-Fi头+Transport Packet双iovec
} linkg_wifi_rx_scratch_t;

/**
 * @brief Wi-Fi接收模块运行上下文。
 */
struct linkg_wifi_rx
{
    pthread_t                  worker;                                       // Socket接收生产线程
    linkg_wifi_rx_scratch_t    scratch;                                      // 生产线程预分配Scratch
    linkg_wifi_rx_queue_item_t consume_items[LINKG_WIFI_RX_BATCH_SIZE_MAX];  // Link RX消费临时元素
    linkg_wifi_rx_queue_t     *queues[LINKG_WIFI_TRAFFIC_COUNT];             // 三业务SPSC接收FIFO

    pthread_mutex_t            peer_stats_lock;                              // Peer Session、Sequence窗口和累计统计锁
    linkg_wifi_rx_peer_stats_t peer_stats[LINKG_WIFI_NODE_SLOT_COUNT];       // 按Node ID直接索引的Wi-Fi接收统计

    linkg_packet_pool_t       *packet_pool;                                  // 借用Link Packet Pool
    int                       *socket_fds;                                   // 借用Wi-Fi Link业务Socket数组
    const uint16_t            *service_ports;                                // 借用Wi-Fi Link业务端口数组

    uint32_t                   capacity;                                     // 单次接收和交付最大Packet数量
    uint32_t                   queue_capacity;                               // 单业务接收队列最大Packet数量

    int                        socket_epoll_fd;                              // Worker监听三业务Socket的epoll描述符
    int                        worker_wakeup_fd;                             // 停止时唤醒Worker的eventfd
    int                        notify_fd;                                    // 通知Link RX存在已完成Packet的eventfd

    bool                       worker_created;                               // Worker线程是否已经创建
    _Atomic bool               started;                                      // 接收模块运行状态
    _Atomic bool               notify_pending;                               // Link RX通知周期是否已经激活
};

/****************************** 内部辅助 ******************************/


/**
 * @brief 生成最低bits位为1的64位掩码。
 */
static uint64_t _linkg_wifi_rx_low_mask(uint32_t bits)
{
    if (bits == 0U)
    {
        return 0U;
    }

    if (bits >= LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE)
    {
        return UINT64_MAX;
    }

    return (UINT64_C(1) << bits) - UINT64_C(1);
}

/**
 * @brief 统计64位Bitmap中的置位数量。
 */
static uint32_t _linkg_wifi_rx_popcount(uint64_t bitmap)
{
    return (uint32_t)__builtin_popcountll((unsigned long long)bitmap);
}

/**
 * @brief 使用指定Session和Sequence建立当前Peer接收窗口基线。
 *
 * 累计received/lost统计不在Session切换时清零，只重建当前Sequence窗口。
 */
static void _linkg_wifi_rx_set_baseline(linkg_wifi_rx_peer_stats_t *peer_stats, uint32_t session_id, uint32_t sequence)
{
    peer_stats->session_id        = session_id;
    peer_stats->highest_sequence  = sequence;
    peer_stats->window_span       = 1U;
    peer_stats->received_bitmap   = UINT64_C(1);
    peer_stats->received_packets++;
    peer_stats->initialized       = true;
}

/**
 * @brief 计算当前Sequence窗口向前滑动时被推出窗口的确认丢包数量。
 */
static uint64_t _linkg_wifi_rx_count_shifted_lost(const linkg_wifi_rx_peer_stats_t *peer_stats, uint32_t shift)
{
    uint64_t valid_mask;
    uint64_t shifted_mask;
    uint32_t shifted_count;
    uint32_t received_count;

    if (shift == 0U || peer_stats->window_span == 0U)
    {
        return 0U;
    }

    valid_mask = _linkg_wifi_rx_low_mask(peer_stats->window_span);

    if (shift >= LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE)
    {
        received_count = _linkg_wifi_rx_popcount(peer_stats->received_bitmap & valid_mask);

        return (uint64_t)(peer_stats->window_span - received_count) +
               (uint64_t)(shift - LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE);
    }

    if (peer_stats->window_span <= LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE - shift)
    {
        return 0U;
    }

    shifted_mask = valid_mask &
                   ~_linkg_wifi_rx_low_mask(LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE - shift);

    shifted_count  = peer_stats->window_span - (LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE - shift);
    received_count = _linkg_wifi_rx_popcount(peer_stats->received_bitmap & shifted_mask);

    return (uint64_t)(shifted_count - received_count);
}

/**
 * @brief 更新当前Session内的Wi-Fi Sequence窗口和累计统计。
 *
 * 主窗口使用64位Bitmap容忍有限乱序。缺口只有被窗口真正推出后才计入
 * confirmed_lost_packets。重复包和已经超出确认窗口的迟到包不增加公开累计统计。
 */
static void _linkg_wifi_rx_record_sequence_locked(linkg_wifi_rx_peer_stats_t *peer_stats, uint32_t sequence)
{
    uint64_t bit;
    uint64_t confirmed_lost;
    uint32_t distance;
    uint32_t shift;
    int32_t  delta;

    delta = (int32_t)(sequence - peer_stats->highest_sequence);

    if (delta > 0)
    {
        shift          = (uint32_t)delta;
        confirmed_lost = _linkg_wifi_rx_count_shifted_lost(peer_stats, shift);

        peer_stats->confirmed_lost_packets += confirmed_lost;

        if (shift >= LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE)
        {
            peer_stats->received_bitmap = UINT64_C(1);
            peer_stats->window_span     = LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE;
        }
        else
        {
            peer_stats->received_bitmap =
                (peer_stats->received_bitmap << shift) | UINT64_C(1);

            peer_stats->window_span += shift;

            if (peer_stats->window_span > LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE)
            {
                peer_stats->window_span = LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE;
            }
        }

        peer_stats->highest_sequence = sequence;
        peer_stats->received_packets++;
        return;
    }

    if (delta == 0)
    {
        return;
    }

    distance = peer_stats->highest_sequence - sequence;

    if (distance >= peer_stats->window_span)
    {
        return;
    }

    bit = UINT64_C(1) << distance;

    if ((peer_stats->received_bitmap & bit) != 0U)
    {
        return;
    }

    peer_stats->received_bitmap |= bit;
    peer_stats->received_packets++;
}

/**
 * @brief 更新指定Peer的Wi-Fi Session和Sequence接收状态。
 *
 * 首次收到Peer报文时直接建立Session基线。同一Session按64位窗口统计。
 * 收到新的Session时将当前Session保存为previous_session_id并立即建立新窗口，
 * 不把旧窗口未确认缺口计为丢包。上一代Session的迟到报文直接拒绝。
 *
 * @return true表示当前Packet属于有效当前Session，可继续向Transport交付；
 *         false表示属于上一代已经退役的Session，应在Wi-Fi RX层丢弃。
 *
 * @note 调用方必须持有rx->peer_stats_lock。
 */
static bool _linkg_wifi_rx_record_wire_locked(linkg_wifi_rx_t *rx, uint8_t peer_node_id, uint32_t session_id, uint32_t sequence)
{
    linkg_wifi_rx_peer_stats_t *peer_stats;

    peer_stats = &rx->peer_stats[peer_node_id];

    if (!peer_stats->initialized)
    {
        _linkg_wifi_rx_set_baseline(peer_stats, session_id, sequence);
        return true;
    }

    if (session_id == peer_stats->session_id)
    {
        _linkg_wifi_rx_record_sequence_locked(peer_stats, sequence);
        return true;
    }

    if (session_id == peer_stats->previous_session_id)
    {
        return false;
    }

    peer_stats->previous_session_id = peer_stats->session_id;
    
    _linkg_wifi_rx_set_baseline(peer_stats, session_id, sequence);

    return true;
}

/**
 * @brief 从Wi-Fi对端IPv4提取Node ID。
 */
static int _linkg_wifi_rx_source_node_id(const struct sockaddr_in *source, uint8_t *peer_node_id)
{
    uint32_t host_address;
    uint8_t  node_id;

    if (source == NULL || peer_node_id == NULL)
    {
        return -EINVAL;
    }

    if (source->sin_family != AF_INET)
    {
        return -EAFNOSUPPORT;
    }

    host_address = ntohl(source->sin_addr.s_addr);
    node_id      = (uint8_t)(host_address & 0xFFU);

    if (node_id == 0U || node_id == UINT8_MAX)
    {
        return -EINVAL;
    }

    *peer_node_id = node_id;

    return 0;
}

/**
 * @brief 关闭并清空单个描述符。
 */
static int _linkg_wifi_rx_close_fd(int *descriptor)
{
    int value;
    int ret;

    if (descriptor == NULL)
    {
        return -EINVAL;
    }

    value       = *descriptor;
    *descriptor = -1;

    if (value < 0)
    {
        return 0;
    }

    ret = close(value);
    if (ret != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 将描述符加入Worker接收epoll。
 */
static int _linkg_wifi_rx_register_fd(int epoll_fd, int descriptor, uint32_t event_index)
{
    struct epoll_event event;
    int                ret;

    if (epoll_fd < 0 || descriptor < 0)
    {
        return -EINVAL;
    }

    memset(&event, 0, sizeof(event));

    event.events   = EPOLLIN;
    event.data.u32 = event_index;

    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, descriptor, &event);
    if (ret != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 写入eventfd唤醒指定等待方。
 */
static int _linkg_wifi_rx_signal_fd(int descriptor)
{
    eventfd_t value;
    int       ret;

    if (descriptor < 0)
    {
        return -ENODEV;
    }

    value = 1U;

    do
    {
        ret = eventfd_write(descriptor, value);
    }
    while (ret != 0 && errno == EINTR);

    if (ret != 0)
    {
        if (errno == EAGAIN)
        {
            return 0;
        }

        return -errno;
    }

    return 0;
}

/**
 * @brief 清除Link RX接收通知eventfd。
 */
static int _linkg_wifi_rx_clear_notify(linkg_wifi_rx_t *rx)
{
    eventfd_t value;
    int       ret;

    do
    {
        ret = eventfd_read(rx->notify_fd, &value);
    }
    while (ret != 0 && errno == EINTR);

    if (ret != 0)
    {
        if (errno == EAGAIN)
        {
            return 0;
        }

        return -errno;
    }

    return 0;
}

/**
 * @brief 判断三个接收Queue是否全部为空。
 */
static bool _linkg_wifi_rx_queues_empty(const linkg_wifi_rx_t *rx)
{
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        if (linkg_wifi_rx_queue_count(rx->queues[class_index]) > 0U)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 在当前通知周期首次入队时唤醒Link RX。
 */
static int _linkg_wifi_rx_notify_link(linkg_wifi_rx_t *rx)
{
    int ret;

    if (atomic_exchange_explicit(&rx->notify_pending, true, memory_order_acq_rel))
    {
        return 0;
    }

    ret = _linkg_wifi_rx_signal_fd(rx->notify_fd);
    if (ret != 0)
    {
        atomic_store_explicit(&rx->notify_pending, false, memory_order_release);
        return ret;
    }

    return 0;
}

/**
 * @brief 根据接收业务类别设置Packet业务标志。
 */
static void _linkg_wifi_rx_set_received_class(linkg_packet_t *packet, linkg_wifi_traffic_class_t traffic_class)
{
    linkg_packet_set_data(packet);

    if (traffic_class == LINKG_WIFI_TRAFFIC_REALTIME)
    {
        linkg_packet_set_realtime(packet, true);
    }
    else if (traffic_class == LINKG_WIFI_TRAFFIC_VIDEO)
    {
        linkg_packet_set_video(packet, true);
    }
}

/**
 * @brief 获取指定业务Socket当前待处理错误。
 */
static int _linkg_wifi_rx_socket_error(const linkg_wifi_rx_t *rx, linkg_wifi_traffic_class_t traffic_class)
{
    socklen_t error_length;
    int       socket_error;
    int       ret;

    socket_error = 0;
    error_length = sizeof(socket_error);

    ret = getsockopt(rx->socket_fds[traffic_class], SOL_SOCKET, SO_ERROR, &socket_error, &error_length);
    if (ret != 0)
    {
        return -errno;
    }

    return socket_error == 0 ? -EIO : -socket_error;
}

/**
 * @brief 释放当前Scratch中尚未转移所有权的Packet。
 */
static void _linkg_wifi_rx_release_packets(linkg_wifi_rx_scratch_t *scratch, uint32_t count)
{
    uint32_t index;

    for (index = 0U; index < count; index++)
    {
        if (scratch->packets[index] != NULL)
        {
            linkg_packet_release(scratch->packets[index]);
            scratch->packets[index] = NULL;
        }
    }
}

/**
 * @brief 对指定业务Socket执行一次批量接收并将有效Packet加入对应Queue。
 *
 * Packet从Pool获得基础引用。recvmmsg使用双iovec将Wi-Fi Wire头直接接收
 * 到Scratch，将原Transport Packet直接写入Packet Pool。成功入队后基础引用
 * 转移给Queue，无效报文、未使用Packet和异常未入队Packet由本函数释放。
 */
static int _linkg_wifi_rx_receive_once(linkg_wifi_rx_t *rx, linkg_wifi_traffic_class_t traffic_class)
{
    linkg_wifi_rx_scratch_t *scratch;
    linkg_wifi_rx_queue_t   *queue;
    struct sockaddr_in      *source;
    linkg_packet_t          *packet;
    uint32_t                 packet_capacity;
    uint32_t                 transport_capacity;
    uint32_t                 transport_length;
    uint32_t                 request_count;
    uint32_t                 allocated_count;
    uint32_t                 received_count;
    uint32_t                 valid_count;
    uint32_t                 deliver_count;
    uint32_t                 pushed_count;
    uint32_t                 available;
    uint32_t                 session_id;
    uint32_t                 sequence;
    uint32_t                 index;
    uint8_t                  peer_node_id;
    int                      socket_fd;
    int                      lock_ret;
    int                      ret;

    queue     = rx->queues[traffic_class];
    socket_fd = rx->socket_fds[traffic_class];
    scratch   = &rx->scratch;

    available = linkg_wifi_rx_queue_available(queue);
    if (available == 0U)
    {
        return 0;
    }

    request_count = available;
    if (request_count > rx->capacity)
    {
        request_count = rx->capacity;
    }

    memset(scratch->packets, 0, request_count * sizeof(scratch->packets[0]));

    allocated_count = linkg_packet_pool_alloc_batch(rx->packet_pool, scratch->packets, request_count);
    if (allocated_count == 0U)
    {
        return 0;
    }

    memset(scratch->messages, 0, allocated_count * sizeof(scratch->messages[0]));

    for (index = 0U; index < allocated_count; index++)
    {
        packet          = scratch->packets[index];
        packet_capacity = linkg_packet_capacity(packet);

        if (packet_capacity == 0U)
        {
            _linkg_wifi_rx_release_packets(scratch, allocated_count);
            return -ENOBUFS;
        }

        transport_capacity = packet_capacity;
        if (transport_capacity > LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX)
        {
            transport_capacity = LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX;
        }

        packet->data_length = 0U;
        linkg_packet_set_data(packet);

        scratch->items[index].packet = packet;
        memset(&scratch->items[index].source, 0, sizeof(scratch->items[index].source));

        scratch->iovecs[index][0].iov_base = &scratch->headers[index];
        scratch->iovecs[index][0].iov_len  = LINKG_WIFI_WIRE_HEADER_SIZE;

        scratch->iovecs[index][1].iov_base = linkg_packet_data(packet);
        scratch->iovecs[index][1].iov_len  = transport_capacity;

        scratch->messages[index].msg_hdr.msg_name    = &scratch->items[index].source.address;
        scratch->messages[index].msg_hdr.msg_namelen = sizeof(scratch->items[index].source.address);
        scratch->messages[index].msg_hdr.msg_iov     = scratch->iovecs[index];
        scratch->messages[index].msg_hdr.msg_iovlen  = 2U;
    }

    do
    {
        ret = recvmmsg(socket_fd, scratch->messages, allocated_count, MSG_DONTWAIT, NULL);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        ret = -errno;

        _linkg_wifi_rx_release_packets(scratch, allocated_count);

        if (ret == -EAGAIN || ret == -EWOULDBLOCK)
        {
            return 0;
        }

        return ret;
    }

    received_count = (uint32_t)ret;
    valid_count    = 0U;

    for (index = 0U; index < received_count; index++)
    {
        packet = scratch->packets[index];
        scratch->items[index].source.length = scratch->messages[index].msg_hdr.msg_namelen;

        if ((scratch->messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0 ||
            scratch->messages[index].msg_len <= LINKG_WIFI_WIRE_HEADER_SIZE ||
            scratch->messages[index].msg_len > LINKG_WIFI_RX_UDP_PAYLOAD_MAX ||
            scratch->items[index].source.length != sizeof(struct sockaddr_in) ||
            scratch->items[index].source.address.ss_family != AF_INET)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        transport_length = scratch->messages[index].msg_len - LINKG_WIFI_WIRE_HEADER_SIZE;

        if (transport_length == 0U ||
            transport_length > scratch->iovecs[index][1].iov_len)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        source = (struct sockaddr_in *)&scratch->items[index].source.address;

        if (!linkg_network_ipv4_address_valid(&source->sin_addr) ||
            source->sin_port != htons(rx->service_ports[traffic_class]))
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        ret = _linkg_wifi_rx_source_node_id(source, &peer_node_id);
        if (ret != 0)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        session_id = ntohl(scratch->headers[index].session_id);
        sequence   = ntohl(scratch->headers[index].sequence);

        if (session_id == 0U)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        // 统一使用DATA端口表示同一Wi-Fi Peer的规范化Endpoint。
        source->sin_port = htons(rx->service_ports[LINKG_WIFI_TRAFFIC_DATA]);

        packet->data_length = transport_length;
        _linkg_wifi_rx_set_received_class(packet, traffic_class);

        if (valid_count != index)
        {
            scratch->items[valid_count] = scratch->items[index];
        }

        scratch->peer_node_ids[valid_count] = peer_node_id;
        scratch->session_ids[valid_count]   = session_id;
        scratch->sequences[valid_count]     = sequence;

        scratch->packets[index] = NULL;
        valid_count++;
    }

    for (index = received_count; index < allocated_count; index++)
    {
        if (scratch->packets[index] != NULL)
        {
            linkg_packet_release(scratch->packets[index]);
            scratch->packets[index] = NULL;
        }
    }

    if (valid_count == 0U)
    {
        return 0;
    }

    /**
     * Wi-Fi链路统计在Socket成功接收并完成Wire校验后立即更新，
     * 不受后续用户态RX Queue是否成功入队影响。
     */
    lock_ret = pthread_mutex_lock(&rx->peer_stats_lock);
    if (lock_ret != 0)
    {
        for (index = 0U; index < valid_count; index++)
        {
            if (scratch->items[index].packet != NULL)
            {
                linkg_packet_release(scratch->items[index].packet);
                scratch->items[index].packet = NULL;
            }
        }

        return -lock_ret;
    }

    deliver_count = 0U;

    for (index = 0U; index < valid_count; index++)
    {
        if (!_linkg_wifi_rx_record_wire_locked(rx,
                                               scratch->peer_node_ids[index],
                                               scratch->session_ids[index],
                                               scratch->sequences[index]))
        {
            if (scratch->items[index].packet != NULL)
            {
                linkg_packet_release(scratch->items[index].packet);
                scratch->items[index].packet = NULL;
            }

            continue;
        }

        if (deliver_count != index)
        {
            scratch->items[deliver_count] = scratch->items[index];
            scratch->items[index].packet  = NULL;
        }

        deliver_count++;
    }

    pthread_mutex_unlock(&rx->peer_stats_lock);

    valid_count = deliver_count;
    if (valid_count == 0U)
    {
        return 0;
    }

    pushed_count = linkg_wifi_rx_queue_push_batch(queue, scratch->items, valid_count);

    // 预留Queue空间后正常情况下必须完整入队，异常剩余Packet仍由生产线程释放。
    for (index = pushed_count; index < valid_count; index++)
    {
        if (scratch->items[index].packet != NULL)
        {
            linkg_packet_release(scratch->items[index].packet);
            scratch->items[index].packet = NULL;
        }
    }

    for (index = 0U; index < pushed_count; index++)
    {
        scratch->items[index].packet = NULL;
    }

    if (pushed_count > 0U)
    {
        ret = _linkg_wifi_rx_notify_link(rx);
        if (ret != 0)
        {
            WIFI_WARN("RX通知Link线程失败，traffic=%u packets=%u error=%d", (unsigned int)traffic_class, pushed_count, ret);
        }
    }

    return (int)pushed_count;
}

/**
 * @brief 运行Wi-Fi Socket接收生产线程。
 */
static void *_linkg_wifi_rx_worker(void *user_data)
{
    struct epoll_event         events[LINKG_WIFI_RX_EPOLL_EVENT_COUNT];
    bool                       ready[LINKG_WIFI_TRAFFIC_COUNT];
    linkg_wifi_rx_t           *rx;
    linkg_wifi_traffic_class_t traffic_class;
    uint32_t                   event_index;
    uint32_t                   class_index;
    bool                       progressed;
    int                        event_count;
    int                        socket_error;
    int                        ret;

    rx = user_data;
    if (rx == NULL)
    {
        return NULL;
    }

    WIFI_DEBUG("RX生产线程启动");

    while (atomic_load(&rx->started))
    {
        memset(ready, 0, sizeof(ready));

        do
        {
            event_count = epoll_wait(rx->socket_epoll_fd, events, LINKG_WIFI_RX_EPOLL_EVENT_COUNT, -1);
        }
        while (event_count < 0 && errno == EINTR && atomic_load(&rx->started));

        if (!atomic_load(&rx->started))
        {
            break;
        }

        if (event_count < 0)
        {
            WIFI_WARN("RX epoll_wait失败，error=%d", -errno);
            break;
        }

        for (event_index = 0U; event_index < (uint32_t)event_count; event_index++)
        {
            if (events[event_index].data.u32 == LINKG_WIFI_RX_WORKER_WAKEUP_EVENT)
            {
                continue;
            }

            if (events[event_index].data.u32 >= LINKG_WIFI_TRAFFIC_COUNT)
            {
                WIFI_WARN("RX epoll事件索引异常，event=%u", events[event_index].data.u32);
                continue;
            }

            traffic_class = (linkg_wifi_traffic_class_t)events[event_index].data.u32;

            if ((events[event_index].events & (EPOLLERR | EPOLLHUP)) != 0U)
            {
                socket_error = _linkg_wifi_rx_socket_error(rx, traffic_class);
                WIFI_WARN("RX Socket异常，traffic=%u error=%d", (unsigned int)traffic_class, socket_error);
                continue;
            }

            if ((events[event_index].events & EPOLLIN) != 0U)
            {
                ready[traffic_class] = true;
            }
        }

        progressed = false;

        // 每轮每个ready业务最多接收一批，保持Socket排空优先级同时避免低优先级长期饥饿。
        for (class_index = LINKG_WIFI_TRAFFIC_REALTIME; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
        {
            traffic_class = (linkg_wifi_traffic_class_t)class_index;

            if (!ready[traffic_class])
            {
                continue;
            }

            ret = _linkg_wifi_rx_receive_once(rx, traffic_class);
            if (ret < 0)
            {
                WIFI_WARN("RX批量接收失败，traffic=%u error=%d", (unsigned int)traffic_class, ret);
                continue;
            }

            if (ret > 0)
            {
                progressed = true;
            }
        }

        if (!progressed)
        {
            linkg_time_sleep_ms(LINKG_WIFI_RX_RETRY_MS);
        }
    }

    WIFI_DEBUG("RX生产线程退出");

    return NULL;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi接收模块。
 *
 * packet_pool、socket_fds和service_ports仅借用，生命周期由Wi-Fi Link保证。
 * 每个业务类别创建独立SPSC Queue，生产端为Wi-Fi RX Worker，消费端为Link RX线程。
 */
linkg_wifi_rx_t *linkg_wifi_rx_create(uint32_t capacity, linkg_packet_pool_t *packet_pool, int *socket_fds, const uint16_t *service_ports)
{
    linkg_wifi_rx_t *rx;
    uint32_t         class_index;
    int              ret;

    if (capacity == 0U || capacity > LINKG_WIFI_RX_BATCH_SIZE_MAX || packet_pool == NULL || socket_fds == NULL || service_ports == NULL)
    {
        return NULL;
    }

    rx = calloc(1, sizeof(*rx));
    if (rx == NULL)
    {
        return NULL;
    }

    rx->packet_pool      = packet_pool;
    rx->socket_fds       = socket_fds;
    rx->service_ports    = service_ports;
    rx->capacity         = capacity;
    rx->queue_capacity   = capacity * LINKG_WIFI_RX_QUEUE_BATCH_COUNT;
    rx->socket_epoll_fd  = -1;
    rx->worker_wakeup_fd = -1;
    rx->notify_fd        = -1;
    rx->worker_created   = false;

    atomic_store(&rx->started, false);
    atomic_store(&rx->notify_pending, false);

    ret = pthread_mutex_init(&rx->peer_stats_lock, NULL);
    if (ret != 0)
    {
        free(rx);
        return NULL;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        rx->queues[class_index] = linkg_wifi_rx_queue_create(rx->queue_capacity);
        if (rx->queues[class_index] == NULL)
        {
            goto fail_queues;
        }
    }

    return rx;

fail_queues:
    while (class_index > 0U)
    {
        class_index--;

        linkg_wifi_rx_queue_destroy(rx->queues[class_index]);
        rx->queues[class_index] = NULL;
    }

    pthread_mutex_destroy(&rx->peer_stats_lock);
    free(rx);

    return NULL;
}

/**
 * @brief 销毁Wi-Fi接收模块。
 *
 * 调用前Link RX消费线程不能再访问rx。
 */
void linkg_wifi_rx_destroy(linkg_wifi_rx_t *rx)
{
    uint32_t class_index;

    if (rx == NULL)
    {
        return;
    }

    (void)linkg_wifi_rx_stop(rx);

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        linkg_wifi_rx_queue_destroy(rx->queues[class_index]);
        rx->queues[class_index] = NULL;
    }

    pthread_mutex_destroy(&rx->peer_stats_lock);

    free(rx);
}

/**
 * @brief 启动Wi-Fi接收模块。
 */
int linkg_wifi_rx_start(linkg_wifi_rx_t *rx)
{
    pthread_attr_t worker_attr;
    cpu_set_t      cpu_set;
    uint32_t       class_index;
    bool           attr_initialized;
    int            ret;

    if (rx == NULL)
    {
        return -EINVAL;
    }

    if (atomic_load(&rx->started) || rx->worker_created)
    {
        return -EALREADY;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        if (rx->socket_fds[class_index] < 0)
        {
            return -ENODEV;
        }
    }

    rx->socket_epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (rx->socket_epoll_fd < 0)
    {
        return -errno;
    }

    rx->worker_wakeup_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (rx->worker_wakeup_fd < 0)
    {
        ret = -errno;
        goto fail_epoll;
    }

    rx->notify_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (rx->notify_fd < 0)
    {
        ret = -errno;
        goto fail_wakeup;
    }

    ret = _linkg_wifi_rx_register_fd(rx->socket_epoll_fd, rx->worker_wakeup_fd, LINKG_WIFI_RX_WORKER_WAKEUP_EVENT);
    if (ret != 0)
    {
        goto fail_notify;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        ret = _linkg_wifi_rx_register_fd(rx->socket_epoll_fd, rx->socket_fds[class_index], class_index);
        if (ret != 0)
        {
            goto fail_notify;
        }
    }

    attr_initialized = false;

    ret = pthread_attr_init(&worker_attr);
    if (ret != 0)
    {
        ret = -ret;
        goto fail_notify;
    }

    attr_initialized = true;

    CPU_ZERO(&cpu_set);
    CPU_SET(LINKG_WIFI_RX_WORKER_CPU_CORE, &cpu_set);

    ret = pthread_attr_setaffinity_np(&worker_attr, sizeof(cpu_set), &cpu_set);
    if (ret != 0)
    {
        ret = -ret;
        goto fail_attr;
    }

    atomic_store(&rx->notify_pending, false);
    atomic_store(&rx->started, true);

    ret = pthread_create(&rx->worker, &worker_attr, _linkg_wifi_rx_worker, rx);
    if (ret != 0)
    {
        atomic_store(&rx->started, false);
        ret = -ret;
        goto fail_attr;
    }

    rx->worker_created = true;

    pthread_attr_destroy(&worker_attr);

    WIFI_DEBUG("RX生产线程绑定CPU%u", LINKG_WIFI_RX_WORKER_CPU_CORE);

    return 0;

fail_attr:
    if (attr_initialized)
    {
        pthread_attr_destroy(&worker_attr);
    }

fail_notify:
    (void)_linkg_wifi_rx_close_fd(&rx->notify_fd);

fail_wakeup:
    (void)_linkg_wifi_rx_close_fd(&rx->worker_wakeup_fd);

fail_epoll:
    (void)_linkg_wifi_rx_close_fd(&rx->socket_epoll_fd);

    return ret;
}

/**
 * @brief 停止Wi-Fi接收模块。
 *
 * 停止生产线程后清空三业务Queue，并释放其中尚未消费的Packet基础引用。
 */
int linkg_wifi_rx_stop(linkg_wifi_rx_t *rx)
{
    uint32_t class_index;
    int      first_error;
    int      ret;

    if (rx == NULL)
    {
        return -EINVAL;
    }

    first_error = 0;

    if (atomic_exchange(&rx->started, false))
    {
        ret = _linkg_wifi_rx_signal_fd(rx->worker_wakeup_fd);
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    if (rx->worker_created)
    {
        ret = pthread_join(rx->worker, NULL);
        if (ret != 0 && first_error == 0)
        {
            first_error = -ret;
        }

        rx->worker_created = false;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        linkg_wifi_rx_queue_clear(rx->queues[class_index]);
    }

    atomic_store_explicit(&rx->notify_pending, false, memory_order_release);

    ret = _linkg_wifi_rx_close_fd(&rx->notify_fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_wifi_rx_close_fd(&rx->worker_wakeup_fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_wifi_rx_close_fd(&rx->socket_epoll_fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/****************************** 数据接收 ******************************/

/**
 * @brief 获取Link RX用于等待已完成Wi-Fi Packet的通知描述符。
 */
int linkg_wifi_rx_get_fd(linkg_wifi_rx_t *rx)
{
    if (rx == NULL)
    {
        return -EINVAL;
    }

    if (rx->notify_fd < 0)
    {
        return -ENODEV;
    }

    return rx->notify_fd;
}

/**
 * @brief 按实时、视频、普通顺序批量交付已经接收完成的Wi-Fi Packet。
 *
 * 每个返回Packet的基础引用从对应Wi-Fi RX Queue转移给Link RX，
 * items为输出数组，同一批次允许混合REALTIME、VIDEO和DATA。
 */
int linkg_wifi_rx_receive_batch(linkg_wifi_rx_t *rx, linkg_link_rx_item_t *items, uint32_t capacity)
{
    linkg_wifi_rx_queue_t *queue;
    uint32_t               received_count;
    uint32_t               remaining;
    uint32_t               pop_count;
    uint32_t               class_index;
    uint32_t               index;
    int                    ret;

    if (rx == NULL || items == NULL)
    {
        return -EINVAL;
    }

    if (capacity == 0U)
    {
        return 0;
    }

    if (capacity > rx->capacity)
    {
        return -EOVERFLOW;
    }

    if (rx->notify_fd < 0)
    {
        return -ENODEV;
    }

retry:
    received_count = 0U;
    remaining      = capacity;

    for (class_index = LINKG_WIFI_TRAFFIC_REALTIME; class_index < LINKG_WIFI_TRAFFIC_COUNT && remaining > 0U; class_index++)
    {
        queue = rx->queues[class_index];

        pop_count = linkg_wifi_rx_queue_pop_batch(queue, rx->consume_items, remaining);
        if (pop_count == 0U)
        {
            continue;
        }

        for (index = 0U; index < pop_count; index++)
        {
            items[received_count + index].packet = rx->consume_items[index].packet;
            items[received_count + index].source = rx->consume_items[index].source;

            rx->consume_items[index].packet = NULL;
        }

        received_count += pop_count;
        remaining      -= pop_count;
    }

    if (received_count > 0U)
    {
        return (int)received_count;
    }

    /**
     * 当前三个Queue已经暂时消费为空。
     * 先清除本轮eventfd累计通知，再撤销notify_pending。
     * Producer若在撤销前入队不会重复通知，下面重新检查Queue并继续消费；
     * Producer若在撤销后入队则会重新写notify_fd，不会丢失下一轮唤醒。
     */
    ret = _linkg_wifi_rx_clear_notify(rx);
    if (ret != 0)
    {
        return ret;
    }

    atomic_store_explicit(&rx->notify_pending, false, memory_order_release);

    if (!_linkg_wifi_rx_queues_empty(rx))
    {
        atomic_store_explicit(&rx->notify_pending, true, memory_order_release);
        goto retry;
    }

    return 0;
}

/****************************** 链路统计 ******************************/

/**
 * @brief 获取指定对端节点的Wi-Fi链路接收累计统计。
 *
 * @note peer_node_id必须为1~254。当前Wi-Fi RX生命周期内尚未收到该节点任何
 *       Wi-Fi Wire Packet时返回-ENOENT。
 */
int linkg_wifi_rx_get_peer_stats(linkg_wifi_rx_t *rx, uint8_t peer_node_id, linkg_wifi_rx_stats_t *stats)
{
    linkg_wifi_rx_peer_stats_t *peer_stats;
    int                         ret;

    if (rx == NULL || stats == NULL)
    {
        return -EINVAL;
    }

    if (peer_node_id == 0U || peer_node_id == UINT8_MAX)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&rx->peer_stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    peer_stats = &rx->peer_stats[peer_node_id];

    if (!peer_stats->initialized)
    {
        pthread_mutex_unlock(&rx->peer_stats_lock);
        return -ENOENT;
    }

    stats->received_packets       = peer_stats->received_packets;
    stats->confirmed_lost_packets = peer_stats->confirmed_lost_packets;

    ret = pthread_mutex_unlock(&rx->peer_stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    return 0;
}
