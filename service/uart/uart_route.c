/**
 * @file uart_route.c
 * @brief LinkG串口TCP/UDP单播路由和非阻塞Socket管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#define _GNU_SOURCE

#include "uart_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_discovery.h"
#include "linkg_log.h"
#include "linkg_network.h"

/****************************** Socket辅助 ******************************/

/**
 * @brief 关闭一个Socket并恢复无效描述符。
 */
static void _uart_route_close_fd(int *fd)
{
    if (fd != NULL && *fd >= 0)
    {
        close(*fd);
        *fd = -1;
    }
}

/**
 * @brief 创建非阻塞IPv4 Socket。
 */
static int _uart_route_socket(int type)
{
    int fd;

    fd = socket(AF_INET, type | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    return fd >= 0 ? fd : -errno;
}

/**
 * @brief 构造已校验的本地或远端IPv4地址。
 */
static struct sockaddr_in _uart_route_sockaddr(const linkg_uart_route_config_t *config)
{
    struct sockaddr_in address;

    memset(&address, 0, sizeof(address));
    address.sin_family      = AF_INET;
    address.sin_addr        = config->ip;
    address.sin_port        = htons(config->port);
    return address;
}

/**
 * @brief 丢弃无法继续发送的TCP缓存并记录一次数据丢弃事件。
 */
static void _uart_route_discard_tcp(linkg_uart_route_t *route)
{
    if (route->tcp_tx_count > 0U)
    {
        atomic_fetch_add(&route->parent->stats.drops, 1U);
        route->tcp_tx_count = 0U;
    }
}

/**
 * @brief 等待LinkG虚拟地址对应的远端节点上线。
 */
static bool _uart_route_wait_for_peer(linkg_uart_route_t *route, uint64_t now_ms)
{
    linkg_network_config_t network;
    bool                   online;
    uint8_t                node_id;
    int                    ret;

    if (route->config->endpoint != LINKG_UART_ROUTE_ENDPOINT_REMOTE)
    {
        route->waiting_for_peer = false;
        return false;
    }

    ret = linkg_network_get_running_config(&network);
    if (ret == 0 && linkg_network_config_get_virtual_node_id(&network, &route->config->ip, &node_id) != 0)
    {
        route->waiting_for_peer = false;
        return false;
    }

    if (ret == 0)
    {
        online = false;
        ret = linkg_discovery_node_online(node_id, &online);
        if (ret == 0 && online)
        {
            route->waiting_for_peer = false;
            return false;
        }
    }

    route->waiting_for_peer = true;
    route->retry_at_ms      = now_ms + LINKG_UART_PEER_CHECK_MS;
    route->retry_delay_ms   = LINKG_UART_RETRY_INITIAL_MS;
    return true;
}

/****************************** 状态维护 ******************************/

/**
 * @brief 失败后进入有上限的指数退避重连。
 */
static void _uart_route_schedule_retry(linkg_uart_route_t *route, int error)
{
    uint32_t retry_delay_ms;

    _uart_route_close_fd(&route->client_fd);
    _uart_route_close_fd(&route->fd);

    _uart_route_discard_tcp(route);
    route->state             = LINKG_UART_ROUTE_IDLE;
    route->sbus_stream_count = 0U;
    route->peer_valid        = false;

    if (route->retry_delay_ms == 0U)
    {
        route->retry_delay_ms = LINKG_UART_RETRY_INITIAL_MS;
    }

    retry_delay_ms     = route->retry_delay_ms;
    route->retry_at_ms = uart_now_ms() + retry_delay_ms;
    if (route->retry_delay_ms < LINKG_UART_RETRY_MAX_MS)
    {
        route->retry_delay_ms *= 2U;
        if (route->retry_delay_ms > LINKG_UART_RETRY_MAX_MS)
        {
            route->retry_delay_ms = LINKG_UART_RETRY_MAX_MS;
        }
    }

    atomic_fetch_add(&route->parent->stats.errors, 1U);
    LINKG_LOG_WARN("UART: route uart=%u proto=%u port=%u error=%d retry_ms=%u", route->parent->config->uart_num,
                   (unsigned)route->config->proto, route->config->port, error, retry_delay_ms);
}

/**
 * @brief 服务端客户端断开时保留监听Socket。
 */
static void _uart_route_disconnect_client(linkg_uart_route_t *route)
{
    _uart_route_close_fd(&route->client_fd);
    _uart_route_discard_tcp(route);
    route->sbus_stream_count = 0U;
    route->state             = LINKG_UART_ROUTE_LISTENING;
    LINKG_LOG_INFO("UART: TCP client disconnected uart=%u port=%u", route->parent->config->uart_num, route->config->port);
}

/**
 * @brief 初始化一条路由运行态，config引用归Manager持有。
 */
void uart_route_init(linkg_uart_route_t *route, linkg_uart_port_t *port, const linkg_uart_route_config_t *config)
{
    memset(route, 0, sizeof(*route));
    route->parent         = port;
    route->config         = config;
    route->fd             = -1;
    route->client_fd      = -1;
    route->state          = LINKG_UART_ROUTE_IDLE;
    route->retry_delay_ms = LINKG_UART_RETRY_INITIAL_MS;
    route->remote         = _uart_route_sockaddr(config);
}

/**
 * @brief 关闭路由全部描述符。
 */
void uart_route_close(linkg_uart_route_t *route)
{
    if (route == NULL)
    {
        return;
    }

    _uart_route_close_fd(&route->client_fd);
    _uart_route_close_fd(&route->fd);
    _uart_route_discard_tcp(route);
    route->sbus_stream_count = 0U;
    route->peer_valid        = false;
    route->waiting_for_peer  = false;
    route->state             = LINKG_UART_ROUTE_IDLE;
}

/**
 * @brief 通过非阻塞Socket建立UDP、TCP监听或TCP客户端连接。
 */
static int _uart_route_open(linkg_uart_route_t *route)
{
    const linkg_uart_route_config_t *config;
    struct sockaddr_in               address;
    int                              type;
    int                              reuse;
    int                              ret;
    int                              fd;

    config  = route->config;
    type    = config->proto == LINKG_UART_ROUTE_PROTO_TCP ? SOCK_STREAM : SOCK_DGRAM;
    fd      = _uart_route_socket(type);
    if (fd < 0)
    {
        return fd;
    }

    address = _uart_route_sockaddr(config);
    if (config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL)
    {
        if (type == SOCK_STREAM)
        {
            reuse = 1;
            (void)setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
        }

        ret = bind(fd, (const struct sockaddr *)&address, sizeof(address));
        if (ret < 0)
        {
            ret = -errno;
            close(fd);
            return ret;
        }

        if (type == SOCK_STREAM && listen(fd, 4) < 0)
        {
            ret = -errno;
            close(fd);
            return ret;
        }

        route->state = type == SOCK_STREAM ? LINKG_UART_ROUTE_LISTENING : LINKG_UART_ROUTE_READY;
    }
    else if (type == SOCK_STREAM)
    {
        ret = connect(fd, (const struct sockaddr *)&address, sizeof(address));
        if (ret == 0)
        {
            route->state = LINKG_UART_ROUTE_READY;
        }
        else if (errno == EINPROGRESS)
        {
            route->state = LINKG_UART_ROUTE_CONNECTING;
        }
        else
        {
            ret = -errno;
            close(fd);
            return ret;
        }
    }
    else
    {
        // UDP remote通过connect锁定对端；local接收并学习最近来源。
        ret = connect(fd, (const struct sockaddr *)&address, sizeof(address));
        if (ret < 0)
        {
            ret = -errno;
            close(fd);
            return ret;
        }
        route->state = LINKG_UART_ROUTE_READY;
    }

    route->fd             = fd;
    route->retry_delay_ms = LINKG_UART_RETRY_INITIAL_MS;
    return 0;
}

/**
 * @brief 周期重建失效的网络路由。
 */
void uart_route_maintenance(linkg_uart_route_t *route, uint64_t now_ms)
{
    int ret;

    if (route == NULL || route->state != LINKG_UART_ROUTE_IDLE || now_ms < route->retry_at_ms)
    {
        return;
    }

    if (_uart_route_wait_for_peer(route, now_ms))
    {
        return;
    }

    ret = _uart_route_open(route);
    if (ret != 0)
    {
        _uart_route_schedule_retry(route, ret);
    }
    else
    {
        LINKG_LOG_INFO("UART: route ready uart=%u proto=%s endpoint=%s port=%u", route->parent->config->uart_num,
                       route->config->proto == LINKG_UART_ROUTE_PROTO_TCP ? "tcp" : "udp",
                       route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL ? "local" : "remote", route->config->port);
    }
}

/****************************** 网络发送 ******************************/

/**
 * @brief 将TCP发送队列尽可能写入网络，必须保留部分发送偏移。
 */
static void _uart_route_tcp_flush(linkg_uart_route_t *route)
{
    int  fd;
    int  n;

    if (route->state != LINKG_UART_ROUTE_READY || route->tcp_tx_count == 0U)
    {
        return;
    }

    fd = route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL ? route->client_fd : route->fd;
    if (fd < 0)
    {
        return;
    }

    while (route->tcp_tx_count > 0U)
    {
        n = (int)send(fd, route->tcp_tx, route->tcp_tx_count, MSG_NOSIGNAL | MSG_DONTWAIT);
        if (n > 0)
        {
            route->tcp_tx_count -= (uint16_t)n;
            memmove(route->tcp_tx, route->tcp_tx + n, route->tcp_tx_count);
            atomic_fetch_add(&route->parent->stats.net_tx_bytes, (uint64_t)n);
            continue;
        }

        if (n < 0 && errno == EINTR)
        {
            continue;
        }

        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            return;
        }

        if (route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL)
        {
            _uart_route_disconnect_client(route);
        }
        else
        {
            _uart_route_schedule_retry(route, n == 0 ? -EPIPE : -errno);
        }
        return;
    }
}

/**
 * @brief UART到网络；TCP保证部分写入顺序，UDP直接尽力发送。
 */
void uart_route_send(linkg_uart_route_t *route, const uint8_t *data, uint32_t length)
{
    int                       n;
    int                       fd;
    const struct sockaddr_in *destination;

    if (route == NULL || data == NULL || length == 0U)
    {
        return;
    }

    if (route->config->dir == LINKG_UART_ROUTE_DIR_RX)
    {
        return;
    }
    if (route->state != LINKG_UART_ROUTE_READY)
    {
        atomic_fetch_add(&route->parent->stats.drops, 1U);
        return;
    }

    if (route->config->proto == LINKG_UART_ROUTE_PROTO_TCP)
    {
        if (length > LINKG_UART_TCP_TX_CAPACITY - route->tcp_tx_count)
        {
            atomic_fetch_add(&route->parent->stats.drops, 1U);
            return;
        }

        memcpy(route->tcp_tx + route->tcp_tx_count, data, length);
        route->tcp_tx_count += (uint16_t)length;
        _uart_route_tcp_flush(route);
        return;
    }

    fd = route->fd;

    destination = route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL && route->peer_valid ? &route->peer : NULL;
    if (fd < 0 || (route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL && destination == NULL))
    {
        atomic_fetch_add(&route->parent->stats.drops, 1U);
        return;
    }

    if (destination != NULL)
    {
        n = (int)sendto(fd, data, length, MSG_DONTWAIT | MSG_NOSIGNAL, (const struct sockaddr *)destination, sizeof(*destination));
    }
    else
    {
        n = (int)send(fd, data, length, MSG_DONTWAIT | MSG_NOSIGNAL);
    }

    if (n == (int)length)
    {
        atomic_fetch_add(&route->parent->stats.net_tx_bytes, length);
    }
    else
    {
        atomic_fetch_add(&route->parent->stats.drops, 1U);
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS)
        {
            _uart_route_schedule_retry(route, -errno);
        }
    }
}

/**
 * @brief 一段串口数据分别发送给全部允许TX的网络路由。
 */
void uart_route_forward_serial(linkg_uart_port_t *port, const uint8_t *data, uint32_t length)
{
    uint32_t index;

    if (port == NULL || data == NULL || length == 0U)
    {
        return;
    }

    for (index = 0U; index < port->route_count; index++)
    {
        uart_route_send(&port->routes[index], data, length);
    }
}

/****************************** poll事件 ******************************/

/**
 * @brief 选择路由fd需要监听的事件。
 */
short uart_route_poll_events(const linkg_uart_route_t *route, bool client)
{
    short events;

    if (route == NULL)
    {
        return 0;
    }

    if (client)
    {
        events = route->config->dir == LINKG_UART_ROUTE_DIR_TX ? 0 : POLLIN;
        if (route->tcp_tx_count > 0U)
        {
            events |= POLLOUT;
        }
        return events;
    }

    if (route->state == LINKG_UART_ROUTE_CONNECTING)
    {
        return POLLOUT;
    }

    if (route->config->proto == LINKG_UART_ROUTE_PROTO_TCP && route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL)
    {
        return POLLIN; // TCP监听套接字始终处理新的连接
    }

    events = route->config->dir != LINKG_UART_ROUTE_DIR_TX ||
             (route->config->proto == LINKG_UART_ROUTE_PROTO_UDP &&
              route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL) ? POLLIN : 0;
    if (route->config->proto == LINKG_UART_ROUTE_PROTO_TCP && route->tcp_tx_count > 0U)
    {
        events |= POLLOUT;
    }
    return events;
}

/**
 * @brief 处理TCP服务端accept；只允许一个当前客户端。
 */
static void _uart_route_accept(linkg_uart_route_t *route)
{
    int fd;

    fd = accept4(route->fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (fd < 0)
    {
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
        {
            _uart_route_schedule_retry(route, -errno);
        }
        return;
    }

    if (route->client_fd >= 0)
    {
        _uart_route_close_fd(&route->client_fd);
        _uart_route_discard_tcp(route);
        route->sbus_stream_count = 0U;
    }

    route->client_fd = fd;
    route->state     = LINKG_UART_ROUTE_READY;
    LINKG_LOG_INFO("UART: accepted TCP client uart=%u port=%u", route->parent->config->uart_num, route->config->port);
}

/**
 * @brief 检查TCP非阻塞连接是否成功。
 */
static void _uart_route_connect_finish(linkg_uart_route_t *route)
{
    socklen_t length;
    int       error;

    error  = 0;
    length = sizeof(error);
    if (getsockopt(route->fd, SOL_SOCKET, SO_ERROR, &error, &length) < 0)
    {
        _uart_route_schedule_retry(route, -errno);
        return;
    }

    if (error != 0)
    {
        _uart_route_schedule_retry(route, -error);
        return;
    }

    route->state          = LINKG_UART_ROUTE_READY;
    route->retry_delay_ms = LINKG_UART_RETRY_INITIAL_MS;
    LINKG_LOG_INFO("UART: TCP connected uart=%u port=%u", route->parent->config->uart_num, route->config->port);
}

/**
 * @brief 将收到的网络数据投递到普通串口队列或SBUS真实帧FIFO。
 */
static void _uart_route_deliver(linkg_uart_route_t *route, const uint8_t *data, uint32_t length, bool stream)
{
    linkg_uart_port_t *port;

    port = route->parent;
    atomic_fetch_add(&port->stats.net_rx_bytes, length);

    if (port->config->port_config.type == LINKG_UART_PORT_TYPE_SBUS)
    {
        uart_sbus_input_network(port, data, length, stream, route);
    }
    else if (uart_port_raw_enqueue(port, data, length) != 0)
    {
        LINKG_LOG_WARN("UART: UART%u TX queue full, drop=%u", port->config->uart_num, length);
    }
}

/**
 * @brief 读取非阻塞Socket；UDP按数据报、TCP按字节流交付。
 */
static void _uart_route_receive(linkg_uart_route_t *route, int fd)
{
    uint8_t             buffer[LINKG_UART_IO_SIZE];
    struct sockaddr_in  peer;
    socklen_t           peer_length;
    bool                udp;
    int                 n;

    udp = route->config->proto == LINKG_UART_ROUTE_PROTO_UDP;

    peer_length = sizeof(peer);

    if (udp)
    {
        n = (int)recvfrom(fd, buffer, sizeof(buffer), MSG_DONTWAIT | MSG_TRUNC, (struct sockaddr *)&peer, &peer_length);
    }
    else
    {
        n = (int)recv(fd, buffer, sizeof(buffer), MSG_DONTWAIT);
    }

    if (n > 0 && (uint32_t)n > sizeof(buffer))
    {
        atomic_fetch_add(&route->parent->stats.drops, 1U);
        LINKG_LOG_WARN("UART: oversized UDP datagram uart=%u size=%d", route->parent->config->uart_num, n);
        return;
    }

    if (n > 0)
    {
        if (udp && route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL)
        {
            route->peer       = peer;
            route->peer_valid = true;
        }
        if (route->config->dir != LINKG_UART_ROUTE_DIR_TX)
        {
            _uart_route_deliver(route, buffer, (uint32_t)n, !udp);
        }
        return;
    }

    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR))
    {
        return;
    }

    if (udp)
    {
        if (n < 0)
        {
            _uart_route_schedule_retry(route, -errno);
        }
        return;
    }

    if (route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL)
    {
        _uart_route_disconnect_client(route);
    }
    else
    {
        _uart_route_schedule_retry(route, n == 0 ? -ECONNRESET : -errno);
    }
}

/**
 * @brief 根据poll事件驱动TCP/UDP状态机。
 */
void uart_route_event(linkg_uart_route_t *route, bool client, short revents)
{
    int fd;

    if (route == NULL || revents == 0)
    {
        return;
    }

    fd = client ? route->client_fd : route->fd;
    if (fd < 0)
    {
        return;
    }

    if (route->state == LINKG_UART_ROUTE_CONNECTING && !client)
    {
        _uart_route_connect_finish(route);
        return;
    }

    if (!client && route->config->proto == LINKG_UART_ROUTE_PROTO_TCP &&
        route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL)
    {
        if ((revents & POLLIN) != 0)
        {
            _uart_route_accept(route);
        }
        else if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            _uart_route_schedule_retry(route, -EIO);
        }
        return;
    }

    // TCP关闭事件可能携带最后一批可读字节；逐轮读到EOF后再断开。
    if (route->config->proto == LINKG_UART_ROUTE_PROTO_TCP &&
        (revents & POLLIN) != 0 && (revents & POLLNVAL) == 0)
    {
        _uart_route_receive(route, fd);
        if ((revents & POLLOUT) != 0)
        {
            _uart_route_tcp_flush(route);
        }
        return;
    }

    if ((revents & (POLLERR | POLLNVAL | POLLHUP)) != 0)
    {
        if (route->config->proto == LINKG_UART_ROUTE_PROTO_TCP)
        {
            if (client)
            {
                _uart_route_disconnect_client(route);
            }
            else
            {
                _uart_route_schedule_retry(route, -EIO);
            }
        }
        else
        {
            _uart_route_schedule_retry(route, -EIO);
        }
        return;
    }

    if ((revents & POLLIN) != 0)
    {
        _uart_route_receive(route, fd);
    }
    if ((revents & POLLOUT) != 0)
    {
        _uart_route_tcp_flush(route);
    }
}
