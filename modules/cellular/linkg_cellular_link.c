/**
 * @file linkg_cellular_link.c
 * @brief LinkG蜂窝IPv6数据链路实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-08-28
 */

#define _GNU_SOURCE

#include "linkg_cellular_link.h"

#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_link_internal.h"
#include "linkg_network_ops.h"
#include "linkg_packet_pool.h"
#include "linkg_system_resources.h"

#include "cellular_tx.h"

/****************************** 数据限制 ******************************/

#define LINKG_CELLULAR_LINK_IPV6_HEADER_SIZE   40U                                                      // IPv6固定头部长度
#define LINKG_CELLULAR_LINK_UDP_HEADER_SIZE    8U                                                       // UDP头部长度
#define LINKG_CELLULAR_LINK_MTU                1500U                                                    // 蜂窝接口MTU
#define LINKG_CELLULAR_LINK_UDP_PAYLOAD_MAX    (LINKG_CELLULAR_LINK_MTU - LINKG_CELLULAR_LINK_IPV6_HEADER_SIZE - LINKG_CELLULAR_LINK_UDP_HEADER_SIZE) // 单个UDP报文最大负载
#define LINKG_CELLULAR_LINK_RX_CONTROL_SIZE    CMSG_SPACE(sizeof(struct in6_pktinfo))                   // 单包IPv6辅助控制区大小
#define LINKG_CELLULAR_LINK_TCLASS_DATA        0x00                                                     // 普通数据IPv6 Traffic Class
#define LINKG_CELLULAR_LINK_TCLASS_VIDEO       0x80                                                     // 视频业务IPv6 Traffic Class
#define LINKG_CELLULAR_LINK_TCLASS_REALTIME    0xC0                                                     // 实时业务IPv6 Traffic Class

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_link_t          base;                                         // 链路基类，必须为首成员
    uint16_t              service_ports[LINKG_LINK_TX_CLASS_COUNT];     // 各业务IPv6 UDP服务端口
    int                   socket_fds[LINKG_LINK_TX_CLASS_COUNT];        // 各业务IPv6 UDP收发套接字
    int                   rx_epoll_fd;                                  // 三业务接收聚合描述符
    linkg_cellular_tx_t  *tx;                                           // 蜂窝发送模块
    struct mmsghdr       *rx_messages;                                  // 批量接收消息数组
    struct iovec         *rx_iovecs;                                    // 批量接收缓冲区数组
    unsigned char        *rx_controls;                                  // 批量接收IPv6辅助控制区
    uint32_t              rx_capacity;                                  // 接收描述符容量
} linkg_cellular_link_t;

_Static_assert(offsetof(linkg_cellular_link_t, base) == 0U, "linkg_link_t must be the first member");

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取业务类别对应的IPv6 Traffic Class。
 */
static int _cellular_link_tclass(linkg_link_tx_class_t tx_class)
{
    switch (tx_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            return LINKG_CELLULAR_LINK_TCLASS_REALTIME;

        case LINKG_LINK_TX_CLASS_VIDEO:
            return LINKG_CELLULAR_LINK_TCLASS_VIDEO;

        case LINKG_LINK_TX_CLASS_DATA:
            return LINKG_CELLULAR_LINK_TCLASS_DATA;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 关闭并清空单个描述符。
 */
static int _cellular_link_close_fd(int *descriptor)
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
 * @brief 创建并绑定单个业务IPv6 UDP套接字。
 */
static int _cellular_link_open_socket(uint16_t local_port, int tclass, int *out)
{
    struct sockaddr_in6 local;
    int                 socket_fd;
    int                 enable;
    int                 ret;

    if (local_port == 0U || out == NULL || tclass < 0 || tclass > UCHAR_MAX)
    {
        return -EINVAL;
    }

    *out = -1;

    socket_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        return -errno;
    }

    enable = 1;

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    memset(&local, 0, sizeof(local));

    local.sin6_family = AF_INET6;
    local.sin6_addr   = in6addr_any;
    local.sin6_port   = htons(local_port);

    ret = bind(socket_fd, (const struct sockaddr *)&local, sizeof(local));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    *out = socket_fd;

    return 0;

fail_socket:
    close(socket_fd);

    return ret;
}

/**
 * @brief 将业务套接字加入接收epoll。
 */
static int _cellular_link_register_rx_socket(int epoll_fd, int socket_fd, linkg_link_tx_class_t tx_class)
{
    struct epoll_event event;
    int                ret;

    if (epoll_fd < 0 || socket_fd < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    memset(&event, 0, sizeof(event));

    event.events   = EPOLLIN;
    event.data.u32 = (uint32_t)tx_class;

    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event);
    if (ret != 0)
    {
        return -errno;
    }

    return 0;
}

/****************************** 具体链路生命周期 ******************************/

/**
 * @brief 初始化蜂窝具体链路对象。
 */
static int _cellular_link_init(linkg_link_t *link, const void *config)
{
    const linkg_cellular_link_config_t *cellular_config;
    linkg_cellular_link_t              *cellular_link;
    uint32_t                            class_index;

    if (link == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (link->runtime == NULL)
    {
        return -ENODEV;
    }

    cellular_link   = (linkg_cellular_link_t *)link;
    cellular_config = config;

    cellular_link->service_ports[LINKG_LINK_TX_CLASS_REALTIME] = cellular_config->realtime_port;
    cellular_link->service_ports[LINKG_LINK_TX_CLASS_VIDEO]    = cellular_config->video_port;
    cellular_link->service_ports[LINKG_LINK_TX_CLASS_DATA]     = cellular_config->data_port;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        cellular_link->socket_fds[class_index] = -1;
    }

    cellular_link->rx_epoll_fd = -1;
    cellular_link->rx_capacity = link->runtime->rx_batch_size;

    cellular_link->tx = linkg_cellular_tx_create(link->runtime->tx_batch_size,
                                                  cellular_link->socket_fds,
                                                  cellular_link->service_ports,
                                                  LINKG_RESOURCE_INTERFACE_CELLULAR);
    if (cellular_link->tx == NULL)
    {
        return -ENOMEM;
    }

    cellular_link->rx_messages = calloc(cellular_link->rx_capacity, sizeof(*cellular_link->rx_messages));
    if (cellular_link->rx_messages == NULL)
    {
        goto fail_tx;
    }

    cellular_link->rx_iovecs = calloc(cellular_link->rx_capacity, sizeof(*cellular_link->rx_iovecs));
    if (cellular_link->rx_iovecs == NULL)
    {
        goto fail_rx_messages;
    }

    cellular_link->rx_controls = calloc(cellular_link->rx_capacity, LINKG_CELLULAR_LINK_RX_CONTROL_SIZE);
    if (cellular_link->rx_controls == NULL)
    {
        goto fail_rx_iovecs;
    }

    return 0;

fail_rx_iovecs:
    free(cellular_link->rx_iovecs);
    cellular_link->rx_iovecs = NULL;

fail_rx_messages:
    free(cellular_link->rx_messages);
    cellular_link->rx_messages = NULL;

fail_tx:
    linkg_cellular_tx_destroy(cellular_link->tx);
    cellular_link->tx = NULL;

    return -ENOMEM;
}

/**
 * @brief 反初始化蜂窝具体链路对象。
 */
static void _cellular_link_deinit(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;
    uint32_t               class_index;

    if (link == NULL)
    {
        return;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    linkg_cellular_tx_destroy(cellular_link->tx);
    cellular_link->tx = NULL;

    free(cellular_link->rx_controls);
    free(cellular_link->rx_iovecs);
    free(cellular_link->rx_messages);

    cellular_link->rx_controls = NULL;
    cellular_link->rx_iovecs   = NULL;
    cellular_link->rx_messages = NULL;
    cellular_link->rx_capacity = 0U;
    cellular_link->rx_epoll_fd = -1;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        cellular_link->socket_fds[class_index]    = -1;
        cellular_link->service_ports[class_index] = 0U;
    }
}

/**
 * @brief 打开蜂窝具体链路运行资源。
 */
static int _cellular_link_open(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;
    int                    socket_fds[LINKG_LINK_TX_CLASS_COUNT];
    linkg_link_tx_class_t  tx_class;
    uint32_t               class_index;
    int                    epoll_fd;
    int                    tclass;
    int                    ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    if (cellular_link->rx_epoll_fd >= 0)
    {
        return -EALREADY;
    }

    epoll_fd = -1;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        socket_fds[class_index] = -1;
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        return -errno;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        tx_class = (linkg_link_tx_class_t)class_index;
        tclass   = _cellular_link_tclass(tx_class);
        if (tclass < 0)
        {
            ret = tclass;
            goto fail;
        }

        ret = _cellular_link_open_socket(cellular_link->service_ports[class_index],
                                         tclass,
                                         &socket_fds[class_index]);
        if (ret != 0)
        {
            goto fail;
        }

        ret = _cellular_link_register_rx_socket(epoll_fd, socket_fds[class_index], tx_class);
        if (ret != 0)
        {
            goto fail;
        }
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        cellular_link->socket_fds[class_index] = socket_fds[class_index];
        socket_fds[class_index]                = -1;
    }

    cellular_link->rx_epoll_fd = epoll_fd;
    epoll_fd                    = -1;

    ret = linkg_cellular_tx_start(cellular_link->tx);
    if (ret != 0)
    {
        goto fail_opened;
    }

    return 0;

fail_opened:
    (void)_cellular_link_close_fd(&cellular_link->rx_epoll_fd);

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        (void)_cellular_link_close_fd(&cellular_link->socket_fds[class_index]);
    }

fail:
    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        (void)_cellular_link_close_fd(&socket_fds[class_index]);
    }

    (void)_cellular_link_close_fd(&epoll_fd);

    return ret;
}

/**
 * @brief 关闭蜂窝具体链路运行资源。
 */
static int _cellular_link_close(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;
    uint32_t               class_index;
    int                    first_error;
    int                    ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;
    first_error   = 0;

    linkg_cellular_tx_stop(cellular_link->tx);

    ret = _cellular_link_close_fd(&cellular_link->rx_epoll_fd);
    if (ret != 0)
    {
        first_error = ret;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        ret = _cellular_link_close_fd(&cellular_link->socket_fds[class_index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 等待描述符 ******************************/

/**
 * @brief 获取蜂窝链路接收等待描述符。
 */
static int _cellular_link_get_rx_fd(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    if (cellular_link->rx_epoll_fd < 0)
    {
        return -ENODEV;
    }

    return cellular_link->rx_epoll_fd;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 将同一业务类别发送请求提交给蜂窝TX模块。
 *
 * @note Packet和Path引用所有权仍由调用方持有，蜂窝TX需要异步保存时自行增加引用。
 */
static int _cellular_link_send_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_cellular_link_t *cellular_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    return linkg_cellular_tx_submit(cellular_link->tx, path, tx_class, destination, packets, count, results);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 根据接收业务类别设置数据包业务标志。
 */
static void _cellular_link_set_received_class(linkg_packet_t *packet, linkg_link_tx_class_t tx_class)
{
    if (packet == NULL)
    {
        return;
    }

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
 * @brief 获取指定业务套接字当前待处理错误。
 */
static int _cellular_link_socket_error(const linkg_cellular_link_t *cellular_link, linkg_link_tx_class_t tx_class)
{
    socklen_t error_length;
    int       socket_error;
    int       ret;

    if (cellular_link == NULL || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    socket_error = 0;
    error_length = sizeof(socket_error);

    ret = getsockopt(cellular_link->socket_fds[tx_class], SOL_SOCKET, SO_ERROR, &socket_error, &error_length);
    if (ret != 0)
    {
        return -errno;
    }

    return socket_error == 0 ? -EIO : -socket_error;
}

/**
 * @brief 从IPv6辅助控制信息中获取接收接口索引。
 */
static unsigned int _cellular_link_rx_ifindex(const struct msghdr *header)
{
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;

    if (header == NULL)
    {
        return 0U;
    }

    for (cmsg = CMSG_FIRSTHDR((struct msghdr *)header);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR((struct msghdr *)header, cmsg))
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
 * @brief 从指定业务套接字批量接收蜂窝IPv6数据包。
 *
 * @note items中的Packet由Link RX提供基础引用，本函数仅填充Packet内容和来源Endpoint。
 */
static int _cellular_link_receive_from(linkg_cellular_link_t *cellular_link, linkg_link_tx_class_t tx_class, linkg_link_rx_item_t *items, uint32_t capacity)
{
    linkg_link_rx_item_t temporary;
    struct sockaddr_in6 *source;
    unsigned char       *control;
    unsigned int         expected_ifindex;
    unsigned int         received_ifindex;
    linkg_packet_t      *packet;
    uint32_t             packet_capacity;
    uint32_t             valid_count;
    uint32_t             index;
    int                  socket_fd;
    int                  ret;

    if (cellular_link == NULL || items == NULL || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    if (capacity == 0U)
    {
        return 0;
    }

    socket_fd = cellular_link->socket_fds[tx_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    expected_ifindex = if_nametoindex(LINKG_RESOURCE_INTERFACE_CELLULAR);
    if (expected_ifindex == 0U)
    {
        return 0;
    }

    memset(cellular_link->rx_messages, 0, (size_t)capacity * sizeof(*cellular_link->rx_messages));
    memset(cellular_link->rx_controls, 0, (size_t)capacity * LINKG_CELLULAR_LINK_RX_CONTROL_SIZE);

    for (index = 0U; index < capacity; index++)
    {
        packet = items[index].packet;
        if (packet == NULL)
        {
            return -EINVAL;
        }

        packet_capacity = linkg_packet_capacity(packet);
        if (packet_capacity == 0U)
        {
            return -ENOBUFS;
        }

        packet->data_length = 0U;
        linkg_packet_set_data(packet);

        memset(&items[index].source, 0, sizeof(items[index].source));

        cellular_link->rx_iovecs[index].iov_base = linkg_packet_data(packet);
        cellular_link->rx_iovecs[index].iov_len  = packet_capacity;

        if (cellular_link->rx_iovecs[index].iov_len > LINKG_CELLULAR_LINK_UDP_PAYLOAD_MAX)
        {
            cellular_link->rx_iovecs[index].iov_len = LINKG_CELLULAR_LINK_UDP_PAYLOAD_MAX;
        }

        cellular_link->rx_messages[index].msg_hdr.msg_name    = &items[index].source.address;
        cellular_link->rx_messages[index].msg_hdr.msg_namelen = sizeof(items[index].source.address);
        cellular_link->rx_messages[index].msg_hdr.msg_iov     = &cellular_link->rx_iovecs[index];
        cellular_link->rx_messages[index].msg_hdr.msg_iovlen  = 1U;

        control = cellular_link->rx_controls + ((size_t)index * LINKG_CELLULAR_LINK_RX_CONTROL_SIZE);

        cellular_link->rx_messages[index].msg_hdr.msg_control    = control;
        cellular_link->rx_messages[index].msg_hdr.msg_controllen = LINKG_CELLULAR_LINK_RX_CONTROL_SIZE;
    }

    do
    {
        ret = recvmmsg(socket_fd, cellular_link->rx_messages, capacity, MSG_DONTWAIT, NULL);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }

        return -errno;
    }

    valid_count = 0U;

    for (index = 0U; index < (uint32_t)ret; index++)
    {
        packet          = items[index].packet;
        packet_capacity = linkg_packet_capacity(packet);

        items[index].source.length = cellular_link->rx_messages[index].msg_hdr.msg_namelen;

        if ((cellular_link->rx_messages[index].msg_hdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
        {
            continue;
        }

        if (cellular_link->rx_messages[index].msg_len == 0U)
        {
            continue;
        }

        if (cellular_link->rx_messages[index].msg_len > packet_capacity)
        {
            continue;
        }

        if (cellular_link->rx_messages[index].msg_len > LINKG_CELLULAR_LINK_UDP_PAYLOAD_MAX)
        {
            continue;
        }

        if (items[index].source.length != sizeof(struct sockaddr_in6))
        {
            continue;
        }

        if (items[index].source.address.ss_family != AF_INET6)
        {
            continue;
        }

        received_ifindex = _cellular_link_rx_ifindex(&cellular_link->rx_messages[index].msg_hdr);
        if (received_ifindex == 0U || received_ifindex != expected_ifindex)
        {
            continue;
        }

        source = (struct sockaddr_in6 *)&items[index].source.address;

        if (!linkg_network_ipv6_address_is_global(&source->sin6_addr))
        {
            continue;
        }

        if (source->sin6_port != htons(cellular_link->service_ports[tx_class]))
        {
            continue;
        }

        // 统一使用DATA端口表示同一蜂窝Peer的规范化Endpoint。
        source->sin6_port = htons(cellular_link->service_ports[LINKG_LINK_TX_CLASS_DATA]);

        packet->data_length = cellular_link->rx_messages[index].msg_len;

        _cellular_link_set_received_class(packet, tx_class);

        if (valid_count != index)
        {
            temporary          = items[valid_count];
            items[valid_count] = items[index];
            items[index]       = temporary;
        }

        valid_count++;
    }

    return (int)valid_count;
}

/**
 * @brief 按实时、视频、普通顺序批量接收蜂窝IPv6数据包。
 */
static int _cellular_link_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t capacity)
{
    struct epoll_event    events[LINKG_LINK_TX_CLASS_COUNT];
    bool                  ready[LINKG_LINK_TX_CLASS_COUNT] = { false };
    linkg_cellular_link_t *cellular_link;
    linkg_link_tx_class_t  tx_class;
    uint32_t               class_index;
    uint32_t               event_index;
    int                    event_count;
    int                    ret;

    if (link == NULL || items == NULL)
    {
        return -EINVAL;
    }

    if (capacity == 0U)
    {
        return 0;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    if (cellular_link->rx_epoll_fd < 0)
    {
        return -ENODEV;
    }

    if (capacity > cellular_link->rx_capacity)
    {
        return -EOVERFLOW;
    }

    do
    {
        event_count = epoll_wait(cellular_link->rx_epoll_fd,
                                 events,
                                 LINKG_LINK_TX_CLASS_COUNT,
                                 0);
    }
    while (event_count < 0 && errno == EINTR);

    if (event_count < 0)
    {
        return -errno;
    }

    for (event_index = 0U; event_index < (uint32_t)event_count; event_index++)
    {
        if (events[event_index].data.u32 >= LINKG_LINK_TX_CLASS_COUNT)
        {
            return -EIO;
        }

        tx_class = (linkg_link_tx_class_t)events[event_index].data.u32;

        if ((events[event_index].events & EPOLLERR) != 0U)
        {
            return _cellular_link_socket_error(cellular_link, tx_class);
        }

        if ((events[event_index].events & EPOLLHUP) != 0U)
        {
            return -EPIPE;
        }

        if ((events[event_index].events & EPOLLIN) != 0U)
        {
            ready[tx_class] = true;
        }
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        tx_class = (linkg_link_tx_class_t)class_index;

        if (!ready[tx_class])
        {
            continue;
        }

        ret = _cellular_link_receive_from(cellular_link, tx_class, items, capacity);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 操作接口 ******************************/

static const linkg_link_ops_t g_cellular_link_ops =
{
    .instance_size = sizeof(linkg_cellular_link_t),
    .init          = _cellular_link_init,
    .deinit        = _cellular_link_deinit,
    .open          = _cellular_link_open,
    .close         = _cellular_link_close,
    .get_rx_fd     = _cellular_link_get_rx_fd,
    .send_batch    = _cellular_link_send_batch,
    .receive_batch = _cellular_link_receive_batch,
};

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝IPv6数据链路对象。
 */
int linkg_cellular_link_create(const linkg_link_config_t *link_config, const linkg_cellular_link_config_t *cellular_config, linkg_link_t **out)
{
    if (link_config == NULL || cellular_config == NULL || out == NULL)
    {
        return -EINVAL;
    }

    if (link_config->access != LINKG_LINK_ACCESS_CELLULAR)
    {
        return -EINVAL;
    }

    if (cellular_config->data_port == 0U ||
        cellular_config->realtime_port == 0U ||
        cellular_config->video_port == 0U)
    {
        return -EINVAL;
    }

    if (cellular_config->data_port == cellular_config->realtime_port ||
        cellular_config->data_port == cellular_config->video_port ||
        cellular_config->realtime_port == cellular_config->video_port)
    {
        return -EINVAL;
    }

    return linkg_link_create(link_config, &g_cellular_link_ops, cellular_config, out);
}
