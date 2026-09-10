/**
 * @file cellular_rx.c
 * @brief LinkG蜂窝链路接收模块实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-10
 */

#define _GNU_SOURCE

#include "cellular_rx.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
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

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_time.h"

#include "cellular_link_heartbeat.h"
#include "cellular_rx_queue.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_RX_IPV6_HEADER_SIZE      40U                                                                                       // IPv6固定头部长度
#define LINKG_CELLULAR_RX_UDP_HEADER_SIZE       8U                                                                                        // UDP头部长度
#define LINKG_CELLULAR_RX_MTU                   1500U                                                                                     // 蜂窝接口MTU
#define LINKG_CELLULAR_RX_UDP_PAYLOAD_MAX       (LINKG_CELLULAR_RX_MTU - LINKG_CELLULAR_RX_IPV6_HEADER_SIZE - LINKG_CELLULAR_RX_UDP_HEADER_SIZE) // 单个UDP报文最大负载
#define LINKG_CELLULAR_RX_CONTROL_SIZE          CMSG_SPACE(sizeof(struct in6_pktinfo))                                                     // 单包IPv6辅助控制区大小
#define LINKG_CELLULAR_RX_BATCH_SIZE_MAX        32U                                                                                       // 单次Socket接收和上层交付最大Packet数量
#define LINKG_CELLULAR_RX_QUEUE_BATCH_COUNT     5U                                                                                        // 单业务接收队列最多缓存批次数
#define LINKG_CELLULAR_RX_EPOLL_EVENT_COUNT     (LINKG_LINK_TX_CLASS_COUNT + 1U)                                                           // 三业务Socket和Worker唤醒描述符数量
#define LINKG_CELLULAR_RX_WORKER_WAKEUP_EVENT   LINKG_LINK_TX_CLASS_COUNT                                                                 // Worker唤醒事件索引
#define LINKG_CELLULAR_RX_WORKER_CPU_CORE       0                                                                                         // RX生产线程绑定CPU0
#define LINKG_CELLULAR_RX_RETRY_MS              1U                                                                                        // Queue或Packet Pool暂不可用时重试间隔

/****************************** 内部类型 ******************************/

/**
 * @brief 蜂窝接收生产线程预分配Scratch。
 */
typedef struct
{
    linkg_cellular_rx_queue_item_t items[LINKG_CELLULAR_RX_BATCH_SIZE_MAX];                               // 当前接收完成Queue元素
    linkg_packet_t                *packets[LINKG_CELLULAR_RX_BATCH_SIZE_MAX];                             // 当前从Packet Pool申请的Packet
    struct mmsghdr                 messages[LINKG_CELLULAR_RX_BATCH_SIZE_MAX];                            // recvmmsg消息数组
    struct iovec                   iovecs[LINKG_CELLULAR_RX_BATCH_SIZE_MAX];                              // recvmmsg缓冲区数组
    unsigned char                  controls[LINKG_CELLULAR_RX_BATCH_SIZE_MAX][LINKG_CELLULAR_RX_CONTROL_SIZE]; // IPv6辅助控制区
} linkg_cellular_rx_scratch_t;

/**
 * @brief 蜂窝接收模块运行上下文。
 */
struct linkg_cellular_rx
{
    pthread_t                      worker;                                         // Socket接收生产线程
    linkg_cellular_rx_scratch_t    scratch;                                        // 生产线程预分配Scratch
    linkg_cellular_rx_queue_item_t consume_items[LINKG_CELLULAR_RX_BATCH_SIZE_MAX]; // Link RX消费临时元素
    linkg_cellular_rx_queue_t     *queues[LINKG_LINK_TX_CLASS_COUNT];               // 三业务SPSC接收FIFO

    linkg_packet_pool_t           *packet_pool;                                    // 借用Link Packet Pool
    int                           *socket_fds;                                     // 借用Cellular Link业务Socket数组
    const uint16_t                *service_ports;                                  // 借用Cellular Link业务端口数组
    const char                    *interface_name;                                 // 借用蜂窝出口接口名称

    uint32_t                       capacity;                                       // 单次接收和交付最大Packet数量
    uint32_t                       queue_capacity;                                 // 单业务接收队列最大Packet数量

    int                            socket_epoll_fd;                                // Worker监听三业务Socket的epoll描述符
    int                            worker_wakeup_fd;                               // 停止时唤醒Worker的eventfd
    int                            notify_fd;                                      // 通知Link RX存在已完成Packet的eventfd

    bool                           worker_created;                                 // Worker线程是否已经创建
    _Atomic bool                   started;                                        // 接收模块运行状态
    _Atomic bool                   notify_pending;                                 // Link RX通知周期是否已经激活
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验蜂窝业务类别。
 */
static bool _linkg_cellular_rx_class_valid(linkg_link_tx_class_t tx_class)
{
    return (int)tx_class >= 0 && tx_class < LINKG_LINK_TX_CLASS_COUNT;
}

/**
 * @brief 关闭并清空单个描述符。
 */
static int _linkg_cellular_rx_close_fd(int *descriptor)
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
static int _linkg_cellular_rx_register_fd(int epoll_fd, int descriptor, uint32_t event_index)
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
static int _linkg_cellular_rx_signal_fd(int descriptor)
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
static int _linkg_cellular_rx_clear_notify(linkg_cellular_rx_t *rx)
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
 * @brief 判断三个蜂窝接收Queue是否全部为空。
 */
static bool _linkg_cellular_rx_queues_empty(const linkg_cellular_rx_t *rx)
{
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        if (linkg_cellular_rx_queue_count(rx->queues[class_index]) > 0U)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 在当前通知周期首次入队时唤醒Link RX。
 */
static int _linkg_cellular_rx_notify_link(linkg_cellular_rx_t *rx)
{
    int ret;

    if (atomic_exchange_explicit(&rx->notify_pending, true, memory_order_acq_rel))
    {
        return 0;
    }

    ret = _linkg_cellular_rx_signal_fd(rx->notify_fd);
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
static void _linkg_cellular_rx_set_received_class(linkg_packet_t *packet, linkg_link_tx_class_t tx_class)
{
    linkg_packet_set_data(packet);

    if (tx_class == LINKG_LINK_TX_CLASS_REALTIME)
    {
        linkg_packet_set_realtime(packet, true);
    }
    else if (tx_class == LINKG_LINK_TX_CLASS_VIDEO)
    {
        linkg_packet_set_video(packet, true);
    }
}

/**
 * @brief 获取指定业务Socket当前待处理错误。
 */
static int _linkg_cellular_rx_socket_error(const linkg_cellular_rx_t *rx, linkg_link_tx_class_t tx_class)
{
    socklen_t error_length;
    int       socket_error;
    int       ret;

    socket_error = 0;
    error_length = sizeof(socket_error);

    ret = getsockopt(rx->socket_fds[tx_class], SOL_SOCKET, SO_ERROR, &socket_error, &error_length);
    if (ret != 0)
    {
        return -errno;
    }

    return socket_error == 0 ? -EIO : -socket_error;
}

/**
 * @brief 获取当前蜂窝出口接口索引。
 */
static unsigned int _linkg_cellular_rx_interface_index(const linkg_cellular_rx_t *rx)
{
    if (rx == NULL || rx->interface_name == NULL)
    {
        return 0U;
    }

    return if_nametoindex(rx->interface_name);
}

/**
 * @brief 从IPv6辅助控制信息中获取接收接口索引。
 */
static unsigned int _linkg_cellular_rx_ifindex(const struct msghdr *header)
{
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;

    if (header == NULL)
    {
        return 0U;
    }

    for (cmsg = CMSG_FIRSTHDR((struct msghdr *)header); cmsg != NULL; cmsg = CMSG_NXTHDR((struct msghdr *)header, cmsg))
    {
        if (cmsg->cmsg_level != IPPROTO_IPV6 ||
            cmsg->cmsg_type != IPV6_PKTINFO ||
            cmsg->cmsg_len < CMSG_LEN(sizeof(*pktinfo)))
        {
            continue;
        }

        pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);

        return pktinfo->ipi6_ifindex;
    }

    return 0U;
}

/**
 * @brief 释放当前Scratch中尚未转移所有权的Packet。
 */
static void _linkg_cellular_rx_release_packets(linkg_cellular_rx_scratch_t *scratch, uint32_t count)
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
 * Packet从Pool获得基础引用。成功入队后基础引用转移给Queue，
 * 无效报文、心跳报文、未使用Packet和异常未入队Packet由本函数释放。
 */
static int _linkg_cellular_rx_receive_once(linkg_cellular_rx_t *rx, linkg_link_tx_class_t tx_class)
{
    linkg_cellular_rx_scratch_t *scratch;
    linkg_cellular_rx_queue_t   *queue;
    struct sockaddr_in6         *source;
    linkg_packet_t              *packet;
    unsigned int                 expected_ifindex;
    unsigned int                 received_ifindex;
    uint32_t                     packet_capacity;
    uint32_t                     request_count;
    uint32_t                     allocated_count;
    uint32_t                     valid_count;
    uint32_t                     pushed_count;
    uint32_t                     available;
    uint32_t                     index;
    int                          socket_fd;
    int                          ret;

    if (rx == NULL || !_linkg_cellular_rx_class_valid(tx_class))
    {
        return -EINVAL;
    }

    queue     = rx->queues[tx_class];
    socket_fd = rx->socket_fds[tx_class];
    scratch   = &rx->scratch;

    available = linkg_cellular_rx_queue_available(queue);
    if (available == 0U)
    {
        return 0;
    }

    expected_ifindex = _linkg_cellular_rx_interface_index(rx);
    if (expected_ifindex == 0U)
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
    memset(scratch->controls, 0, allocated_count * sizeof(scratch->controls[0]));

    for (index = 0U; index < allocated_count; index++)
    {
        packet          = scratch->packets[index];
        packet_capacity = linkg_packet_capacity(packet);

        if (packet_capacity == 0U)
        {
            _linkg_cellular_rx_release_packets(scratch, allocated_count);
            return -ENOBUFS;
        }

        packet->data_length = 0U;
        linkg_packet_set_data(packet);

        scratch->items[index].packet = packet;
        memset(&scratch->items[index].source, 0, sizeof(scratch->items[index].source));

        scratch->iovecs[index].iov_base = linkg_packet_data(packet);
        scratch->iovecs[index].iov_len  = packet_capacity;

        if (scratch->iovecs[index].iov_len > LINKG_CELLULAR_RX_UDP_PAYLOAD_MAX)
        {
            scratch->iovecs[index].iov_len = LINKG_CELLULAR_RX_UDP_PAYLOAD_MAX;
        }

        scratch->messages[index].msg_hdr.msg_name       = &scratch->items[index].source.address;
        scratch->messages[index].msg_hdr.msg_namelen    = sizeof(scratch->items[index].source.address);
        scratch->messages[index].msg_hdr.msg_iov        = &scratch->iovecs[index];
        scratch->messages[index].msg_hdr.msg_iovlen     = 1U;
        scratch->messages[index].msg_hdr.msg_control    = scratch->controls[index];
        scratch->messages[index].msg_hdr.msg_controllen = sizeof(scratch->controls[index]);
    }

    do
    {
        ret = recvmmsg(socket_fd, scratch->messages, allocated_count, MSG_DONTWAIT, NULL);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        ret = -errno;

        _linkg_cellular_rx_release_packets(scratch, allocated_count);

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

        if ((scratch->messages[index].msg_hdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
            scratch->messages[index].msg_len == 0U ||
            scratch->messages[index].msg_len > packet_capacity ||
            scratch->messages[index].msg_len > LINKG_CELLULAR_RX_UDP_PAYLOAD_MAX ||
            scratch->items[index].source.length != sizeof(struct sockaddr_in6) ||
            scratch->items[index].source.address.ss_family != AF_INET6)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        received_ifindex = _linkg_cellular_rx_ifindex(&scratch->messages[index].msg_hdr);
        if (received_ifindex == 0U || received_ifindex != expected_ifindex)
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        source = (struct sockaddr_in6 *)&scratch->items[index].source.address;

        if (!linkg_network_ipv6_address_is_global(&source->sin6_addr) ||
            source->sin6_port != htons(rx->service_ports[tx_class]))
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        if (linkg_cellular_link_heartbeat_is_packet(linkg_packet_const_data(packet), scratch->messages[index].msg_len, tx_class))
        {
            linkg_packet_release(packet);
            scratch->packets[index] = NULL;
            continue;
        }

        // 统一使用DATA端口表示同一蜂窝Peer的规范化Endpoint。
        source->sin6_port = htons(rx->service_ports[LINKG_LINK_TX_CLASS_DATA]);

        packet->data_length = scratch->messages[index].msg_len;
        _linkg_cellular_rx_set_received_class(packet, tx_class);

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

    pushed_count = linkg_cellular_rx_queue_push_batch(queue, scratch->items, valid_count);

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
        ret = _linkg_cellular_rx_notify_link(rx);
        if (ret != 0)
        {
            LINKG_LOG_WARN("Cellular RX notify Link failed, class=%u, packets=%u, error=%d", (unsigned int)tx_class, pushed_count, ret);
        }
    }

    return (int)pushed_count;
}

/**
 * @brief 运行蜂窝Socket接收生产线程。
 */
static void *_linkg_cellular_rx_worker(void *user_data)
{
    static const linkg_link_tx_class_t priority_order[LINKG_LINK_TX_CLASS_COUNT] =
    {
        LINKG_LINK_TX_CLASS_REALTIME,
        LINKG_LINK_TX_CLASS_VIDEO,
        LINKG_LINK_TX_CLASS_DATA
    };

    struct epoll_event    events[LINKG_CELLULAR_RX_EPOLL_EVENT_COUNT];
    bool                  ready[LINKG_LINK_TX_CLASS_COUNT];
    linkg_cellular_rx_t  *rx;
    linkg_link_tx_class_t tx_class;
    uint32_t              event_index;
    uint32_t              order_index;
    bool                  progressed;
    int                   event_count;
    int                   socket_error;
    int                   ret;

    rx = user_data;
    if (rx == NULL)
    {
        return NULL;
    }

    while (atomic_load(&rx->started))
    {
        memset(ready, 0, sizeof(ready));

        do
        {
            event_count = epoll_wait(rx->socket_epoll_fd, events, LINKG_CELLULAR_RX_EPOLL_EVENT_COUNT, -1);
        }
        while (event_count < 0 && errno == EINTR && atomic_load(&rx->started));

        if (!atomic_load(&rx->started))
        {
            break;
        }

        if (event_count < 0)
        {
            LINKG_LOG_WARN("Cellular RX epoll_wait failed, error=%d", -errno);
            break;
        }

        for (event_index = 0U; event_index < (uint32_t)event_count; event_index++)
        {
            if (events[event_index].data.u32 == LINKG_CELLULAR_RX_WORKER_WAKEUP_EVENT)
            {
                continue;
            }

            if (events[event_index].data.u32 >= LINKG_LINK_TX_CLASS_COUNT)
            {
                LINKG_LOG_WARN("Cellular RX invalid epoll event=%u", events[event_index].data.u32);
                continue;
            }

            tx_class = (linkg_link_tx_class_t)events[event_index].data.u32;

            if ((events[event_index].events & (EPOLLERR | EPOLLHUP)) != 0U)
            {
                socket_error = _linkg_cellular_rx_socket_error(rx, tx_class);
                LINKG_LOG_WARN("Cellular RX socket error, class=%u, error=%d", (unsigned int)tx_class, socket_error);
                continue;
            }

            if ((events[event_index].events & EPOLLIN) != 0U)
            {
                ready[tx_class] = true;
            }
        }

        progressed = false;

        // 每轮每个ready业务最多接收一批，优先实时同时避免低优先级Socket长期饥饿。
        for (order_index = 0U; order_index < LINKG_LINK_TX_CLASS_COUNT; order_index++)
        {
            tx_class = priority_order[order_index];

            if (!ready[tx_class])
            {
                continue;
            }

            ret = _linkg_cellular_rx_receive_once(rx, tx_class);
            if (ret < 0)
            {
                LINKG_LOG_WARN("Cellular RX batch receive failed, class=%u, error=%d", (unsigned int)tx_class, ret);
                continue;
            }

            if (ret > 0)
            {
                progressed = true;
            }
        }

        if (!progressed)
        {
            linkg_time_sleep_ms(LINKG_CELLULAR_RX_RETRY_MS);
        }
    }

    return NULL;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝链路接收模块。
 *
 * packet_pool、socket_fds、service_ports和interface_name仅借用，生命周期由Cellular Link保证。
 * 每个业务类别创建独立SPSC Queue，生产端为Cellular RX Worker，消费端为Link RX线程。
 */
linkg_cellular_rx_t *linkg_cellular_rx_create(uint32_t capacity, linkg_packet_pool_t *packet_pool, int *socket_fds, const uint16_t *service_ports, const char *interface_name)
{
    linkg_cellular_rx_t *rx;
    uint32_t             class_index;

    if (capacity == 0U || capacity > LINKG_CELLULAR_RX_BATCH_SIZE_MAX || packet_pool == NULL || socket_fds == NULL || service_ports == NULL || interface_name == NULL || interface_name[0] == '\0')
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
    rx->interface_name   = interface_name;
    rx->capacity         = capacity;
    rx->queue_capacity   = capacity * LINKG_CELLULAR_RX_QUEUE_BATCH_COUNT;
    rx->socket_epoll_fd  = -1;
    rx->worker_wakeup_fd = -1;
    rx->notify_fd        = -1;
    rx->worker_created   = false;

    atomic_store(&rx->started, false);
    atomic_store(&rx->notify_pending, false);

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        rx->queues[class_index] = linkg_cellular_rx_queue_create(rx->queue_capacity);
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

        linkg_cellular_rx_queue_destroy(rx->queues[class_index]);
        rx->queues[class_index] = NULL;
    }

    free(rx);

    return NULL;
}

/**
 * @brief 销毁蜂窝链路接收模块。
 *
 * 调用前Link RX消费线程不能再访问rx。
 */
void linkg_cellular_rx_destroy(linkg_cellular_rx_t *rx)
{
    uint32_t class_index;

    if (rx == NULL)
    {
        return;
    }

    (void)linkg_cellular_rx_stop(rx);

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        linkg_cellular_rx_queue_destroy(rx->queues[class_index]);
        rx->queues[class_index] = NULL;
    }

    free(rx);
}

/**
 * @brief 启动蜂窝链路接收模块。
 */
int linkg_cellular_rx_start(linkg_cellular_rx_t *rx)
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

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
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

    ret = _linkg_cellular_rx_register_fd(rx->socket_epoll_fd, rx->worker_wakeup_fd, LINKG_CELLULAR_RX_WORKER_WAKEUP_EVENT);
    if (ret != 0)
    {
        goto fail_notify;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        ret = _linkg_cellular_rx_register_fd(rx->socket_epoll_fd, rx->socket_fds[class_index], class_index);
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
    CPU_SET(LINKG_CELLULAR_RX_WORKER_CPU_CORE, &cpu_set);

    ret = pthread_attr_setaffinity_np(&worker_attr, sizeof(cpu_set), &cpu_set);
    if (ret != 0)
    {
        ret = -ret;
        goto fail_attr;
    }

    atomic_store(&rx->notify_pending, false);
    atomic_store(&rx->started, true);

    ret = pthread_create(&rx->worker, &worker_attr, _linkg_cellular_rx_worker, rx);
    if (ret != 0)
    {
        atomic_store(&rx->started, false);
        ret = -ret;
        goto fail_attr;
    }

    rx->worker_created = true;

    pthread_attr_destroy(&worker_attr);

    return 0;

fail_attr:
    if (attr_initialized)
    {
        pthread_attr_destroy(&worker_attr);
    }

fail_notify:
    (void)_linkg_cellular_rx_close_fd(&rx->notify_fd);

fail_wakeup:
    (void)_linkg_cellular_rx_close_fd(&rx->worker_wakeup_fd);

fail_epoll:
    (void)_linkg_cellular_rx_close_fd(&rx->socket_epoll_fd);

    return ret;
}

/**
 * @brief 停止蜂窝链路接收模块。
 *
 * 停止生产线程后清空三业务Queue，并释放其中尚未消费的Packet基础引用。
 */
int linkg_cellular_rx_stop(linkg_cellular_rx_t *rx)
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
        ret = _linkg_cellular_rx_signal_fd(rx->worker_wakeup_fd);
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

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        linkg_cellular_rx_queue_clear(rx->queues[class_index]);
    }

    atomic_store_explicit(&rx->notify_pending, false, memory_order_release);

    ret = _linkg_cellular_rx_close_fd(&rx->notify_fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_cellular_rx_close_fd(&rx->worker_wakeup_fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_cellular_rx_close_fd(&rx->socket_epoll_fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/****************************** 数据接收 ******************************/

/**
 * @brief 获取Link RX用于等待已完成蜂窝Packet的通知描述符。
 */
int linkg_cellular_rx_get_fd(linkg_cellular_rx_t *rx)
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
 * @brief 按实时、视频、普通顺序批量交付已经接收完成的蜂窝Packet。
 *
 * 每个返回Packet的基础引用从对应蜂窝RX Queue转移给Link RX，
 * items为输出数组，同一批次允许混合REALTIME、VIDEO和DATA。
 */
int linkg_cellular_rx_receive_batch(linkg_cellular_rx_t *rx, linkg_link_rx_item_t *items, uint32_t capacity)
{
    static const linkg_link_tx_class_t priority_order[LINKG_LINK_TX_CLASS_COUNT] =
    {
        LINKG_LINK_TX_CLASS_REALTIME,
        LINKG_LINK_TX_CLASS_VIDEO,
        LINKG_LINK_TX_CLASS_DATA
    };

    linkg_cellular_rx_queue_t *queue;
    linkg_link_tx_class_t      tx_class;
    uint32_t                   received_count;
    uint32_t                   remaining;
    uint32_t                   pop_count;
    uint32_t                   order_index;
    uint32_t                   index;
    int                        ret;

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

    for (order_index = 0U; order_index < LINKG_LINK_TX_CLASS_COUNT && remaining > 0U; order_index++)
    {
        tx_class = priority_order[order_index];
        queue    = rx->queues[tx_class];

        pop_count = linkg_cellular_rx_queue_pop_batch(queue, rx->consume_items, remaining);
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
    ret = _linkg_cellular_rx_clear_notify(rx);
    if (ret != 0)
    {
        return ret;
    }

    atomic_store_explicit(&rx->notify_pending, false, memory_order_release);

    if (!_linkg_cellular_rx_queues_empty(rx))
    {
        atomic_store_explicit(&rx->notify_pending, true, memory_order_release);
        goto retry;
    }

    return 0;
}
