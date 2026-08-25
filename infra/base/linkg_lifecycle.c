/**
 * @file linkg_lifecycle.c
 * @brief LinkG应用生命周期管理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#define _GNU_SOURCE

#include "linkg_lifecycle.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/signalfd.h>
#include <unistd.h>

/****************************** 内部类型 ******************************/

typedef struct
{
    int          event_fd;       // 应用退出事件描述符
    int          signal_fd;      // 系统信号描述符
    sigset_t     previous_mask;  // 初始化前的信号掩码
    _Atomic int  action;         // 当前退出动作
    _Atomic bool requested;      // 是否已请求退出
    _Atomic bool initialized;    // 是否已经初始化
} linkg_lifecycle_context_t;

/****************************** 全局上下文 ******************************/

static linkg_lifecycle_context_t g_lifecycle =
{
    .event_fd  = -1,
    .signal_fd = -1
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查生命周期动作是否有效。
 */
static bool _lifecycle_action_valid(linkg_exit_action_t action)
{
    return action == LINKG_EXIT_ACTION_EXIT ||
           action == LINKG_EXIT_ACTION_RESTART_SERVICES ||
           action == LINKG_EXIT_ACTION_REBOOT_SYSTEM;
}

/**
 * @brief 提高待处理的生命周期退出动作等级。
 */
static void _lifecycle_action_raise(linkg_exit_action_t action)
{
    int current;

    current = atomic_load_explicit(&g_lifecycle.action, memory_order_relaxed);

    while (current < (int)action &&
           !atomic_compare_exchange_weak_explicit(&g_lifecycle.action,
                                                  &current,
                                                  (int)action,
                                                  memory_order_release,
                                                  memory_order_relaxed))
    {
    }

    atomic_store_explicit(&g_lifecycle.requested, true, memory_order_release);
}

/**
 * @brief 清空生命周期事件通知。
 */
static void _lifecycle_event_drain(void)
{
    uint64_t value;
    ssize_t  read_size;

    if (g_lifecycle.event_fd < 0)
    {
        return;
    }

    for (;;)
    {
        read_size = read(g_lifecycle.event_fd, &value, sizeof(value));
        if (read_size == (ssize_t)sizeof(value))
        {
            continue;
        }

        if (read_size < 0 && errno == EINTR)
        {
            continue;
        }

        break;
    }
}


/**
 * @brief 清空生命周期信号通知。
 */
static void _lifecycle_signal_drain(void)
{
    struct signalfd_siginfo signal_info;
    ssize_t                 read_size;

    if (g_lifecycle.signal_fd < 0)
    {
        return;
    }

    for (;;)
    {
        read_size = read(g_lifecycle.signal_fd, &signal_info, sizeof(signal_info));
        if (read_size == (ssize_t)sizeof(signal_info))
        {
            continue;
        }

        if (read_size < 0 && errno == EINTR)
        {
            continue;
        }

        break;
    }
}
/****************************** 生命周期 ******************************/

/**
 * @brief 初始化应用生命周期管理。
 */
int linkg_lifecycle_init(void)
{
    sigset_t signal_mask;
    int      ret;

    if (atomic_load_explicit(&g_lifecycle.initialized, memory_order_acquire))
    {
        return -EALREADY;
    }

    sigemptyset(&signal_mask);
    sigaddset(&signal_mask, SIGINT);
    sigaddset(&signal_mask, SIGTERM);
    sigaddset(&signal_mask, SIGQUIT);

    ret = pthread_sigmask(SIG_BLOCK, &signal_mask, &g_lifecycle.previous_mask);
    if (ret != 0)
    {
        return -ret;
    }

    g_lifecycle.event_fd = eventfd(0U, EFD_CLOEXEC | EFD_NONBLOCK);
    if (g_lifecycle.event_fd < 0)
    {
        ret = -errno;
        pthread_sigmask(SIG_SETMASK, &g_lifecycle.previous_mask, NULL);
        return ret;
    }

    g_lifecycle.signal_fd = signalfd(-1, &signal_mask, SFD_CLOEXEC | SFD_NONBLOCK);
    if (g_lifecycle.signal_fd < 0)
    {
        ret = -errno;

        close(g_lifecycle.event_fd);
        g_lifecycle.event_fd = -1;

        pthread_sigmask(SIG_SETMASK, &g_lifecycle.previous_mask, NULL);

        return ret;
    }

    atomic_store_explicit(&g_lifecycle.action, LINKG_EXIT_ACTION_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_lifecycle.requested, false, memory_order_relaxed);
    atomic_store_explicit(&g_lifecycle.initialized, true, memory_order_release);

    return 0;
}

/**
 * @brief 重置生命周期退出状态。
 *
 * @note 只能在上一轮服务已经全部停止，并且不再提交新的退出请求时调用。
 */
int linkg_lifecycle_reset(void)
{
    if (!atomic_load_explicit(&g_lifecycle.initialized, memory_order_acquire))
    {
        return -ENODEV;
    }

    // 清除上一轮生命周期残留的事件和信号通知。
    _lifecycle_event_drain();
    _lifecycle_signal_drain();

    atomic_store_explicit(&g_lifecycle.action, LINKG_EXIT_ACTION_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_lifecycle.requested, false, memory_order_release);

    return 0;
}

/**
 * @brief 反初始化应用生命周期管理。
 */
void linkg_lifecycle_deinit(void)
{
    if (!atomic_exchange_explicit(&g_lifecycle.initialized, false, memory_order_acq_rel))
    {
        return;
    }

    if (g_lifecycle.signal_fd >= 0)
    {
        close(g_lifecycle.signal_fd);
        g_lifecycle.signal_fd = -1;
    }

    if (g_lifecycle.event_fd >= 0)
    {
        close(g_lifecycle.event_fd);
        g_lifecycle.event_fd = -1;
    }

    pthread_sigmask(SIG_SETMASK, &g_lifecycle.previous_mask, NULL);

    atomic_store_explicit(&g_lifecycle.action, LINKG_EXIT_ACTION_NONE, memory_order_relaxed);
    atomic_store_explicit(&g_lifecycle.requested, false, memory_order_relaxed);
}

/****************************** 退出请求 ******************************/

/**
 * @brief 提交应用退出请求。
 */
int linkg_lifecycle_request_exit(linkg_exit_action_t action)
{
    uint64_t value;
    ssize_t  write_size;

    if (!_lifecycle_action_valid(action))
    {
        return -EINVAL;
    }

    if (!atomic_load_explicit(&g_lifecycle.initialized, memory_order_acquire))
    {
        return -ENODEV;
    }

    /**
     * 动作值同时表示优先级：
     * REBOOT_SYSTEM > RESTART_APP > EXIT。
     */
    _lifecycle_action_raise(action);

    value = 1U;

    do
    {
        write_size = write(g_lifecycle.event_fd, &value, sizeof(value));
    }
    while (write_size < 0 && errno == EINTR);

    if (write_size == (ssize_t)sizeof(value))
    {
        return 0;
    }

    /**
     * eventfd 中已经存在未消费事件时，计数器理论上可能饱和。
     * requested 已经置位，因此可视为请求已经成功提交。
     */
    if (write_size < 0 && errno == EAGAIN)
    {
        return 0;
    }

    return write_size < 0 ? -errno : -EIO;
}

/**
 * @brief 阻塞等待应用退出请求。
 */
linkg_exit_action_t linkg_lifecycle_wait_for_exit(void)
{
    struct signalfd_siginfo signal_info;
    struct pollfd           poll_fds[2];
    uint64_t                value;
    ssize_t                 read_size;
    int                     ret;

    if (!atomic_load_explicit(&g_lifecycle.initialized, memory_order_acquire))
    {
        return LINKG_EXIT_ACTION_NONE;
    }

    if (atomic_load_explicit(&g_lifecycle.requested, memory_order_acquire))
    {
        return linkg_lifecycle_get_exit_action();
    }

    memset(poll_fds, 0, sizeof(poll_fds));

    poll_fds[0].fd     = g_lifecycle.event_fd;
    poll_fds[0].events = POLLIN;
    poll_fds[1].fd     = g_lifecycle.signal_fd;
    poll_fds[1].events = POLLIN;

    for (;;)
    {
        ret = poll(poll_fds, 2U, -1);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return LINKG_EXIT_ACTION_NONE;
        }

        if ((poll_fds[0].revents & POLLIN) != 0)
        {
            do
            {
                read_size = read(g_lifecycle.event_fd, &value, sizeof(value));
            }
            while (read_size < 0 && errno == EINTR);

            if (read_size == (ssize_t)sizeof(value) ||
                (read_size < 0 && errno == EAGAIN))
            {
                return linkg_lifecycle_get_exit_action();
            }

            return LINKG_EXIT_ACTION_NONE;
        }

        if ((poll_fds[1].revents & POLLIN) != 0)
        {
            do
            {
                read_size = read(g_lifecycle.signal_fd, &signal_info, sizeof(signal_info));
            }
            while (read_size < 0 && errno == EINTR);

            if (read_size != (ssize_t)sizeof(signal_info))
            {
                return LINKG_EXIT_ACTION_NONE;
            }

            _lifecycle_action_raise(LINKG_EXIT_ACTION_EXIT);

            return linkg_lifecycle_get_exit_action();
        }

        if ((poll_fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0 ||
            (poll_fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return LINKG_EXIT_ACTION_NONE;
        }
    }
}

/****************************** 状态查询 ******************************/

/**
 * @brief 检查是否已收到退出请求。
 */
bool linkg_lifecycle_is_exit_requested(void)
{
    return atomic_load_explicit(&g_lifecycle.requested, memory_order_acquire);
}

/**
 * @brief 获取当前退出动作。
 */
linkg_exit_action_t linkg_lifecycle_get_exit_action(void)
{
    return (linkg_exit_action_t)atomic_load_explicit(&g_lifecycle.action, memory_order_acquire);
}
