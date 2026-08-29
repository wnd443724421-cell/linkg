/**
 * @file linkg_wifi_link.c
 * @brief LinkG Wi-Fi数据链路实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#define _GNU_SOURCE

#include "linkg_wifi_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_network_ops.h"
#include "linkg_packet_pool.h"

#include "link_internal.h"
#include "wifi_platform_internal.h"
#include "wifi_traffic.h"
#include "wifi_tx.h"

/****************************** 模块常量 ******************************/

#define LINKG_WIFI_LINK_IPV4_HEADER_SIZE 20U   // IPv4最小头部长度
#define LINKG_WIFI_LINK_UDP_HEADER_SIZE  8U    // UDP头部长度
#define LINKG_WIFI_LINK_MTU              1500U // Wi-Fi接口MTU
#define LINKG_WIFI_LINK_IP_TOS_DATA      0x00  // 普通数据流量，映射WMM AC_BE
#define LINKG_WIFI_LINK_IP_TOS_VIDEO     0x80  // 视频流量，映射WMM AC_VI
#define LINKG_WIFI_LINK_IP_TOS_REALTIME  0xC0  // 实时流量，映射WMM AC_VO
#define LINKG_WIFI_LINK_UDP_PAYLOAD_MAX  (LINKG_WIFI_LINK_MTU - LINKG_WIFI_LINK_IPV4_HEADER_SIZE - LINKG_WIFI_LINK_UDP_HEADER_SIZE) // 单个UDP报文最大负载

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_link_t           base;                                           // 链路基类，必须为首成员

    struct in_addr         local_address;                                  // 当前绑定的本地IPv4地址
    uint16_t               service_ports[LINKG_WIFI_TRAFFIC_COUNT];        // 各业务UDP服务端口
    uint32_t               send_buffer_sizes[LINKG_WIFI_TRAFFIC_COUNT];    // 各业务UDP发送缓冲请求值
    int                    socket_fds[LINKG_WIFI_TRAFFIC_COUNT];           // 各业务UDP收发套接字
    int                    rx_epoll_fd;                                    // 三业务接收聚合描述符

    linkg_wifi_tx_t       *tx;                                             // Wi-Fi发送模块

    struct mmsghdr        *rx_messages;                                    // 批量接收消息数组
    struct iovec          *rx_iovecs;                                      // 批量接收缓冲区数组
    uint32_t               rx_capacity;                                    // 接收描述符容量
} linkg_wifi_link_t;

_Static_assert(offsetof(linkg_wifi_link_t, base) == 0U, "linkg_link_t must be the first member");

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取业务类别对应的IPv4 TOS。
 */
static int _wifi_link_traffic_tos(linkg_wifi_traffic_class_t traffic_class)
{
    switch (traffic_class)
    {
        case LINKG_WIFI_TRAFFIC_REALTIME:
            return LINKG_WIFI_LINK_IP_TOS_REALTIME;

        case LINKG_WIFI_TRAFFIC_VIDEO:
            return LINKG_WIFI_LINK_IP_TOS_VIDEO;

        case LINKG_WIFI_TRAFFIC_DATA:
            return LINKG_WIFI_LINK_IP_TOS_DATA;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 创建并绑定单个业务UDP套接字。
 */
static int _wifi_link_open_socket(const struct in_addr *local_address, uint16_t local_port, int tos, uint32_t send_buffer_size, int *out)
{
    struct sockaddr_in local;
    int                socket_fd;
    int                send_buffer;
    int                enable;
    int                ret;

    if (local_address == NULL || out == NULL || local_port == 0U)
    {
        return -EINVAL;
    }

    if (send_buffer_size > (uint32_t)INT_MAX)
    {
        return -EINVAL;
    }

    *out = -1;

    socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
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

    if (send_buffer_size > 0U)
    {
        send_buffer = (int)send_buffer_size;

        ret = setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
        if (ret != 0)
        {
            ret = -errno;
            goto fail_socket;
        }
    }

    ret = setsockopt(socket_fd,
                     SOL_SOCKET,
                     SO_BINDTODEVICE,
                     WIFI_PLATFORM_INTERFACE_NAME,
                     strlen(WIFI_PLATFORM_INTERFACE_NAME));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    memset(&local, 0, sizeof(local));

    local.sin_family = AF_INET;
    local.sin_addr   = *local_address;
    local.sin_port   = htons(local_port);

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
static int _wifi_link_register_rx_socket(int epoll_fd, int socket_fd, linkg_wifi_traffic_class_t traffic_class)
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
static int _wifi_link_close_fd(int *descriptor)
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
 * @brief 根据接收业务类别设置数据包业务标志。
 */
static void _wifi_link_set_received_class(linkg_packet_t *packet, linkg_wifi_traffic_class_t traffic_class)
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
static int _wifi_link_socket_error(const linkg_wifi_link_t *wifi_link, linkg_wifi_traffic_class_t traffic_class)
{
    socklen_t error_length;
    int       socket_error;
    int       ret;

    if (wifi_link == NULL || traffic_class >= LINKG_WIFI_TRAFFIC_COUNT)
    {
        return -EINVAL;
    }

    socket_error = 0;
    error_length = sizeof(socket_error);

    ret = getsockopt(wifi_link->socket_fds[traffic_class],
                     SOL_SOCKET,
                     SO_ERROR,
                     &socket_error,
                     &error_length);
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
static int _wifi_link_receive_from(linkg_wifi_link_t *wifi_link, linkg_wifi_traffic_class_t traffic_class, linkg_link_rx_item_t *items, uint32_t capacity)
{
    linkg_link_rx_item_t temporary;
    struct sockaddr_in  *source;
    linkg_packet_t      *packet;
    uint32_t             packet_capacity;
    uint32_t             valid_count;
    uint32_t             index;
    int                  socket_fd;
    int                  ret;

    if (wifi_link == NULL || items == NULL)
    {
        return -EINVAL;
    }

    if (traffic_class >= LINKG_WIFI_TRAFFIC_COUNT)
    {
        return -EINVAL;
    }

    socket_fd = wifi_link->socket_fds[traffic_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    memset(wifi_link->rx_messages, 0, (size_t)capacity * sizeof(*wifi_link->rx_messages));

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

        wifi_link->rx_iovecs[index].iov_base = linkg_packet_data(packet);
        wifi_link->rx_iovecs[index].iov_len  = packet_capacity;

        if (wifi_link->rx_iovecs[index].iov_len > LINKG_WIFI_LINK_UDP_PAYLOAD_MAX)
        {
            wifi_link->rx_iovecs[index].iov_len = LINKG_WIFI_LINK_UDP_PAYLOAD_MAX;
        }

        wifi_link->rx_messages[index].msg_hdr.msg_name    = &items[index].source.address;
        wifi_link->rx_messages[index].msg_hdr.msg_namelen = sizeof(items[index].source.address);
        wifi_link->rx_messages[index].msg_hdr.msg_iov     = &wifi_link->rx_iovecs[index];
        wifi_link->rx_messages[index].msg_hdr.msg_iovlen  = 1U;
    }

    do
    {
        ret = recvmmsg(socket_fd, wifi_link->rx_messages, capacity, MSG_DONTWAIT, NULL);
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

        items[index].source.length = wifi_link->rx_messages[index].msg_hdr.msg_namelen;

        if ((wifi_link->rx_messages[index].msg_hdr.msg_flags & MSG_TRUNC) != 0)
        {
            continue;
        }

        if (wifi_link->rx_messages[index].msg_len == 0U)
        {
            continue;
        }

        if (wifi_link->rx_messages[index].msg_len > packet_capacity)
        {
            continue;
        }

        if (wifi_link->rx_messages[index].msg_len > LINKG_WIFI_LINK_UDP_PAYLOAD_MAX)
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

        if (source->sin_port != htons(wifi_link->service_ports[traffic_class]))
        {
            continue;
        }

        // 统一使用DATA端口表示同一Wi-Fi Peer的规范化Endpoint。
        source->sin_port = htons(wifi_link->service_ports[LINKG_WIFI_TRAFFIC_DATA]);

        packet->data_length = wifi_link->rx_messages[index].msg_len;

        _wifi_link_set_received_class(packet, traffic_class);

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

/****************************** 具体链路生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi具体链路对象。
 */
static int _wifi_link_init(linkg_link_t *link, const void *config)
{
    const linkg_wifi_link_config_t *wifi_config;
    linkg_wifi_link_t              *wifi_link;
    uint32_t                        class_index;

    if (link == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (link->runtime == NULL)
    {
        return -ENODEV;
    }

    wifi_link   = (linkg_wifi_link_t *)link;
    wifi_config = config;

    memset(&wifi_link->local_address, 0, sizeof(wifi_link->local_address));

    wifi_link->service_ports[LINKG_WIFI_TRAFFIC_REALTIME] = wifi_config->realtime_port;
    wifi_link->service_ports[LINKG_WIFI_TRAFFIC_VIDEO]    = wifi_config->video_port;
    wifi_link->service_ports[LINKG_WIFI_TRAFFIC_DATA]     = wifi_config->data_port;

    wifi_link->send_buffer_sizes[LINKG_WIFI_TRAFFIC_REALTIME] = wifi_config->realtime_send_buffer_size;
    wifi_link->send_buffer_sizes[LINKG_WIFI_TRAFFIC_VIDEO]    = wifi_config->video_send_buffer_size;
    wifi_link->send_buffer_sizes[LINKG_WIFI_TRAFFIC_DATA]     = wifi_config->data_send_buffer_size;

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        wifi_link->socket_fds[class_index] = -1;
    }

    wifi_link->rx_epoll_fd = -1;
    wifi_link->rx_capacity = link->runtime->rx_batch_size;

    wifi_link->tx = linkg_wifi_tx_create(link->runtime->tx_batch_size,
                                         wifi_link->socket_fds,
                                         wifi_link->service_ports);
    if (wifi_link->tx == NULL)
    {
        return -ENOMEM;
    }

    wifi_link->rx_messages = calloc(wifi_link->rx_capacity, sizeof(*wifi_link->rx_messages));
    if (wifi_link->rx_messages == NULL)
    {
        goto fail_tx;
    }

    wifi_link->rx_iovecs = calloc(wifi_link->rx_capacity, sizeof(*wifi_link->rx_iovecs));
    if (wifi_link->rx_iovecs == NULL)
    {
        goto fail_rx_messages;
    }

    return 0;

fail_rx_messages:
    free(wifi_link->rx_messages);
    wifi_link->rx_messages = NULL;

fail_tx:
    linkg_wifi_tx_destroy(wifi_link->tx);
    wifi_link->tx = NULL;

    return -ENOMEM;
}

/**
 * @brief 反初始化Wi-Fi具体链路对象。
 */
static void _wifi_link_deinit(linkg_link_t *link)
{
    linkg_wifi_link_t *wifi_link;
    uint32_t           class_index;

    if (link == NULL)
    {
        return;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    linkg_wifi_tx_destroy(wifi_link->tx);
    wifi_link->tx = NULL;

    free(wifi_link->rx_iovecs);
    free(wifi_link->rx_messages);

    wifi_link->rx_iovecs   = NULL;
    wifi_link->rx_messages = NULL;
    wifi_link->rx_capacity = 0U;

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        wifi_link->service_ports[class_index]     = 0U;
        wifi_link->send_buffer_sizes[class_index] = 0U;
        wifi_link->socket_fds[class_index]        = -1;
    }

    wifi_link->rx_epoll_fd = -1;

    memset(&wifi_link->local_address, 0, sizeof(wifi_link->local_address));
}

/**
 * @brief 打开Wi-Fi具体链路运行资源。
 */
static int _wifi_link_open(linkg_link_t *link)
{
    linkg_wifi_link_t           *wifi_link;
    int                          socket_fds[LINKG_WIFI_TRAFFIC_COUNT];
    linkg_wifi_traffic_class_t   traffic_class;
    uint32_t                     class_index;
    uint32_t                     send_buffer_size;
    int                          epoll_fd;
    int                          tos;
    int                          ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    if (wifi_link->rx_epoll_fd >= 0)
    {
        return -EALREADY;
    }

    epoll_fd = -1;

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        socket_fds[class_index] = -1;
    }

    ret = linkg_network_interface_get_ipv4(WIFI_PLATFORM_INTERFACE_NAME, &wifi_link->local_address);
    if (ret != 0)
    {
        return ret;
    }

    epoll_fd = epoll_create1(EPOLL_CLOEXEC);
    if (epoll_fd < 0)
    {
        return -errno;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        traffic_class   = (linkg_wifi_traffic_class_t)class_index;
        tos             = _wifi_link_traffic_tos(traffic_class);
        send_buffer_size = wifi_link->send_buffer_sizes[traffic_class];

        if (tos < 0)
        {
            ret = tos;
            goto fail;
        }

        ret = _wifi_link_open_socket(&wifi_link->local_address,
                                     wifi_link->service_ports[class_index],
                                     tos,
                                     send_buffer_size,
                                     &socket_fds[class_index]);
        if (ret != 0)
        {
            goto fail;
        }

        ret = _wifi_link_register_rx_socket(epoll_fd, socket_fds[class_index], traffic_class);
        if (ret != 0)
        {
            goto fail;
        }
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        wifi_link->socket_fds[class_index] = socket_fds[class_index];
        socket_fds[class_index]            = -1;
    }

    wifi_link->rx_epoll_fd = epoll_fd;
    epoll_fd               = -1;

    ret = linkg_wifi_tx_start(wifi_link->tx);
    if (ret != 0)
    {
        goto fail_opened;
    }

    return 0;

fail_opened:
    (void)_wifi_link_close_fd(&wifi_link->rx_epoll_fd);

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        (void)_wifi_link_close_fd(&wifi_link->socket_fds[class_index]);
    }

fail:
    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        (void)_wifi_link_close_fd(&socket_fds[class_index]);
    }

    (void)_wifi_link_close_fd(&epoll_fd);

    return ret;
}

/**
 * @brief 关闭Wi-Fi具体链路运行资源。
 */
static int _wifi_link_close(linkg_link_t *link)
{
    linkg_wifi_link_t *wifi_link;
    uint32_t           class_index;
    int                first_error;
    int                ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link   = (linkg_wifi_link_t *)link;
    first_error = 0;

    linkg_wifi_tx_stop(wifi_link->tx);

    ret = _wifi_link_close_fd(&wifi_link->rx_epoll_fd);
    if (ret != 0)
    {
        first_error = ret;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        ret = _wifi_link_close_fd(&wifi_link->socket_fds[class_index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 等待描述符 ******************************/

/**
 * @brief 获取Wi-Fi链路接收等待描述符。
 */
static int _wifi_link_get_rx_fd(linkg_link_t *link)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    if (wifi_link->rx_epoll_fd < 0)
    {
        return -ENODEV;
    }

    return wifi_link->rx_epoll_fd;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 将同一业务类别发送请求提交给Wi-Fi TX模块。
 *
 * @note Packet和Path引用所有权仍由调用方持有，Wi-Fi TX需要异步保存时自行增加引用。
 */
static int _wifi_link_send_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    return linkg_wifi_tx_submit(wifi_link->tx, path, tx_class, destination, packets, count, results);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 按实时、视频、普通顺序批量接收Wi-Fi数据包。
 */
static int _wifi_link_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t capacity)
{
    struct epoll_event         events[LINKG_WIFI_TRAFFIC_COUNT];
    linkg_wifi_traffic_class_t traffic_class;
    bool                       ready[LINKG_WIFI_TRAFFIC_COUNT] = { false };
    linkg_wifi_link_t         *wifi_link;
    uint32_t                   class_index;
    uint32_t                   event_index;
    int                        event_count;
    int                        ret;

    if (link == NULL || items == NULL)
    {
        return -EINVAL;
    }

    if (capacity == 0U)
    {
        return 0;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    if (wifi_link->rx_epoll_fd < 0)
    {
        return -ENODEV;
    }

    if (capacity > wifi_link->rx_capacity)
    {
        return -EOVERFLOW;
    }

    do
    {
        event_count = epoll_wait(wifi_link->rx_epoll_fd,
                                 events,
                                 LINKG_WIFI_TRAFFIC_COUNT,
                                 0);
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
            return _wifi_link_socket_error(wifi_link, traffic_class);
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

        ret = _wifi_link_receive_from(wifi_link, traffic_class, items, capacity);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 操作接口 ******************************/

static const linkg_link_ops_t g_wifi_link_ops =
{
    .instance_size = sizeof(linkg_wifi_link_t),
    .init          = _wifi_link_init,
    .deinit        = _wifi_link_deinit,
    .open          = _wifi_link_open,
    .close         = _wifi_link_close,
    .get_rx_fd     = _wifi_link_get_rx_fd,
    .send_batch    = _wifi_link_send_batch,
    .receive_batch = _wifi_link_receive_batch,
};

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi数据链路对象。
 */
int linkg_wifi_link_create(const linkg_link_config_t *link_config, const linkg_wifi_link_config_t *wifi_config, linkg_link_t **out)
{
    if (link_config == NULL || wifi_config == NULL || out == NULL)
    {
        return -EINVAL;
    }

    if (link_config->access != LINKG_LINK_ACCESS_WIFI)
    {
        return -EINVAL;
    }

    if (wifi_config->data_port == 0U ||
        wifi_config->realtime_port == 0U ||
        wifi_config->video_port == 0U)
    {
        return -EINVAL;
    }

    if (wifi_config->data_port == wifi_config->realtime_port ||
        wifi_config->data_port == wifi_config->video_port ||
        wifi_config->realtime_port == wifi_config->video_port)
    {
        return -EINVAL;
    }

    return linkg_link_create(link_config, &g_wifi_link_ops, wifi_config, out);
}
