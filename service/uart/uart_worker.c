/**
 * @file uart_worker.c
 * @brief LinkG串口与Socket统一poll事件调度
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#include "uart_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_log.h"

/****************************** 事件列表 ******************************/

/**
 * @brief 在容量受限的poll列表中追加一项。
 */
static bool _uart_worker_add(struct pollfd *fds, linkg_uart_event_t *events, uint32_t *count, int fd, short flags, linkg_uart_event_kind_t kind, void *owner)
{
    if (fd < 0 || *count >= LINKG_UART_MAX_FDS)
    {
        return false;
    }

    fds[*count].fd      = fd;
    fds[*count].events  = flags;
    fds[*count].revents = 0;
    events[*count].kind = kind;
    events[*count].owner = owner;
    (*count)++;
    return true;
}

/**
 * @brief 每轮重建全部fd，覆盖重连、串口重开和TCP客户端替换。
 */
static uint32_t _uart_worker_build(linkg_uart_manager_context_t *manager, linkg_thread_t *thread, struct pollfd *fds,
                                   linkg_uart_event_t *events)
{
    linkg_uart_port_t  *port;
    linkg_uart_route_t *route;
    uint32_t            count;
    uint32_t            i;
    uint32_t            j;
    short               flags;

    count = 0U;
    (void)_uart_worker_add(fds, events, &count, linkg_thread_get_wakeup_fd(thread), POLLIN, LINKG_UART_EVENT_WAKEUP, NULL);

    for (i = 0U; i < manager->port_count; i++)
    {
        port = &manager->ports[i];
        if (port->fd >= 0)
        {
            flags = POLLIN;
            if (port->config->port_config.type == LINKG_UART_PORT_TYPE_NORMAL && port->raw_tx.count > 0U)
            {
                flags |= POLLOUT;
            }
            (void)_uart_worker_add(fds, events, &count, port->fd, flags, LINKG_UART_EVENT_PORT, port);
        }

        for (j = 0U; j < port->route_count; j++)
        {
            route = &port->routes[j];
            if (route->fd >= 0)
            {
                (void)_uart_worker_add(fds, events, &count, route->fd, uart_route_poll_events(route, false), LINKG_UART_EVENT_ROUTE, route);
            }
            if (route->client_fd >= 0)
            {
                (void)_uart_worker_add(fds, events, &count, route->client_fd, uart_route_poll_events(route, true), LINKG_UART_EVENT_CLIENT, route);
            }
        }
    }

    return count;
}

/****************************** 串口接收 ******************************/

/**
 * @brief 读取有限轮次的物理UART数据，避免一个设备长期占用Worker。
 */
static void _uart_worker_port_receive(linkg_uart_port_t *port)
{
    uint8_t  buffer[LINKG_UART_IO_SIZE];
    uint32_t rounds;
    int      n;

    for (rounds = 0U; rounds < 4U; rounds++)
    {
        n = uart_port_read(port, buffer, sizeof(buffer));
        if (n <= 0)
        {
            return;
        }

        atomic_fetch_add(&port->stats.uart_rx_bytes, (uint64_t)n);
        uart_debug_data(port, "UART-RX", buffer, (uint32_t)n);

        if (port->config->port_config.type == LINKG_UART_PORT_TYPE_SBUS)
        {
            uart_sbus_input_serial(port, buffer, (uint32_t)n);
        }
        else
        {
            uart_route_forward_serial(port, buffer, (uint32_t)n);
        }
    }
}

/****************************** 事件分发 ******************************/

/**
 * @brief 处理单个已就绪fd，fd一致性检查防止TCP客户端替换后误用旧事件。
 */
static void _uart_worker_dispatch(linkg_thread_t *thread, const struct pollfd *fd, const linkg_uart_event_t *event)
{
    linkg_uart_port_t  *port;
    linkg_uart_route_t *route;

    switch (event->kind)
    {
        case LINKG_UART_EVENT_WAKEUP:
            (void)linkg_thread_clear_wakeup(thread);
            return;

        case LINKG_UART_EVENT_PORT:
            port = (linkg_uart_port_t *)event->owner;
            if (port->fd != fd->fd)
            {
                return;
            }
            if ((fd->revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
            {
                uart_port_fault(port, -EIO);
                return;
            }
            if ((fd->revents & POLLIN) != 0)
            {
                _uart_worker_port_receive(port);
            }
            if ((fd->revents & POLLOUT) != 0 && port->config->port_config.type == LINKG_UART_PORT_TYPE_NORMAL)
            {
                uart_port_raw_flush(port);
            }
            return;

        case LINKG_UART_EVENT_ROUTE:
        case LINKG_UART_EVENT_CLIENT:
            route = (linkg_uart_route_t *)event->owner;
            if ((event->kind == LINKG_UART_EVENT_CLIENT ? route->client_fd : route->fd) != fd->fd)
            {
                return;
            }
            uart_route_event(route, event->kind == LINKG_UART_EVENT_CLIENT, fd->revents);
            return;

        default:
            return;
    }
}

/****************************** Worker调度 ******************************/

/**
 * @brief 单Worker负责全部普通串口与TCP/UDP Socket的非阻塞调度。
 */
void uart_worker_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_uart_manager_context_t *manager;
    struct pollfd                fds[LINKG_UART_MAX_FDS];
    linkg_uart_event_t           events[LINKG_UART_MAX_FDS];
    linkg_uart_port_t           *port;
    uint32_t                     count;
    uint32_t                     i;
    uint32_t                     j;
    uint64_t                     now_ms;
    int                          ret;

    manager = (linkg_uart_manager_context_t *)user_data;
    if (manager == NULL)
    {
        return;
    }

    while (linkg_thread_is_running(thread))
    {
        now_ms = uart_now_ms();
        for (i = 0U; i < manager->port_count; i++)
        {
            port = &manager->ports[i];
            uart_port_maintenance(port, now_ms);
            for (j = 0U; j < port->route_count; j++)
            {
                uart_route_maintenance(&port->routes[j], now_ms);
            }
        }

        count = _uart_worker_build(manager, thread, fds, events);
        if (count == 0U)
        {
            LINKG_LOG_ERROR("UART: poll list empty, worker terminating");
            break;
        }

        do
        {
            ret = poll(fds, count, LINKG_UART_POLL_TIMEOUT_MS);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (!linkg_thread_is_running(thread))
        {
            break;
        }
        if (ret < 0)
        {
            LINKG_LOG_ERROR("UART: poll failed errno=%d", errno);
            break;
        }

        if (ret > 0)
        {
            for (i = 0U; i < count && linkg_thread_is_running(thread); i++)
            {
                if (fds[i].revents != 0)
                {
                    _uart_worker_dispatch(thread, &fds[i], &events[i]);
                }
            }
        }

        uart_stats_report(manager);
    }
}
