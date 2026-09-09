/**
 * @file wifi_rx.c
 * @brief LinkG Wi-Fi接收模块实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-09
 */

#define _GNU_SOURCE

#include "wifi_rx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_network_ops.h"
#include "linkg_packet_pool.h"

#include "wifi_traffic.h"

/****************************** 接收参数 ******************************/

#define LINKG_WIFI_RX_IPV4_HEADER_SIZE     20U                                                                               // IPv4最小头部长度
#define LINKG_WIFI_RX_UDP_HEADER_SIZE      8U                                                                                // UDP头部长度
#define LINKG_WIFI_RX_MTU                  1500U                                                                             // Wi-Fi接口MTU
#define LINKG_WIFI_RX_UDP_PAYLOAD_MAX      (LINKG_WIFI_RX_MTU - LINKG_WIFI_RX_IPV4_HEADER_SIZE - LINKG_WIFI_RX_UDP_HEADER_SIZE) // 单个UDP报文最大负载

/****************************** 内部类型 ******************************/

/**
 * @brief Wi-Fi接收模块运行上下文。
 */
struct linkg_wifi_rx
{
    struct mmsghdr   *messages;       // 批量接收消息数组
    struct iovec     *iovecs;         // 批量接收缓冲区数组

    int              *socket_fds;     // 借用Wi-Fi Link业务Socket数组
    const uint16_t   *service_ports;  // 借用Wi-Fi Link业务端口数组

    uint32_t          capacity;       // 接收描述符容量
    int               epoll_fd;       // 三业务接收聚合描述符
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 将业务套接字加入接收epoll。
 */
static int _linkg_wifi_rx_register_socket(int epoll_fd, int socket_fd, linkg_wifi_traffic_class_t traffic_class)
{
    struct epoll_event event;
    int                ret;

    if (epoll_fd < 0 || socket_fd < 0)
    {
        return -EINVAL;
    }

    memset(&event, 0, sizeof(event));

    event.events   = EPOLLIN;
    event.data.u32 = (uint32_t)traffic_class;

    ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, socket_fd, &event);
    if (ret != 0)
    {
        return -errno;
    }

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
 * @brief 根据接收业务类别设置Packet业务标志。
 */
static void _linkg_wifi_rx_set_received_class(linkg_packet_t *packet, linkg_wifi_traffic_class_t traffic_class)
{
    if (packet == NULL)
    {
        return;
    }

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
 * @brief 获取指定业务套接字当前待处理错误。
 */
static int _linkg_wifi_rx_socket_error(const linkg_wifi_rx_t *rx, linkg_wifi_traffic_class_t traffic_class)
{
    socklen_t error_length;
    int       socket_error;
    int       ret;

    if (rx == NULL || traffic_class >= LINKG_WIFI_TRAFFIC_COUNT)
    {
        return -EINVAL;
    }

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
 * @brief 从指定业务套接字批量接收数据包。
 *
 * @note items中的Packet由Link RX提供基础引用，本函数仅填充Packet内容和来源Endpoint。
 */
static int _linkg_wifi_rx_receive_from(linkg_wifi_rx_t *rx, linkg_wifi_traffic_class_t traffic_class, linkg_link_rx_item_t *items, uint32_t capacity)
{
    linkg_link_rx_item_t temporary;
    struct sockaddr_in  *source;
    linkg_packet_t      *packet;
    uint32_t             packet_capacity;
    uint32_t             valid_count;
    uint32_t             index;
    int                  socket_fd;
    int                  ret;

    if (rx == NULL || items == NULL)
    {
        return -EINVAL;
    }

    if (traffic_class >= LINKG_WIFI_TRAFFIC_COUNT)
    {
        return -EINVAL;
    }

    socket_fd = rx->socket_fds[traffic_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    memset(rx->messages, 0, (size_t)capacity * sizeof(*rx->messages));

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

        rx->iovecs[index].iov_base = linkg_packet_data(packet);
        rx->iovecs[index].iov_len  = packet_capacity;

        if (rx->iovecs[index].iov_len > LINKG_WIFI_RX_UDP_PAYLOAD_MAX)
        {
            rx->iovecs[index].iov_len = LINKG_WIFI_RX_UDP_PAYLOAD_MAX;
        }

        rx->messages[index].msg_hdr.msg_name    = &items[index].source.address;
        rx->messages[index].msg_hdr.msg_namelen = sizeof(items[index].source.address);
        rx->messages[index].msg_hdr.msg_iov     = &rx->iovecs[index];
        rx->messages[index].msg_hdr.msg_iovlen  = 1U;
    }

    do
    {
        ret = recvmmsg(socket_fd, rx->messages, capacity, MSG_DONTWAIT, NULL);
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

        items[index].source.length = rx->messages[index].msg_hdr.msg_namelen;

        if ((rx->messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0)
        {
            continue;
        }

        if (rx->messages[index].msg_len == 0U)
        {
            continue;
        }

        if (rx->messages[index].msg_len > packet_capacity)
        {
            continue;
        }

        if (rx->messages[index].msg_len > LINKG_WIFI_RX_UDP_PAYLOAD_MAX)
        {
            continue;
        }

        if (items[index].source.length != sizeof(struct sockaddr_in))
        {
            continue;
        }

        if (items[index].source.address.ss_family != AF_INET)
        {
            continue;
        }

        source = (struct sockaddr_in *)&items[index].source.address;

        if (!linkg_network_ipv4_address_valid(&source->sin_addr))
        {
            continue;
        }

        if (source->sin_port != htons(rx->service_ports[traffic_class]))
        {
            continue;
        }

        // 统一使用DATA端口表示同一Wi-Fi Peer的规范化Endpoint。
        source->sin_port = htons(rx->service_ports[LINKG_WIFI_TRAFFIC_DATA]);

        packet->data_length = rx->messages[index].msg_len;

        _linkg_wifi_rx_set_received_class(packet, traffic_class);

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

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi接收模块。
 */
linkg_wifi_rx_t *linkg_wifi_rx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports)
{
    linkg_wifi_rx_t *rx;

    if (capacity == 0U || socket_fds == NULL || service_ports == NULL)
    {
        return NULL;
    }

    rx = calloc(1, sizeof(*rx));
    if (rx == NULL)
    {
        return NULL;
    }

    rx->socket_fds    = socket_fds;
    rx->service_ports = service_ports;
    rx->capacity      = capacity;
    rx->epoll_fd      = -1;

    rx->messages = calloc(rx->capacity, sizeof(*rx->messages));
    if (rx->messages == NULL)
    {
        goto fail_rx;
    }

    rx->iovecs = calloc(rx->capacity, sizeof(*rx->iovecs));
    if (rx->iovecs == NULL)
    {
        goto fail_messages;
    }

    return rx;

fail_messages:
    free(rx->messages);
    rx->messages = NULL;

fail_rx:
    free(rx);

    return NULL;
}

/**
 * @brief 销毁Wi-Fi接收模块。
 */
void linkg_wifi_rx_destroy(linkg_wifi_rx_t *rx)
{
    if (rx == NULL)
    {
        return;
    }

    (void)linkg_wifi_rx_stop(rx);

    free(rx->iovecs);
    free(rx->messages);

    rx->iovecs   = NULL;
    rx->messages = NULL;
    rx->capacity = 0U;

    free(rx);
}

/**
 * @brief 启动Wi-Fi接收模块。
 */
int linkg_wifi_rx_start(linkg_wifi_rx_t *rx)
{
    linkg_wifi_traffic_class_t traffic_class;
    uint32_t                   class_index;
    int                        epoll_fd;
    int                        ret;

    if (rx == NULL)
    {
        return -EINVAL;
    }

    if (rx->epoll_fd >= 0)
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

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        return -errno;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        traffic_class = (linkg_wifi_traffic_class_t)class_index;

        ret = _linkg_wifi_rx_register_socket(epoll_fd, rx->socket_fds[class_index], traffic_class);
        if (ret != 0)
        {
            (void)_linkg_wifi_rx_close_fd(&epoll_fd);
            return ret;
        }
    }

    rx->epoll_fd = epoll_fd;

    return 0;
}

/**
 * @brief 停止Wi-Fi接收模块。
 */
int linkg_wifi_rx_stop(linkg_wifi_rx_t *rx)
{
    if (rx == NULL)
    {
        return -EINVAL;
    }

    return _linkg_wifi_rx_close_fd(&rx->epoll_fd);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 获取Wi-Fi接收等待描述符。
 */
int linkg_wifi_rx_get_fd(linkg_wifi_rx_t *rx)
{
    if (rx == NULL)
    {
        return -EINVAL;
    }

    if (rx->epoll_fd < 0)
    {
        return -ENODEV;
    }

    return rx->epoll_fd;
}

/**
 * @brief 按实时、视频、普通顺序批量接收Wi-Fi数据包。
 */
int linkg_wifi_rx_receive_batch(linkg_wifi_rx_t *rx, linkg_link_rx_item_t *items, uint32_t capacity)
{
    struct epoll_event         events[LINKG_WIFI_TRAFFIC_COUNT];
    linkg_wifi_traffic_class_t traffic_class;
    bool                       ready[LINKG_WIFI_TRAFFIC_COUNT] = { false };
    uint32_t                   class_index;
    uint32_t                   event_index;
    int                        event_count;
    int                        ret;

    if (rx == NULL || items == NULL)
    {
        return -EINVAL;
    }

    if (capacity == 0U)
    {
        return 0;
    }

    if (rx->epoll_fd < 0)
    {
        return -ENODEV;
    }

    if (capacity > rx->capacity)
    {
        return -EOVERFLOW;
    }

    do
    {
        event_count = epoll_wait(rx->epoll_fd, events, LINKG_WIFI_TRAFFIC_COUNT, 0);
    }
    while (event_count < 0 && errno == EINTR);

    if (event_count < 0)
    {
        return -errno;
    }

    for (event_index = 0U; event_index < (uint32_t)event_count; event_index++)
    {
        if (events[event_index].data.u32 >= LINKG_WIFI_TRAFFIC_COUNT)
        {
            return -EIO;
        }

        traffic_class = (linkg_wifi_traffic_class_t)events[event_index].data.u32;

        if ((events[event_index].events & EPOLLERR) != 0U)
        {
            return _linkg_wifi_rx_socket_error(rx, traffic_class);
        }

        if ((events[event_index].events & EPOLLHUP) != 0U)
        {
            return -EPIPE;
        }

        if ((events[event_index].events & EPOLLIN) != 0U)
        {
            ready[traffic_class] = true;
        }
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        traffic_class = (linkg_wifi_traffic_class_t)class_index;

        if (!ready[traffic_class])
        {
            continue;
        }

        ret = _linkg_wifi_rx_receive_from(rx, traffic_class, items, capacity);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}
