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
    _Atomic bool                         degraded; // eventfd异常后的定时调度模式
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
    bool                       degraded;

    worker   = argument;
    degraded = false;

    ret = pthread_setname_np(pthread_self(), "cellular_tx");
    if (ret != 0)
    {
        LINKG_LOG_WARN("Cellular TX worker naming failed, error=%d", ret);
    }

    descriptor.fd      = worker->event_fd;
    descriptor.events  = POLLIN;
    descriptor.revents = 0;

    LINKG_LOG_INFO("Cellular TX worker started, event_fd=%d", worker->event_fd);

    while (atomic_load(&worker->running))
    {
        now_us      = linkg_time_monotonic_us();
        deadline_us = worker->process(worker->context, now_us);

        if (!atomic_load(&worker->running))
        {
            break;
        }

        timeout_ms = _linkg_cellular_tx_worker_timeout_ms(deadline_us, linkg_time_monotonic_us());

        /*
         * eventfd失效时继续执行TX调度回调，不能在started仍为true的情况下
         * 让唯一Worker消失。降级期间每5ms检查一次队列和令牌状态。
         */
        if (degraded)
        {
            if (timeout_ms < 0 || timeout_ms > 5)
            {
                timeout_ms = 5;
            }

            (void)poll(NULL, 0U, timeout_ms);
            continue;
        }

        descriptor.revents = 0;
        do
        {
            ret = poll(&descriptor, 1U, timeout_ms);
        }
        while (ret < 0 && errno == EINTR && atomic_load(&worker->running));

        if (!atomic_load(&worker->running))
        {
            break;
        }

        if (ret > 0 && (descriptor.revents & POLLIN) != 0)
        {
            _linkg_cellular_tx_worker_drain(worker->event_fd);
        }
        else if (ret < 0 ||
                 (ret > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0))
        {
            LINKG_LOG_ERROR("Cellular TX worker poll error=%d revents=0x%x, enable timed fallback",
                            ret < 0 ? errno : EIO, (unsigned int)descriptor.revents);
            degraded = true;
            atomic_store(&worker->degraded, true);
        }
    }

    LINKG_LOG_INFO("Cellular TX worker stopped");
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
    atomic_init(&worker->degraded, false);
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

    atomic_store(&worker->degraded, false);
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

    if (worker == NULL || atomic_load(&worker->degraded))
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
