/**
 * @file uart_sbus.c
 * @brief LinkG SBUS真实帧解析、有限FIFO与绝对时间定时输出
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#define _POSIX_C_SOURCE 200809L

#include "uart_internal.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <string.h>
#include <time.h>

#include "linkg_log.h"

#define LINKG_UART_SBUS_RT_PRIORITY 75 // SBUS定时线程尽力使用FIFO调度

/****************************** SBUS帧识别 ******************************/

/**
 * @brief 最小协议完整性检查；不尝试推断SBUS帧序号。
 */
static bool _uart_sbus_valid(const uint8_t *frame)
{
    return frame != NULL && frame[0] == 0x0FU &&
           (frame[LINKG_UART_SBUS_FRAME_SIZE - 1U] == 0x00U ||
            frame[LINKG_UART_SBUS_FRAME_SIZE - 1U] == 0x04U ||
            frame[LINKG_UART_SBUS_FRAME_SIZE - 1U] == 0x14U);
}

/****************************** SBUS FIFO ******************************/

/**
 * @brief 入队真实SBUS帧，满时删除最旧的未发送帧。
 * @note 调用方必须持有sbus_lock。
 */
static void _uart_sbus_push_locked(linkg_uart_port_t *port, const uint8_t *frame)
{
    linkg_uart_sbus_fifo_t *fifo;
    uint32_t                index;

    fifo = &port->sbus_fifo;

    if (fifo->count == LINKG_UART_SBUS_FIFO_CAPACITY)
    {
        fifo->head = (uint8_t)((fifo->head + 1U) % LINKG_UART_SBUS_FIFO_CAPACITY);
        fifo->count--;
        atomic_fetch_add(&port->stats.drops, 1U);
    }

    index = ((uint32_t)fifo->head + fifo->count) % LINKG_UART_SBUS_FIFO_CAPACITY;
    memcpy(fifo->frames[index], frame, LINKG_UART_SBUS_FRAME_SIZE);
    fifo->count++;
    uart_stats_fifo_sample(port, fifo->count);
}

/**
 * @brief 弹出一帧真实数据。调用方必须持有sbus_lock。
 */
static bool _uart_sbus_pop_locked(linkg_uart_port_t *port, uint8_t *frame)
{
    linkg_uart_sbus_fifo_t *fifo;

    fifo = &port->sbus_fifo;
    if (fifo->count == 0U)
    {
        return false;
    }

    memcpy(frame, fifo->frames[fifo->head], LINKG_UART_SBUS_FRAME_SIZE);
    fifo->head = (uint8_t)((fifo->head + 1U) % LINKG_UART_SBUS_FIFO_CAPACITY);
    fifo->count--;
    uart_stats_fifo_sample(port, fifo->count);
    return true;
}

/**
 * @brief 串口重新打开后丢弃旧控制帧，避免恢复时发送过期数据。
 */
void uart_sbus_reset(linkg_uart_port_t *port)
{
    if (port == NULL)
    {
        return;
    }

    pthread_mutex_lock(&port->sbus_lock);
    atomic_fetch_add(&port->stats.drops, port->sbus_fifo.count);
    memset(&port->sbus_fifo, 0, sizeof(port->sbus_fifo));
    port->sbus_rx_count = 0U;
    pthread_mutex_unlock(&port->sbus_lock);

    atomic_store(&port->sbus_interval_us, LINKG_UART_SBUS_INTERVAL_US);
}

/****************************** SBUS串口输入 ******************************/

/**
 * @brief 将串口的任意分片拼接成真实25字节SBUS帧并转发网络。
 */
void uart_sbus_input_serial(linkg_uart_port_t *port, const uint8_t *data, uint32_t length)
{
    uint32_t index;

    if (port == NULL || data == NULL)
    {
        return;
    }

    for (index = 0U; index < length; index++)
    {
        if (port->sbus_rx_count == 0U && data[index] != 0x0FU)
        {
            continue;
        }

        port->sbus_rx[port->sbus_rx_count++] = data[index];
        if (port->sbus_rx_count != LINKG_UART_SBUS_FRAME_SIZE)
        {
            continue;
        }

        if (_uart_sbus_valid(port->sbus_rx))
        {
            atomic_fetch_add(&port->stats.sbus_rx_frames, 1U);
            uart_route_forward_serial(port, port->sbus_rx, LINKG_UART_SBUS_FRAME_SIZE);
            port->sbus_rx_count = 0U;
        }
        else
        {
            atomic_fetch_add(&port->stats.drops, 1U);
            memmove(port->sbus_rx, port->sbus_rx + 1, LINKG_UART_SBUS_FRAME_SIZE - 1U);
            port->sbus_rx_count = LINKG_UART_SBUS_FRAME_SIZE - 1U;
        }
    }
}

/****************************** SBUS网络输入 ******************************/

/**
 * @brief 将一条真实SBUS帧投递至调度FIFO。
 */
static void _uart_sbus_deliver_frame(linkg_uart_port_t *port, const uint8_t *frame)
{
    if (!_uart_sbus_valid(frame))
    {
        atomic_fetch_add(&port->stats.drops, 1U);
        return;
    }

    pthread_mutex_lock(&port->sbus_lock);
    _uart_sbus_push_locked(port, frame);
    pthread_mutex_unlock(&port->sbus_lock);
}

/**
 * @brief 接收网络侧SBUS；UDP必须整帧，TCP按流逐字节拼帧。
 */
void uart_sbus_input_network(linkg_uart_port_t *port, const uint8_t *data, uint32_t length, bool stream, linkg_uart_route_t *route)
{
    uint32_t index;

    if (port == NULL || data == NULL || length == 0U)
    {
        return;
    }

    if (!stream)
    {
        if (length % LINKG_UART_SBUS_FRAME_SIZE != 0U)
        {
            atomic_fetch_add(&port->stats.drops, 1U);
            LINKG_LOG_WARN("UART: invalid UDP SBUS packet uart=%u bytes=%u", port->config->uart_num, length);
            return;
        }

        for (index = 0U; index < length; index += LINKG_UART_SBUS_FRAME_SIZE)
        {
            _uart_sbus_deliver_frame(port, data + index);
        }
        return;
    }

    if (route == NULL)
    {
        return;
    }

    for (index = 0U; index < length; index++)
    {
        if (route->sbus_stream_count == 0U && data[index] != 0x0FU)
        {
            continue;
        }

        route->sbus_stream[route->sbus_stream_count++] = data[index];
        if (route->sbus_stream_count != LINKG_UART_SBUS_FRAME_SIZE)
        {
            continue;
        }

        if (_uart_sbus_valid(route->sbus_stream))
        {
            _uart_sbus_deliver_frame(port, route->sbus_stream);
            route->sbus_stream_count = 0U;
        }
        else
        {
            atomic_fetch_add(&port->stats.drops, 1U);
            memmove(route->sbus_stream, route->sbus_stream + 1, LINKG_UART_SBUS_FRAME_SIZE - 1U);
            route->sbus_stream_count = LINKG_UART_SBUS_FRAME_SIZE - 1U;
        }
    }
}

/****************************** SBUS定时调度 ******************************/

/**
 * @brief 尽力启用实时调度，权限不足时维持普通线程并记录诊断。
 */
static void _uart_sbus_try_realtime(void)
{
    struct sched_param param;
    int                ret;

    memset(&param, 0, sizeof(param));
    param.sched_priority = LINKG_UART_SBUS_RT_PRIORITY;
    ret = pthread_setschedparam(pthread_self(), SCHED_FIFO, &param);
    if (ret != 0)
    {
        LINKG_LOG_WARN("UART: SBUS FIFO scheduling unavailable error=%d", ret);
    }
}


/**
 * @brief 向绝对deadline累加微秒，避免相对睡眠造成累计漂移。
 */
static void _uart_sbus_deadline_add(struct timespec *deadline, uint32_t interval_us)
{
    deadline->tv_nsec += (long)(interval_us * 1000U);
    if (deadline->tv_nsec >= 1000000000L)
    {
        deadline->tv_sec++;
        deadline->tv_nsec -= 1000000000L;
    }
}

/**
 * @brief 基于输出前FIFO水位微调下一个周期，每次最多1us。
 */
static uint32_t _uart_sbus_adjust(uint32_t interval, uint32_t depth, uint32_t target)
{
    if (depth > target && interval > LINKG_UART_SBUS_MIN_US)
    {
        interval -= LINKG_UART_SBUS_ADJUST_US;
    }
    else if (depth > 0U && depth < target && interval < LINKG_UART_SBUS_MAX_US)
    {
        interval += LINKG_UART_SBUS_ADJUST_US;
    }

    if (interval < LINKG_UART_SBUS_MIN_US)
    {
        interval = LINKG_UART_SBUS_MIN_US;
    }
    if (interval > LINKG_UART_SBUS_MAX_US)
    {
        interval = LINKG_UART_SBUS_MAX_US;
    }
    return interval;
}

/**
 * @brief 只向物理UART写真实帧，部分提交时先补足本帧，绝不交织另一帧。
 */
static void _uart_sbus_write_pending(linkg_uart_port_t *port, uint8_t *frame, uint32_t *offset, bool *pending, uint32_t generation)
{
    int ret;

    ret = uart_port_write_generation(port, frame + *offset, LINKG_UART_SBUS_FRAME_SIZE - *offset, generation);
    if (ret > 0)
    {
        *offset += (uint32_t)ret;
        if (*offset == LINKG_UART_SBUS_FRAME_SIZE)
        {
            *pending = false;
            *offset  = 0U;
            atomic_fetch_add(&port->stats.sbus_tx_frames, 1U);
        }
        return;
    }

    if (ret != -EAGAIN)
    {
        // 物理设备异常：不能将未完成的帧前缀当作新帧重新发送。
        *pending = false;
        *offset  = 0U;
        atomic_fetch_add(&port->stats.drops, 1U);
    }
}

/**
 * @brief 单SBUS端口独立定时线程，FIFO空时不输出或伪造控制帧。
 */
void uart_sbus_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_uart_port_t *port;
    struct timespec    deadline;
    uint8_t            pending_frame[LINKG_UART_SBUS_FRAME_SIZE];
    uint32_t           pending_offset;
    uint32_t           interval;
    uint32_t           target;
    uint32_t           depth;
    uint32_t           pending_generation;
    uint64_t           warmup_started_ms;
    bool               primed;
    bool               pending;
    int                ret;

    port = (linkg_uart_port_t *)user_data;
    if (port == NULL || clock_gettime(CLOCK_MONOTONIC, &deadline) != 0)
    {
        return;
    }

    _uart_sbus_try_realtime();

    pending        = false;
    pending_offset = 0U;
    pending_generation = atomic_load(&port->generation);
    warmup_started_ms = 0U;
    primed = false;

    while (linkg_thread_is_running(thread))
    {
        interval = atomic_load(&port->sbus_interval_us);
        _uart_sbus_deadline_add(&deadline, interval);

        do
        {
            ret = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, NULL);
        }
        while (ret == EINTR && linkg_thread_is_running(thread));

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        // 落后多个周期时重置绝对时间，防止补偿式突发连发。
        {
            struct timespec now;
            if (clock_gettime(CLOCK_MONOTONIC, &now) == 0 &&
                ((now.tv_sec > deadline.tv_sec) ||
                 (now.tv_sec == deadline.tv_sec && now.tv_nsec > deadline.tv_nsec + 1000000L)))
            {
                deadline = now;
            }
        }

        if (pending && pending_generation != atomic_load(&port->generation))
        {
            pending = false;
            pending_offset = 0U;
            atomic_fetch_add(&port->stats.drops, 1U);
        }

        if (pending)
        {
            _uart_sbus_write_pending(port, pending_frame, &pending_offset, &pending, pending_generation);
            continue;
        }

        pthread_mutex_lock(&port->sbus_lock);
        depth  = port->sbus_fifo.count;
        target = atomic_load(&g_uart_manager.sbus_target_level);

        if (!primed && depth > 0U)
        {
            if (warmup_started_ms == 0U)
            {
                warmup_started_ms = uart_now_ms();
            }
            if (depth >= target || uart_now_ms() - warmup_started_ms >= (uint64_t)target * LINKG_UART_SBUS_INTERVAL_US / 1000U)
            {
                primed = true;
            }
        }

        if (primed && depth > 0U)
        {
            pending_generation = atomic_load(&port->generation);
            pending = _uart_sbus_pop_locked(port, pending_frame);
        }
        pthread_mutex_unlock(&port->sbus_lock);

        interval = primed ? _uart_sbus_adjust(interval, depth, target) : LINKG_UART_SBUS_INTERVAL_US;
        atomic_store(&port->sbus_interval_us, interval);

        if (!pending)
        {
            if (primed)
            {
                atomic_fetch_add(&port->stats.fifo_empty, 1U);
            }
            continue;
        }

        pending_offset = 0U;
        _uart_sbus_write_pending(port, pending_frame, &pending_offset, &pending, pending_generation);
    }
}
