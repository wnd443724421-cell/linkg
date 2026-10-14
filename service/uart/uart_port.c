/**
 * @file uart_port.c
 * @brief LinkG物理串口打开、故障恢复和普通数据发送
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#include "uart_internal.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_system_resources.h"
#include "linkg_uart.h"

/****************************** 固定设备映射 ******************************/

/**
 * @brief 将业务逻辑串口号映射为板级设备节点。
 */
const char *uart_port_device_path(uint8_t uart_num)
{
    switch (uart_num)
    {
        case 0U:
            return LINKG_RESOURCE_UART0_DEVICE;

        case 1U:
            return LINKG_RESOURCE_UART1_DEVICE;

        case 2U:
            return LINKG_RESOURCE_UART2_DEVICE;

        default:
            return NULL;
    }
}

/****************************** 错误恢复 ******************************/

/**
 * @brief 标记串口故障并安排异步重开，不在调用线程立即关闭fd。
 */
void uart_port_fault(linkg_uart_port_t *port, int error)
{
    uint64_t now_ms;

    if (port == NULL)
    {
        return;
    }

    now_ms = uart_now_ms();

    pthread_mutex_lock(&port->io_lock);

    if (port->fd >= 0)
    {
        linux_uart_close(port->fd);
        port->fd = -1;
        atomic_fetch_add(&port->generation, 1U);
        atomic_fetch_add(&port->stats.errors, 1U);
        LINKG_LOG_WARN("UART: uart=%u device=%s fault=%d, will reopen", port->config->uart_num, port->device_path, error);
    }

    if (port->retry_delay_ms == 0U)
    {
        port->retry_delay_ms = LINKG_UART_RETRY_INITIAL_MS;
    }

    port->retry_at_ms = now_ms + port->retry_delay_ms;
    if (port->retry_delay_ms < LINKG_UART_RETRY_MAX_MS)
    {
        port->retry_delay_ms *= 2U;
        if (port->retry_delay_ms > LINKG_UART_RETRY_MAX_MS)
        {
            port->retry_delay_ms = LINKG_UART_RETRY_MAX_MS;
        }
    }

    pthread_mutex_unlock(&port->io_lock);
}

/****************************** 物理设备 ******************************/

/**
 * @brief 使用已校验的配置打开串口。
 */
int uart_port_open(linkg_uart_port_t *port)
{
    const linkg_uart_port_config_t *config;
    uart_config_t                   physical;
    int                             fd;

    if (port == NULL || port->config == NULL || port->device_path == NULL)
    {
        return -EINVAL;
    }

    config = &port->config->port_config;

    memset(&physical, 0, sizeof(physical));
    physical.baudrate        = (int)config->baudrate;
    physical.data_bits       = (int)config->data_bits;
    physical.stop_bits       = (int)config->stop_bits;
    physical.parity          = config->parity;
    physical.hw_flow_control = false;
    physical.exclusive       = true;

    pthread_mutex_lock(&port->io_lock);

    if (port->fd >= 0)
    {
        pthread_mutex_unlock(&port->io_lock);
        return 0;
    }

    fd = linux_uart_open(port->device_path, &physical);
    if (fd >= 0)
    {
        port->fd = fd;
        atomic_fetch_add(&port->generation, 1U);
        port->retry_delay_ms = LINKG_UART_RETRY_INITIAL_MS;
        port->retry_at_ms    = 0U;
        port->raw_tx.head    = 0U;
        port->raw_tx.count   = 0U;
    }

    pthread_mutex_unlock(&port->io_lock);

    if (fd < 0)
    {
        return fd;
    }

    if (config->type == LINKG_UART_PORT_TYPE_SBUS)
    {
        uart_sbus_reset(port);
    }

    LINKG_LOG_INFO("UART: opened logical=%u path=%s baud=%u", port->config->uart_num, port->device_path, config->baudrate);
    return 0;
}

/**
 * @brief 关闭串口设备。调用前必须停止SBUS输出线程和Worker。
 */
void uart_port_close(linkg_uart_port_t *port)
{
    if (port == NULL)
    {
        return;
    }

    pthread_mutex_lock(&port->io_lock);

    if (port->fd >= 0)
    {
        linux_uart_close(port->fd);
        port->fd = -1;
    }

    pthread_mutex_unlock(&port->io_lock);
}

/**
 * @brief 仅由Worker周期调用，避免在I/O错误回调中执行耗时重开。
 */
void uart_port_maintenance(linkg_uart_port_t *port, uint64_t now_ms)
{
    int ret;

    if (port == NULL || port->fd >= 0 || now_ms < port->retry_at_ms)
    {
        return;
    }

    ret = uart_port_open(port);
    if (ret == 0)
    {
        return;
    }

    atomic_fetch_add(&port->stats.errors, 1U);
    LINKG_LOG_WARN("UART: open retry logical=%u failed error=%d", port->config->uart_num, ret);

    port->retry_at_ms = now_ms + (port->retry_delay_ms != 0U ? port->retry_delay_ms : LINKG_UART_RETRY_INITIAL_MS);
    if (port->retry_delay_ms < LINKG_UART_RETRY_MAX_MS)
    {
        port->retry_delay_ms = port->retry_delay_ms == 0U ? LINKG_UART_RETRY_INITIAL_MS : port->retry_delay_ms * 2U;
        if (port->retry_delay_ms > LINKG_UART_RETRY_MAX_MS)
        {
            port->retry_delay_ms = LINKG_UART_RETRY_MAX_MS;
        }
    }
}

/****************************** 串口收发 ******************************/

/**
 * @brief 读取物理串口，返回读取字节数或负errno。
 */
int uart_port_read(linkg_uart_port_t *port, uint8_t *buffer, uint32_t capacity)
{
    int ret;

    if (port == NULL || buffer == NULL || capacity == 0U)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&port->io_lock);
    ret = port->fd >= 0 ? linux_uart_read(port->fd, buffer, (int)capacity) : -ENODEV;
    pthread_mutex_unlock(&port->io_lock);

    if (ret < 0 && ret != -ENODEV)
    {
        uart_port_fault(port, ret);
    }

    return ret;
}

/**
 * @brief 单次非阻塞写入并返回真实提交字节数。
 * @note 这里直接使用write，绕过当前linux_uart_write在部分写入后遇EAGAIN会丢失进度的问题。
 */
int uart_port_write(linkg_uart_port_t *port, const uint8_t *buffer, uint32_t length)
{
    ssize_t written;
    int     error;

    if (port == NULL || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&port->io_lock);
    if (port->fd < 0)
    {
        pthread_mutex_unlock(&port->io_lock);
        return -ENODEV;
    }

    do
    {
        written = write(port->fd, buffer, length);
    }
    while (written < 0 && errno == EINTR);

    error = written < 0 ? errno : 0;
    pthread_mutex_unlock(&port->io_lock);

    if (written > 0)
    {
        atomic_fetch_add(&port->stats.uart_tx_bytes, (uint64_t)written);
        uart_debug_data(port, "UART-TX", buffer, (uint32_t)written);
        return (int)written;
    }

    if (written == 0 || error == EAGAIN || error == EWOULDBLOCK)
    {
        return -EAGAIN;
    }

    uart_port_fault(port, -error);
    return -error;
}

/****************************** 普通串口发送缓存 ******************************/

/**
 * @brief 将整个网络输入片段顺序加入串口环形发送队列。
 */
int uart_port_raw_enqueue(linkg_uart_port_t *port, const uint8_t *buffer, uint32_t length)
{
    linkg_uart_raw_queue_t *queue;
    uint32_t                tail;
    uint32_t                first;

    if (port == NULL || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    queue = &port->raw_tx;
    if (length > LINKG_UART_RAW_TX_CAPACITY - queue->count)
    {
        atomic_fetch_add(&port->stats.drops, 1U);
        return -ENOBUFS;
    }

    tail  = ((uint32_t)queue->head + queue->count) % LINKG_UART_RAW_TX_CAPACITY;
    first = LINKG_UART_RAW_TX_CAPACITY - tail;
    if (first > length)
    {
        first = length;
    }

    memcpy(queue->bytes + tail, buffer, first);
    memcpy(queue->bytes, buffer + first, length - first);
    queue->count += (uint16_t)length;
    return 0;
}

/**
 * @brief 由Worker在POLLOUT事件上补发部分写入的普通串口字节。
 */
void uart_port_raw_flush(linkg_uart_port_t *port)
{
    linkg_uart_raw_queue_t *queue;
    uint32_t                budget;
    uint32_t                length;
    int                     written;

    if (port == NULL || port->fd < 0)
    {
        return;
    }

    queue  = &port->raw_tx;
    budget = LINKG_UART_RAW_TX_CAPACITY;

    while (queue->count > 0U && budget > 0U)
    {
        length = LINKG_UART_RAW_TX_CAPACITY - queue->head;
        if (length > queue->count)
        {
            length = queue->count;
        }
        if (length > budget)
        {
            length = budget;
        }

        written = uart_port_write(port, queue->bytes + queue->head, length);
        if (written <= 0)
        {
            break;
        }

        queue->head  = (uint16_t)(((uint32_t)queue->head + (uint32_t)written) % LINKG_UART_RAW_TX_CAPACITY);
        queue->count -= (uint16_t)written;
        budget       -= (uint32_t)written;
    }
}
