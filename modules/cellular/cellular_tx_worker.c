/**
 * @file cellular_tx_worker.c
 * @brief LinkG蜂窝发送Worker的eventfd/poll调度与生命周期
 */

#define _GNU_SOURCE

#include "cellular_tx_worker.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_time.h"

/****************************** Worker状态 ******************************/

struct linkg_cellular_tx_worker
{
    pthread_t                            thread;   // 当前Worker线程
    linkg_cellular_tx_worker_process_fn  process;  // TX调度回调
    void                                *context;  // TX对象，由创建方保证生命周期
    int                                  event_fd; // 非阻塞事件通知
    _Atomic bool                         running;  // Worker循环结束条件
    bool                                 started;  // 生命周期由Cellular Link串行化
};

/****************************** 内部调度 ******************************/

/** @brief 将绝对单调时钟期限转换为poll等待毫秒，向上取整避免提前自旋。 */
static int _linkg_cellular_tx_worker_timeout_ms(uint64_t deadline_us, uint64_t now_us)
{
    uint64_t remaining_ms;

    if (deadline_us == UINT64_MAX)
    {
        return -1;
    }

    if (deadline_us <= now_us)
    {
        return 0;
    }

    remaining_ms = (deadline_us - now_us + 999ULL) / 1000ULL;
    if (remaining_ms > (uint64_t)INT_MAX)
    {
        return INT_MAX;
    }

    return (int)remaining_ms;
}

/** @brief 清空eventfd累计唤醒次数，EAGAIN代表已排空。 */
static void _linkg_cellular_tx_worker_drain(int event_fd)
{
    uint64_t value;
    ssize_t  length;

    for (;;)
    {
        length = read(event_fd, &value, sizeof(value));
        if (length == (ssize_t)sizeof(value))
        {
            continue;
        }

        if (length < 0 && errno == EINTR)
        {
            continue;
        }

        break;
    }
}

/** @brief 唯一TX Worker，不拥有Packet、Socket和令牌桶。 */
static void *_linkg_cellular_tx_worker_run(void *argument)
{
    linkg_cellular_tx_worker_t *worker;
    struct pollfd              descriptor;
    uint64_t                   deadline_us;
    uint64_t                   now_us;
    int                        timeout_ms;
    int                        ret;

    worker = argument;
    descriptor.fd      = worker->event_fd;
    descriptor.events  = POLLIN;
    descriptor.revents = 0;

    while (atomic_load(&worker->running))
    {
        now_us      = linkg_time_monotonic_us();
        deadline_us = worker->process(worker->context, now_us);

        if (!atomic_load(&worker->running))
        {
            break;
        }

        timeout_ms = _linkg_cellular_tx_worker_timeout_ms(deadline_us, linkg_time_monotonic_us());

        do
        {
            ret = poll(&descriptor, 1U, timeout_ms);
        }
        while (ret < 0 && errno == EINTR && atomic_load(&worker->running));

        if (ret > 0 && (descriptor.revents & POLLIN) != 0)
        {
            _linkg_cellular_tx_worker_drain(worker->event_fd);
        }
        else if (ret < 0 || (ret > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0))
        {
            LINKG_LOG_ERROR("Cellular TX worker poll failed, errno=%d", ret < 0 ? errno : EIO);
            // eventfd异常时停止本线程；上层关闭时仍会执行join。
            break;
        }
    }

    atomic_store(&worker->running, false);
    return NULL;
}

/****************************** 生命周期 ******************************/

linkg_cellular_tx_worker_t *linkg_cellular_tx_worker_create(linkg_cellular_tx_worker_process_fn process, void *context)
{
    linkg_cellular_tx_worker_t *worker;

    if (process == NULL || context == NULL)
    {
        return NULL;
    }

    worker = calloc(1, sizeof(*worker));
    if (worker == NULL)
    {
        return NULL;
    }

    worker->event_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (worker->event_fd < 0)
    {
        free(worker);
        return NULL;
    }

    worker->process = process;
    worker->context = context;
    atomic_init(&worker->running, false);
    return worker;
}

int linkg_cellular_tx_worker_start(linkg_cellular_tx_worker_t *worker)
{
    int ret;

    if (worker == NULL)
    {
        return -EINVAL;
    }

    if (worker->started)
    {
        return -EALREADY;
    }

    atomic_store(&worker->running, true);
    ret = pthread_create(&worker->thread, NULL, _linkg_cellular_tx_worker_run, worker);
    if (ret != 0)
    {
        atomic_store(&worker->running, false);
        return -ret;
    }

    worker->started = true;
    return 0;
}

int linkg_cellular_tx_worker_stop(linkg_cellular_tx_worker_t *worker)
{
    int ret;

    if (worker == NULL)
    {
        return -EINVAL;
    }

    if (!worker->started)
    {
        return 0;
    }

    atomic_store(&worker->running, false);
    linkg_cellular_tx_worker_notify(worker);

    ret = pthread_join(worker->thread, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    worker->started = false;
    return 0;
}

void linkg_cellular_tx_worker_destroy(linkg_cellular_tx_worker_t *worker)
{
    if (worker == NULL)
    {
        return;
    }

    if (linkg_cellular_tx_worker_stop(worker) != 0)
    {
        LINKG_LOG_ERROR("Cellular TX worker join failed; resources retained");
        return;
    }

    close(worker->event_fd);
    free(worker);
}

/****************************** 事件通知 ******************************/

/** @brief 无锁唤醒；eventfd EAGAIN时已有待处理唤醒，不需要重复写。 */
void linkg_cellular_tx_worker_notify(linkg_cellular_tx_worker_t *worker)
{
    uint64_t value;
    ssize_t  length;

    if (worker == NULL)
    {
        return;
    }

    value = 1U;
    do
    {
        length = write(worker->event_fd, &value, sizeof(value));
    }
    while (length < 0 && errno == EINTR);
}
