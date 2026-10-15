/**
 * @file linkg_uart_manager.c
 * @brief LinkG串口服务配置快照、GPIO控制与资源生命周期
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#include "linkg_uart_manager.h"

#include <errno.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_gpio_manager.h"
#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "uart_internal.h"

/****************************** 运行上下文 ******************************/

linkg_uart_manager_context_t g_uart_manager =
{
    .life_lock = PTHREAD_MUTEX_INITIALIZER,
    .state     = LINKG_UART_LIFE_UNINITIALIZED,
    .sbus_target_level = LINKG_UART_SBUS_WIFI_LEVEL
};

/****************************** 内部清理 ******************************/

/**
 * @brief 销毁已经创建的端口锁与SBUS线程对象。
 */
static void _uart_manager_release_ports(void)
{
    linkg_uart_port_t *port;
    uint32_t           index;

    for (index = 0U; index < g_uart_manager.port_count; index++)
    {
        port = &g_uart_manager.ports[index];
        if (port->config->port_config.type == LINKG_UART_PORT_TYPE_SBUS)
        {
            linkg_thread_deinit(&port->sbus_thread);
        }
        pthread_mutex_destroy(&port->sbus_lock);
        pthread_mutex_destroy(&port->io_lock);
    }

    g_uart_manager.port_count = 0U;
}

/**
 * @brief 关闭网络路由；只在所有工作线程退出后调用。
 */
static void _uart_manager_close_routes(void)
{
    linkg_uart_port_t *port;
    uint32_t           i;
    uint32_t           j;

    for (i = 0U; i < g_uart_manager.port_count; i++)
    {
        port = &g_uart_manager.ports[i];
        for (j = 0U; j < port->route_count; j++)
        {
            uart_route_close(&port->routes[j]);
        }
        uart_port_close(port);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化串口服务快照、端口上下文和线程对象，不打开设备。
 */
int uart_manager_init(void)
{
    const linkg_uart_item_config_t *item;
    linkg_uart_port_t              *port;
    uint32_t                        i;
    uint32_t                        j;
    int                             ret;

    pthread_mutex_lock(&g_uart_manager.life_lock);
    if (g_uart_manager.state != LINKG_UART_LIFE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return -EALREADY;
    }

    memset(&g_uart_manager.config, 0, sizeof(g_uart_manager.config));
    memset(g_uart_manager.ports, 0, sizeof(g_uart_manager.ports));
    g_uart_manager.port_count = 0U;

    ret = linkg_config_get_uart(&g_uart_manager.config);
    if (ret != 0 || linkg_uart_config_validate(&g_uart_manager.config) != 0)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return -EINVAL;
    }

    for (i = 0U; i < g_uart_manager.config.count; i++)
    {
        item = &g_uart_manager.config.items[i];
        if (item->route_count == 0U)
        {
            continue;
        }

        port = &g_uart_manager.ports[g_uart_manager.port_count];
        port->config       = item;
        port->device_path  = uart_port_device_path(item->uart_num);
        port->fd           = -1;
        port->route_count  = item->route_count;

        if (port->device_path == NULL)
        {
            ret = -EINVAL;
            goto fail;
        }

        ret = pthread_mutex_init(&port->io_lock, NULL);
        if (ret != 0)
        {
            ret = -ret;
            goto fail;
        }

        ret = pthread_mutex_init(&port->sbus_lock, NULL);
        if (ret != 0)
        {
            pthread_mutex_destroy(&port->io_lock);
            ret = -ret;
            goto fail;
        }

        atomic_store(&port->sbus_interval_us, LINKG_UART_SBUS_INTERVAL_US);
        uart_stats_reset(port);

        if (item->port_config.type == LINKG_UART_PORT_TYPE_SBUS)
        {
            ret = linkg_thread_init(&port->sbus_thread, "linkg-sbus", uart_sbus_thread, port);
            if (ret != 0)
            {
                pthread_mutex_destroy(&port->sbus_lock);
                pthread_mutex_destroy(&port->io_lock);
                goto fail;
            }
        }

        for (j = 0U; j < item->route_count; j++)
        {
            uart_route_init(&port->routes[j], port, &item->routes[j]);
        }
        g_uart_manager.port_count++;
    }

    ret = linkg_thread_init(&g_uart_manager.worker, "linkg-uart", uart_worker_thread, &g_uart_manager);
    if (ret != 0)
    {
        goto fail;
    }

    g_uart_manager.state = LINKG_UART_LIFE_STOPPED;
    pthread_mutex_unlock(&g_uart_manager.life_lock);
    return 0;

fail:
    _uart_manager_release_ports();
    memset(&g_uart_manager.config, 0, sizeof(g_uart_manager.config));
    pthread_mutex_unlock(&g_uart_manager.life_lock);
    return ret;
}

/**
 * @brief 打开配置的串口和路由并启动统一Worker与SBUS定时线程。
 */
int uart_manager_start(bool strict)
{
    linkg_uart_port_t *port;
    bool              has_sbus;
    uint32_t          i;
    uint32_t          j;
    int               ret;
    int               cleanup_ret;

    pthread_mutex_lock(&g_uart_manager.life_lock);
    if (g_uart_manager.state != LINKG_UART_LIFE_STOPPED)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return -EBUSY;
    }

    if (g_uart_manager.port_count == 0U)
    {
        g_uart_manager.state = LINKG_UART_LIFE_RUNNING;
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        LINKG_LOG_INFO("UART: no active route, service started idle");
        return 0;
    }

    // UART RX使能是板级统一GPIO，只有实际启动串口服务时才开启。
    ret = linkg_gpio_manager_set_uart_rx_enabled(false);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return ret;
    }

    has_sbus = false;
    for (i = 0U; i < g_uart_manager.port_count; i++)
    {
        if (g_uart_manager.ports[i].config->port_config.type == LINKG_UART_PORT_TYPE_SBUS)
        {
            has_sbus = true;
        }
    }

    ret = linkg_gpio_manager_set_sbus_enabled(has_sbus);
    if (ret != 0)
    {
        (void)linkg_gpio_manager_set_uart_rx_enabled(false);
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return ret;
    }

    // MCU按1ms主循环采样模式；留出切换时间后才允许业务线程发送。
    (void)linkg_time_sleep_ms(5U);

    for (i = 0U; i < g_uart_manager.port_count; i++)
    {
        port = &g_uart_manager.ports[i];
        if (strict)
        {
            ret = uart_port_open(port);
            if (ret != 0)
            {
                goto fail;
            }
        }
        else
        {
            uart_port_maintenance(port, uart_now_ms());
        }
        for (j = 0U; j < port->route_count; j++)
        {
            uart_route_maintenance(&port->routes[j], uart_now_ms());
            if (strict && port->routes[j].config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL &&
                atomic_load(&port->routes[j].state) == LINKG_UART_ROUTE_IDLE)
            {
                ret = -EADDRINUSE;
                goto fail;
            }
        }
    }

    ret = linkg_gpio_manager_set_uart_rx_enabled(true);
    if (ret != 0)
    {
        goto fail;
    }

    g_uart_manager.state = LINKG_UART_LIFE_RUNNING;
    ret = linkg_thread_start(&g_uart_manager.worker);
    if (ret != 0)
    {
        goto fail;
    }

    for (i = 0U; i < g_uart_manager.port_count; i++)
    {
        port = &g_uart_manager.ports[i];
        if (port->config->port_config.type != LINKG_UART_PORT_TYPE_SBUS)
        {
            continue;
        }

        ret = linkg_thread_start(&port->sbus_thread);
        if (ret != 0)
        {
            goto fail;
        }
    }

    pthread_mutex_unlock(&g_uart_manager.life_lock);
    LINKG_LOG_INFO("UART: service started ports=%u", g_uart_manager.port_count);
    return 0;

fail:
    // 如果无法确认线程退出，严禁继续释放其引用的fd或配置快照。
    cleanup_ret = linkg_thread_stop(&g_uart_manager.worker);
    if (cleanup_ret != 0)
    {
        g_uart_manager.state = LINKG_UART_LIFE_STOPPING;
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return cleanup_ret;
    }

    for (i = 0U; i < g_uart_manager.port_count; i++)
    {
        port = &g_uart_manager.ports[i];
        if (port->config->port_config.type != LINKG_UART_PORT_TYPE_SBUS)
        {
            continue;
        }

        cleanup_ret = linkg_thread_stop(&port->sbus_thread);
        if (cleanup_ret != 0)
        {
            g_uart_manager.state = LINKG_UART_LIFE_STOPPING;
            pthread_mutex_unlock(&g_uart_manager.life_lock);
            return cleanup_ret;
        }
    }
    _uart_manager_close_routes();
    (void)linkg_gpio_manager_set_uart_rx_enabled(false);
    (void)linkg_gpio_manager_set_sbus_enabled(false);
    g_uart_manager.state = LINKG_UART_LIFE_STOPPED;
    pthread_mutex_unlock(&g_uart_manager.life_lock);
    return ret;
}

/**
 * @brief 停止Worker和SBUS线程，关闭Socket与物理串口。
 */
int uart_manager_stop(void)
{
    linkg_uart_port_t *port;
    uint32_t           i;
    int                ret;
    int                first_error;

    pthread_mutex_lock(&g_uart_manager.life_lock);
    if (g_uart_manager.state == LINKG_UART_LIFE_STOPPED || g_uart_manager.state == LINKG_UART_LIFE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return 0;
    }
    if (g_uart_manager.state != LINKG_UART_LIFE_RUNNING && g_uart_manager.state != LINKG_UART_LIFE_STOPPING)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return -EBUSY;
    }

    g_uart_manager.state = LINKG_UART_LIFE_STOPPING;
    first_error = 0;

    if (g_uart_manager.port_count > 0U)
    {
        ret = linkg_thread_stop(&g_uart_manager.worker);
        if (ret != 0)
        {
            g_uart_manager.state = LINKG_UART_LIFE_STOPPING;
            pthread_mutex_unlock(&g_uart_manager.life_lock);
            return ret;
        }

        for (i = 0U; i < g_uart_manager.port_count; i++)
        {
            port = &g_uart_manager.ports[i];
            if (port->config->port_config.type != LINKG_UART_PORT_TYPE_SBUS)
            {
                continue;
            }
            ret = linkg_thread_stop(&port->sbus_thread);
            if (ret != 0)
            {
                g_uart_manager.state = LINKG_UART_LIFE_STOPPING;
                pthread_mutex_unlock(&g_uart_manager.life_lock);
                return ret;
            }
        }

        _uart_manager_close_routes();
        ret = linkg_gpio_manager_set_uart_rx_enabled(false);
        if (ret != 0)
        {
            first_error = ret;
        }
        ret = linkg_gpio_manager_set_sbus_enabled(false);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    g_uart_manager.state = LINKG_UART_LIFE_STOPPED;
    pthread_mutex_unlock(&g_uart_manager.life_lock);
    return first_error;
}

/**
 * @brief 释放全部对象，要求业务线程已经停止。
 */
int uart_manager_deinit(void)
{
    pthread_mutex_lock(&g_uart_manager.life_lock);
    if (g_uart_manager.state == LINKG_UART_LIFE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return 0;
    }
    if (g_uart_manager.state != LINKG_UART_LIFE_STOPPED)
    {
        pthread_mutex_unlock(&g_uart_manager.life_lock);
        return -EBUSY;
    }

    linkg_thread_deinit(&g_uart_manager.worker);
    _uart_manager_release_ports();
    memset(&g_uart_manager.config, 0, sizeof(g_uart_manager.config));
    g_uart_manager.state = LINKG_UART_LIFE_UNINITIALIZED;
    pthread_mutex_unlock(&g_uart_manager.life_lock);
    return 0;
}

/****************************** 运行配置 ******************************/

/**
 * @brief 切换SBUS目标水位，不清空FIFO或重启定时线程。
 */
int linkg_uart_manager_set_sbus_profile(linkg_uart_sbus_profile_t profile)
{
    uint32_t target;

    if (profile != LINKG_UART_SBUS_PROFILE_WIFI && profile != LINKG_UART_SBUS_PROFILE_BACKUP)
    {
        return -EINVAL;
    }

    target = profile == LINKG_UART_SBUS_PROFILE_WIFI ? LINKG_UART_SBUS_WIFI_LEVEL : LINKG_UART_SBUS_BACKUP_LEVEL;
    atomic_store(&g_uart_manager.sbus_target_level, target);

    return 0;
}

/**
 * @brief 动态开关调试，避免等待重载线程退出。
 */
int linkg_uart_manager_set_debug(bool enabled)
{
    atomic_store(&g_uart_manager.debug_enabled, enabled);

    return 0;
}
