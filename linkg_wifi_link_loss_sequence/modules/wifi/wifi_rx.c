/**
 * @file wifi_rx.c
 * @brief LinkG Wi-Fi接收模块实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-11
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

#define LINKG_WIFI_RX_IPV4_HEADER_SIZE       20U                                                                                  // IPv4最小头部长度
#define LINKG_WIFI_RX_UDP_HEADER_SIZE        8U                                                                                   // UDP头部长度
#define LINKG_WIFI_RX_MTU                    1500U                                                                                // Wi-Fi接口MTU
#define LINKG_WIFI_RX_UDP_PAYLOAD_MAX        (LINKG_WIFI_RX_MTU - LINKG_WIFI_RX_IPV4_HEADER_SIZE - LINKG_WIFI_RX_UDP_HEADER_SIZE) // 单个UDP报文最大负载
#define LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX  (LINKG_WIFI_RX_UDP_PAYLOAD_MAX - LINKG_WIFI_WIRE_HEADER_SIZE)                        // Wi-Fi私有头之后最大Transport报文长度
#define LINKG_WIFI_RX_BATCH_SIZE_MAX         32U                                                                                  // 单次Socket接收和上层交付最大Packet数量
#define LINKG_WIFI_RX_QUEUE_BATCH_COUNT      5U                                                                                   // 单业务接收队列最多缓存批次数
#define LINKG_WIFI_RX_EPOLL_EVENT_COUNT      (LINKG_WIFI_TRAFFIC_COUNT + 1U)                                                       // 三业务Socket和Worker唤醒描述符数量
#define LINKG_WIFI_RX_WORKER_WAKEUP_EVENT    LINKG_WIFI_TRAFFIC_COUNT                                                             // Worker唤醒事件索引
#define LINKG_WIFI_RX_WORKER_CPU_CORE        1U                                                                                   // RX生产线程绑定CPU1
#define LINKG_WIFI_RX_RETRY_MS               1U                                                                                   // Queue或Packet Pool暂不可用时重试间隔
#define LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE   64U                                                                                  // Wi-Fi链路乱序确认窗口
#define LINKG_WIFI_RX_PEER_SLOT_COUNT        256U                                                                                 // 进程生命周期最多维护的Wi-Fi对端统计状态
#define LINKG_WIFI_RX_PEER_SLOT_MASK         (LINKG_WIFI_RX_PEER_SLOT_COUNT - 1U)                                                // 对端统计表掩码

_Static_assert((LINKG_WIFI_RX_PEER_SLOT_COUNT & (LINKG_WIFI_RX_PEER_SLOT_COUNT - 1U)) == 0U, "Wi-Fi RX peer slot count must be power of two");
_Static_assert(LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE == 64U, "Wi-Fi RX sequence window must match uint64 bitmap");

/****************************** 内部类型 ******************************/

/**
 * @brief Wi-Fi接收生产线程预分配Scratch。
 */
typedef struct
{
    linkg_wifi_rx_queue_item_t items[LINKG_WIFI_RX_BATCH_SIZE_MAX];      // 当前接收完成Queue元素
    linkg_packet_t            *packets[LINKG_WIFI_RX_BATCH_SIZE_MAX];    // 当前从Packet Pool申请的Packet
    linkg_wifi_wire_header_t   headers[LINKG_WIFI_RX_BATCH_SIZE_MAX];    // 当前接收Wi-Fi私有头
    struct mmsghdr             messages[LINKG_WIFI_RX_BATCH_SIZE_MAX];   // recvmmsg消息数组
    struct iovec               iovecs[LINKG_WIFI_RX_BATCH_SIZE_MAX][2];  // Wi-Fi头+Transport Packet双iovec
} linkg_wifi_rx_scratch_t;

/**
 * @brief 单个Wi-Fi对端的64包接收窗口和累计统计。
 *
 * valid一旦发布后address不再修改；窗口仅由RX Worker单线程更新，公开累计值使用Atomic读取。
 */
typedef struct
{
    struct in_addr   address;                         // 对端Wi-Fi IPv4
    uint32_t         highest_sequence;                // 当前窗口最高Sequence
    uint32_t         window_span;                     // 当前窗口真实有效历史长度，1~64
    uint64_t         received_bitmap;                 // bit0对应highest，bit63对应highest-63
    _Atomic uint64_t received_packets;                // 累计首次成功接收唯一Packet
    _Atomic uint64_t confirmed_lost_packets;          // 累计窗口推出后确认丢失Packet
    bool             initialized;                     // Sequence窗口是否已经收到首包
    _Atomic bool     valid;                           // 当前统计槽位是否有效
} linkg_wifi_rx_peer_stats_t;

/**
 * @brief Wi-Fi接收模块运行上下文。
 */
struct linkg_wifi_rx
{
    pthread_t                  worker;                                      // Socket接收生产线程
    linkg_wifi_rx_scratch_t    scratch;                                     // 生产线程预分配Scratch
    linkg_wifi_rx_queue_item_t consume_items[LINKG_WIFI_RX_BATCH_SIZE_MAX]; // Link RX消费临时元素
    linkg_wifi_rx_queue_t     *queues[LINKG_WIFI_TRAFFIC_COUNT];            // 三业务SPSC接收FIFO
    linkg_wifi_rx_peer_stats_t peer_stats[LINKG_WIFI_RX_PEER_SLOT_COUNT];   // 每个对端独立Wi-Fi链路接收统计
    linkg_packet_pool_t       *packet_pool;                                 // 借用Link Packet Pool
    int                       *socket_fds;                                  // 借用Wi-Fi Link业务Socket数组
    const uint16_t            *service_ports;                               // 借用Wi-Fi Link业务端口数组

    uint32_t                   capacity;                                    // 单次接收和交付最大Packet数量
    uint32_t                   queue_capacity;                              // 单业务接收队列最大Packet数量
    int                        socket_epoll_fd;                             // Worker监听三业务Socket的epoll描述符
    int                        worker_wakeup_fd;                            // 停止时唤醒Worker的eventfd
    int                        notify_fd;                                   // 通知Link RX存在已完成Packet的eventfd
    bool                       worker_created;                              // Worker线程是否已经创建
    _Atomic bool               started;                                     // 接收模块运行状态
    _Atomic bool               notify_pending;                              // Link RX通知周期是否已经激活
};

/****************************** Sequence统计 ******************************/

/**
 * @brief 统计64位整数中的1数量。
 */
static uint32_t _linkg_wifi_rx_popcount64(uint64_t value)
{
    uint32_t count;

    count = 0U;
    while (value != 0U)
    {
        value &= value - 1U;
        count++;
    }

    return count;
}

/**
 * @brief 构造低count位为1的64位Mask。
 */
static uint64_t _linkg_wifi_rx_low_mask(uint32_t count)
{
    if (count == 0U)
    {
        return 0U;
    }

    if (count >= 64U)
    {
        return UINT64_MAX;
    }

    return (UINT64_C(1) << count) - 1U;
}

/**
 * @brief 计算对端IPv4对应统计槽位起点。
 */
static uint32_t _linkg_wifi_rx_peer_hash(const struct in_addr *address)
{
    uint32_t value;

    value = ntohl(address->s_addr);
    value ^= value >> 16U;
    value *= 0x7FEB352DU;
    value ^= value >> 15U;

    return value & LINKG_WIFI_RX_PEER_SLOT_MASK;
}

/**
 * @brief 获取或创建指定对端统计状态。
 *
 * @note 仅由RX Worker单线程调用，因此槽位创建不需要额外写锁；valid使用release发布不可变address。
 */
static linkg_wifi_rx_peer_stats_t *_linkg_wifi_rx_peer_get_or_create(linkg_wifi_rx_t *rx, const struct in_addr *address)
{
    linkg_wifi_rx_peer_stats_t *state;
    uint32_t                    slot;
    uint32_t                    probe;

    slot = _linkg_wifi_rx_peer_hash(address);

    for (probe = 0U; probe < LINKG_WIFI_RX_PEER_SLOT_COUNT; probe++)
    {
        state = &rx->peer_stats[slot];

        if (atomic_load_explicit(&state->valid, memory_order_acquire))
        {
            if (state->address.s_addr == address->s_addr)
            {
                return state;
            }
        }
        else
        {
            state->address = *address;
            state->highest_sequence = 0U;
            state->window_span = 0U;
            state->received_bitmap = 0U;
            state->initialized = false;
            atomic_store_explicit(&state->received_packets, 0U, memory_order_relaxed);
            atomic_store_explicit(&state->confirmed_lost_packets, 0U, memory_order_relaxed);
            atomic_store_explicit(&state->valid, true, memory_order_release);
            return state;
        }

        slot = (slot + 1U) & LINKG_WIFI_RX_PEER_SLOT_MASK;
    }

    return NULL;
}

/**
 * @brief 将当前有效窗口内尚未收到的Packet数量计为确认丢包。
 */
static uint64_t _linkg_wifi_rx_window_missing(const linkg_wifi_rx_peer_stats_t *state)
{
    uint64_t valid_mask;
    uint32_t received;

    if (state->window_span == 0U)
    {
        return 0U;
    }

    valid_mask = _linkg_wifi_rx_low_mask(state->window_span);
    received   = _linkg_wifi_rx_popcount64(state->received_bitmap & valid_mask);

    return (uint64_t)state->window_span - (uint64_t)received;
}

/**
 * @brief 更新单个对端64包Wi-Fi Sequence窗口。
 *
 * 新Sequence只在首次出现时增加received_packets；窗口内乱序晚到只补Bitmap，不增加Lost。
 * 只有缺口被窗口真正推出后才增加confirmed_lost_packets。
 */
static void _linkg_wifi_rx_peer_update(linkg_wifi_rx_peer_stats_t *state, uint32_t sequence)
{
    uint64_t finalize_mask;
    uint64_t lost_count;
    uint32_t finalize_count;
    uint32_t finalize_start;
    uint32_t combined_span;
    uint32_t advance;
    uint32_t offset;
    int32_t  difference;

    if (!state->initialized)
    {
        state->highest_sequence = sequence;
        state->window_span      = 1U;
        state->received_bitmap  = 1U;
        state->initialized      = true;
        atomic_fetch_add_explicit(&state->received_packets, 1U, memory_order_relaxed);
        return;
    }

    difference = (int32_t)(sequence - state->highest_sequence);
    if (difference > 0)
    {
        advance    = (uint32_t)difference;
        lost_count = 0U;

        if (advance >= LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE)
        {
            /**
             * 新Sequence至少越过完整64窗口：旧窗口全部完成确认；
             * advance-64表示旧highest与新窗口起点之间已经完全落在窗口外的新增缺口。
             */
            lost_count += _linkg_wifi_rx_window_missing(state);
            lost_count += (uint64_t)advance - LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE;

            state->received_bitmap = 1U;
            state->window_span     = LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE;
        }
        else
        {
            combined_span = state->window_span + advance;
            finalize_count = combined_span > LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE
                ? combined_span - LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE
                : 0U;

            if (finalize_count > 0U)
            {
                finalize_start = state->window_span - finalize_count;
                finalize_mask  = _linkg_wifi_rx_low_mask(finalize_count) << finalize_start;
                lost_count    += (uint64_t)finalize_count -
                                 (uint64_t)_linkg_wifi_rx_popcount64(state->received_bitmap & finalize_mask);
            }

            state->received_bitmap <<= advance;
            state->received_bitmap  |= 1U;
            state->window_span = combined_span > LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE
                ? LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE
                : combined_span;
        }

        state->highest_sequence = sequence;
        atomic_fetch_add_explicit(&state->received_packets, 1U, memory_order_relaxed);

        if (lost_count > 0U)
        {
            atomic_fetch_add_explicit(&state->confirmed_lost_packets, lost_count, memory_order_relaxed);
        }

        return;
    }

    if (difference == 0)
    {
        return; // 当前highest重复Packet。
    }

    offset = state->highest_sequence - sequence;
    if (offset >= LINKG_WIFI_RX_SEQUENCE_WINDOW_SIZE || offset >= state->window_span)
    {
        return; // 已经确认窗口之外的超迟Packet不再修改公开累计统计。
    }

    if ((state->received_bitmap & (UINT64_C(1) << offset)) != 0U)
    {
        return; // 窗口内重复Packet。
    }

    state->received_bitmap |= UINT64_C(1) << offset;
    atomic_fetch_add_explicit(&state->received_packets, 1U, memory_order_relaxed);
}

/****************************** 内部辅助 ******************************/

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

/****************************** Socket接收 ******************************/

/**
 * @brief 对指定业务Socket执行一次批量接收并将有效Packet加入对应Queue。
 *
 * Wi-Fi私有4B Header直接接收到Scratch，Transport Packet直接进入Packet Pool槽位，
 * 不执行memmove。链路Sequence在规范化Peer Endpoint之前按源IPv4更新64包统计窗口。
 */
static int _linkg_wifi_rx_receive_once(linkg_wifi_rx_t *rx, linkg_wifi_traffic_class_t traffic_class)
{
    linkg_wifi_rx_peer_stats_t *peer_stats;
    linkg_wifi_rx_scratch_t    *scratch;
    linkg_wifi_rx_queue_t      *queue;
    struct sockaddr_in         *source;
    linkg_packet_t             *packet;
    uint32_t                    transport_length;
    uint32_t                    packet_capacity;
    uint32_t                    request_count;
    uint32_t                    allocated_count;
    uint32_t                    valid_count;
    uint32_t                    pushed_count;
    uint32_t                    available;
    uint32_t                    sequence;
    uint32_t                    index;
    int                         socket_fd;
    int                         ret;

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

        packet->data_length = 0U;
        linkg_packet_set_data(packet);

        scratch->items[index].packet = packet;
        memset(&scratch->items[index].source, 0, sizeof(scratch->items[index].source));
        memset(&scratch->headers[index], 0, sizeof(scratch->headers[index]));

        scratch->iovecs[index][0].iov_base = &scratch->headers[index];
        scratch->iovecs[index][0].iov_len  = sizeof(scratch->headers[index]);
        scratch->iovecs[index][1].iov_base = linkg_packet_data(packet);
        scratch->iovecs[index][1].iov_len  = packet_capacity;

        if (scratch->iovecs[index][1].iov_len > LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX)
        {
            scratch->iovecs[index][1].iov_len = LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX;
        }

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

    valid_count = 0U;

    for (index = 0U; index < (uint32_t)ret; index++)
    {
        packet          = scratch->packets[index];
        packet_capacity = linkg_packet_capacity(packet);
        scratch->items[index].source.length = scratch->messages[index].msg_hdr.msg_namelen;

        if ((scratch->messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0U ||
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
        if (transport_length == 0U || transport_length > packet_capacity || transport_length > LINKG_WIFI_RX_TRANSPORT_PAYLOAD_MAX)
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

        peer_stats = _linkg_wifi_rx_peer_get_or_create(rx, &source->sin_addr);
        if (peer_stats == NULL)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            WIFI_WARN("RX Wi-Fi对端统计槽位耗尽，source=%s", inet_ntoa(source->sin_addr));
            continue;
        }

        sequence = ntohl(scratch->headers[index].sequence);
        _linkg_wifi_rx_peer_update(peer_stats, sequence);

        // 统一使用DATA端口表示同一Wi-Fi Peer的规范化Endpoint。
        source->sin_port     = htons(rx->service_ports[LINKG_WIFI_TRAFFIC_DATA]);
        packet->data_length  = transport_length;
        _linkg_wifi_rx_set_received_class(packet, traffic_class);

        if (valid_count != index)
        {
            scratch->items[valid_count] = scratch->items[index];
        }

        scratch->packets[index] = NULL;
        valid_count++;
    }

    for (index = (uint32_t)ret; index < allocated_count; index++)
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

/****************************** Worker ******************************/

/**
 * @brief 运行Wi-Fi Socket接收生产线程。
 */
static void *_linkg_wifi_rx_worker(void *user_data)
{
    struct epoll_event          events[LINKG_WIFI_RX_EPOLL_EVENT_COUNT];
    bool                        ready[LINKG_WIFI_TRAFFIC_COUNT];
    linkg_wifi_rx_t            *rx;
    linkg_wifi_traffic_class_t  traffic_class;
    uint32_t                    event_index;
    uint32_t                    class_index;
    bool                        progressed;
    eventfd_t                   wakeup_value;
    int                         event_count;
    int                         socket_error;
    int                         ret;

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
            WIFI_WARN("RX epoll_wait失败，error=%d", errno);
            linkg_time_sleep_ms(LINKG_WIFI_RX_RETRY_MS);
            continue;
        }

        for (event_index = 0U; event_index < (uint32_t)event_count; event_index++)
        {
            if (events[event_index].data.u32 == LINKG_WIFI_RX_WORKER_WAKEUP_EVENT)
            {
                do
                {
                    ret = eventfd_read(rx->worker_wakeup_fd, &wakeup_value);
                }
                while (ret != 0 && errno == EINTR);

                continue;
            }

            if (events[event_index].data.u32 >= LINKG_WIFI_TRAFFIC_COUNT)
            {
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

    free(rx);
    return NULL;
}

/**
 * @brief 销毁Wi-Fi接收模块。
 *
 * @note 调用前Link RX消费线程不能再访问rx。
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
 * 对端Sequence窗口和累计统计保留到RX对象销毁，保证同进程内Wi-Fi重启不丢失累计基线。
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
 * @brief 获取指定对端Wi-Fi链路累计接收和确认丢包统计。
 */
int linkg_wifi_rx_get_peer_stats(const linkg_wifi_rx_t *rx, const struct in_addr *peer_address, uint64_t *received_packets, uint64_t *confirmed_lost_packets)
{
    const linkg_wifi_rx_peer_stats_t *state;
    uint32_t                          slot;
    uint32_t                          probe;

    if (rx == NULL || peer_address == NULL || received_packets == NULL || confirmed_lost_packets == NULL)
    {
        return -EINVAL;
    }

    *received_packets       = 0U;
    *confirmed_lost_packets = 0U;

    slot = _linkg_wifi_rx_peer_hash(peer_address);

    for (probe = 0U; probe < LINKG_WIFI_RX_PEER_SLOT_COUNT; probe++)
    {
        state = &rx->peer_stats[slot];

        if (!atomic_load_explicit(&state->valid, memory_order_acquire))
        {
            return -ENOENT;
        }

        if (state->address.s_addr == peer_address->s_addr)
        {
            *received_packets = atomic_load_explicit(&state->received_packets, memory_order_relaxed);
            *confirmed_lost_packets = atomic_load_explicit(&state->confirmed_lost_packets, memory_order_relaxed);
            return 0;
        }

        slot = (slot + 1U) & LINKG_WIFI_RX_PEER_SLOT_MASK;
    }

    return -ENOENT;
}
