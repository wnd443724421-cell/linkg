/**
 * @file uart_status.c
 * @brief UART运行状态与线程安全计数采样
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include "uart_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_uart.h"

/****************************** 状态查询 ******************************/

/**
 * @brief 返回板级固定串口节点，Web不得自行推测tty编号。
 */
const char *linkg_uart_manager_device_path(uint8_t uart_num)
{
    return uart_port_device_path(uart_num);
}

/**
 * @brief 将网络路由状态转为稳定名称，UDP就绪不代表远端可达。
 */
static const char *_uart_status_route(linkg_uart_route_t *route)
{
    int state = atomic_load(&route->state);

    if (state == LINKG_UART_ROUTE_IDLE)
    {
        return "retrying";
    }
    if (state == LINKG_UART_ROUTE_CONNECTING)
    {
        return "connecting";
    }
    if (state == LINKG_UART_ROUTE_LISTENING)
    {
        return "listening";
    }
    if (route->config->proto == LINKG_UART_ROUTE_PROTO_UDP)
    {
        return route->config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL && !atomic_load(&route->peer_valid) ?
               "waiting_peer" : "udp_ready";
    }

    return "connected";
}

/**
 * @brief 读取生命周期及原子计数；重载持有生命周期锁时立即返回正在重载。
 *
 * 计数逐项原子采样，不声明多个计数属于同一瞬间。
 */
int linkg_uart_manager_get_status(linkg_uart_status_t *out)
{
    linkg_uart_apply_status_t apply;
    linkg_uart_port_t        *port;
    linkg_uart_port_status_t *status;
    uint32_t                  i;
    uint32_t                  j;

    if (out == NULL)
    {
        return -EINVAL;
    }

    memset(out, 0, sizeof(*out));
    linkg_uart_manager_get_apply_status(&apply);
    if (apply.state == LINKG_UART_APPLY_RUNNING || pthread_mutex_trylock(&g_uart_manager.life_lock) != 0)
    {
        out->state = "reloading";
        return 0;
    }

    switch (g_uart_manager.state)
    {
        case LINKG_UART_LIFE_UNINITIALIZED:
            out->state = "uninitialized";
            break;

        case LINKG_UART_LIFE_STOPPED:
            out->state = "stopped";
            break;

        case LINKG_UART_LIFE_STOPPING:
            out->state = "failed";
            break;

        default:
            out->state = "running";
            break;
    }

    out->worker_running = linkg_thread_is_running(&g_uart_manager.worker);
    out->port_count     = g_uart_manager.port_count;

    if (g_uart_manager.state == LINKG_UART_LIFE_RUNNING && out->port_count > 0U && !out->worker_running)
    {
        out->state = "failed";
    }

    for (i = 0U; i < out->port_count; i++)
    {
        port                  = &g_uart_manager.ports[i];
        status                = &out->ports[i];
        status->uart_num      = port->config->uart_num;
        status->device_path   = port->device_path;
        status->device_open   = atomic_load(&port->fd) >= 0;
        status->sbus_running  = linkg_thread_is_running(&port->sbus_thread);
        status->route_count   = port->route_count;
        status->uart_rx_bytes = atomic_load(&port->stats.uart_rx_bytes);
        status->uart_tx_bytes = atomic_load(&port->stats.uart_tx_bytes);
        status->net_rx_bytes  = atomic_load(&port->stats.net_rx_bytes);
        status->net_tx_bytes  = atomic_load(&port->stats.net_tx_bytes);
        status->errors        = atomic_load(&port->stats.errors);
        status->drops         = atomic_load(&port->stats.drops);

        for (j = 0U; j < status->route_count; j++)
        {
            status->routes[j] = _uart_status_route(&port->routes[j]);
        }

        if (pthread_mutex_trylock(&port->io_lock) == 0)
        {
            status->baud_verified = linux_uart_baud_info(atomic_load(&port->fd), port->config->port_config.baudrate, &status->baud) == 0;
            pthread_mutex_unlock(&port->io_lock);
        }

        if (g_uart_manager.state == LINKG_UART_LIFE_RUNNING &&
            strcmp(out->state, "failed") != 0 &&
            (!status->device_open || (port->config->port_config.type == LINKG_UART_PORT_TYPE_SBUS && !status->sbus_running)))
        {
            out->state = "not_ready";
        }
    }

    pthread_mutex_unlock(&g_uart_manager.life_lock);

    return 0;
}
