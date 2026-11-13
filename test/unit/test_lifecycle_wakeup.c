/**
 * @file test_lifecycle_wakeup.c
 * @brief 生命周期通知失败后已登记重载仍能完成等待
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */
#include "linkg_lifecycle.h"

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <poll.h>
#include <stdatomic.h>
#include <stdint.h>
#include <unistd.h>

static atomic_bool fail_write;
static atomic_bool waiting;
static linkg_exit_action_t result;

ssize_t __real_write(int fd, const void *data, size_t length);

/**
 * @brief 只对本次eventfd通知注入一次写失败。
 */
ssize_t __wrap_write(int fd, const void *data, size_t length)
{
    if (length == sizeof(uint64_t) && atomic_exchange(&fail_write, false))
    {
        errno = EIO;
        return -1;
    }
    return __real_write(fd, data, length);
}

/**
 * @brief 在请求登记前先阻塞，覆盖丢失通知的真实等待场景。
 */
int __real_poll(struct pollfd *fds, nfds_t count, int timeout);

/**
 * @brief 在真实等待调用前同步，保证请求发生在初始requested检查之后。
 */
int __wrap_poll(struct pollfd *fds, nfds_t count, int timeout)
{
    atomic_store(&waiting, true);
    return __real_poll(fds, count, timeout);
}

static void *waiter(void *unused)
{
    (void)unused;
    result = linkg_lifecycle_wait_for_exit();
    return NULL;
}

/**
 * @brief 请求返回错误后，无额外通知也能在有界轮询中发现登记动作。
 */
int main(void)
{
    pthread_t thread;

    assert(linkg_lifecycle_init() == 0);
    assert(pthread_create(&thread, NULL, waiter, NULL) == 0);
    while (!atomic_load(&waiting))
    {
        usleep(1000U);
    }
    atomic_store(&fail_write, true);
    assert(linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_RESTART_SERVICES) == -EIO);
    assert(linkg_lifecycle_is_exit_requested());
    assert(pthread_join(thread, NULL) == 0);
    assert(result == LINKG_EXIT_ACTION_RESTART_SERVICES);
    assert(linkg_lifecycle_reset() == 0);
    linkg_lifecycle_deinit();
    return 0;
}
