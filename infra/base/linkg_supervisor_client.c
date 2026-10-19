/**
 * @file linkg_supervisor_client.c
 * @brief LinkG非阻塞进程守护通知与RUN阶段心跳
 * @author Dawn
 * @version 0.1.0
 * @date 2026-10-19
 */

#include "linkg_supervisor_client.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include "linkg_supervisor_protocol.h"

/****************************** 模块常量 ******************************/

#ifdef LINKG_SUPERVISOR_TEST_BUILD
#define LINKG_SUPERVISOR_HEARTBEAT_MS 100U  // 独立测试目标使用的周期
#else
#define LINKG_SUPERVISOR_HEARTBEAT_MS 1000U // 产品心跳周期，单位毫秒
#endif

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t lock;        // 保护描述符、运行状态及发送顺序
    pthread_cond_t  changed;     // 单调时钟条件变量，用于唤醒或停止线程
    pthread_t       worker;      // 整个进程生命周期仅创建一次
    int             pipe_fd;     // 客户端持有写端，deinit负责关闭
    bool            initialized; // 初始化成功，允许状态通知
    bool            running;     // 仅READY成功后允许周期心跳
    bool            stopping;    // 通知心跳线程退出
} linkg_supervisor_client_t;

/****************************** 全局上下文 ******************************/

static linkg_supervisor_client_t g_supervisor_client =
{
    .lock    = PTHREAD_MUTEX_INITIALIZER, // 串行化通知与状态切换
    .pipe_fd = -1                        // 未连接Supervisor
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 发送一个原子消息，仅消费本次写入产生的SIGPIPE。
 *
 * 调用方持有客户端锁。管道设置为非阻塞，不等待可写或无限重试。
 */
static int _supervisor_client_send_locked(linkg_supervisor_message_t message)
{
    const struct timespec zero = {0, 0};
    sigset_t              blocked;
    sigset_t              previous;
    sigset_t              pending;
    uint8_t               value;
    ssize_t               written;
    int                   ret;
    int                   saved_errno;
    bool                  already_pending;

    if (g_supervisor_client.pipe_fd < 0)
    {
        return -EPIPE;
    }

    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    ret = pthread_sigmask(SIG_BLOCK, &blocked, &previous);
    if (ret != 0)
    {
        return -ret;
    }

    if (sigpending(&pending) != 0)
    {
        saved_errno = errno;
        pthread_sigmask(SIG_SETMASK, &previous, NULL);
        return -saved_errno;
    }

    already_pending = sigismember(&pending, SIGPIPE) == 1;
    value           = (uint8_t)message;
    written         = write(g_supervisor_client.pipe_fd, &value, sizeof(value));
    saved_errno     = errno;

    if (written < 0 && saved_errno == EPIPE && !already_pending)
    {
        // 保留调用前已存在的SIGPIPE，不更改进程全局信号处理方式。
        while (sigtimedwait(&blocked, NULL, &zero) < 0 && errno == EINTR)
        {
        }
    }

    ret = pthread_sigmask(SIG_SETMASK, &previous, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    if (written == (ssize_t)sizeof(value))
    {
        return 0;
    }

    // EAGAIN/EINTR不阻塞业务；控制通知失败由入口结束本轮进程，心跳则下周期重试。
    return written < 0 ? -saved_errno : -EIO;
}

/**
 * @brief 计算下一次心跳的单调时钟期限。
 */
static int _supervisor_client_next_deadline(struct timespec *deadline)
{
    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
    {
        return -errno;
    }

    deadline->tv_nsec += (long)LINKG_SUPERVISOR_HEARTBEAT_MS * 1000000L;
    deadline->tv_sec  += deadline->tv_nsec / 1000000000L;
    deadline->tv_nsec %= 1000000000L;
    return 0;
}

/**
 * @brief 在RUN阶段周期通知基本活性，服务重启期间停止心跳。
 */
static void *_supervisor_client_worker(void *argument)
{
    struct timespec deadline;
    int             ret;

    (void)argument;
    pthread_mutex_lock(&g_supervisor_client.lock);

    while (!g_supervisor_client.stopping)
    {
        while (!g_supervisor_client.running && !g_supervisor_client.stopping)
        {
            pthread_cond_wait(&g_supervisor_client.changed, &g_supervisor_client.lock);
        }

        if (g_supervisor_client.stopping)
        {
            break;
        }

        ret = _supervisor_client_next_deadline(&deadline);
        if (ret != 0)
        {
            break;
        }

        do
        {
            ret = pthread_cond_timedwait(&g_supervisor_client.changed, &g_supervisor_client.lock, &deadline);
        }
        while (ret == 0 && g_supervisor_client.running && !g_supervisor_client.stopping);

        if (ret == ETIMEDOUT && g_supervisor_client.running && !g_supervisor_client.stopping)
        {
            ret = _supervisor_client_send_locked(LINKG_SUPERVISOR_HEARTBEAT);
            if (ret != 0 && ret != -EAGAIN && ret != -EINTR)
            {
                g_supervisor_client.running = false;
            }
        }
        else if (ret != 0 && ret != ETIMEDOUT)
        {
            break;
        }
    }

    pthread_mutex_unlock(&g_supervisor_client.lock);
    return NULL;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 接管继承的通知写端，没有环境变量时保持独立运行模式。
 *
 * 由主线程在生命周期信号初始化之后、业务线程启动之前调用。
 */
int linkg_supervisor_client_init(void)
{
    pthread_condattr_t attributes;
    struct stat        descriptor_stat;
    const char        *value;
    char              *end;
    long               descriptor;
    int                flags;
    int                ret;

    if (g_supervisor_client.initialized)
    {
        return -EALREADY;
    }

    value = getenv(LINKG_SUPERVISOR_FD_ENV);
    if (value == NULL)
    {
        return 0;
    }

    errno      = 0;
    descriptor = strtol(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || descriptor < 3 || descriptor > INT_MAX)
    {
        unsetenv(LINKG_SUPERVISOR_FD_ENV);
        return -EINVAL;
    }

    // 通知通道只传给LinkG，不再通过环境变量传给后续启动的工具程序。
    if (unsetenv(LINKG_SUPERVISOR_FD_ENV) != 0)
    {
        return -errno;
    }

    flags = fcntl((int)descriptor, F_GETFL);
    if (flags < 0)
    {
        return -errno;
    }

    if (fstat((int)descriptor, &descriptor_stat) != 0)
    {
        return -errno;
    }

    if (!S_ISFIFO(descriptor_stat.st_mode) || (flags & O_ACCMODE) != O_WRONLY)
    {
        return -EINVAL;
    }

    g_supervisor_client.pipe_fd = (int)descriptor;
    if (fcntl((int)descriptor, F_SETFD, FD_CLOEXEC) != 0 || fcntl((int)descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
    {
        ret = -errno;
        goto close_pipe;
    }

    ret = pthread_condattr_init(&attributes);
    if (ret != 0)
    {
        ret = -ret;
        goto close_pipe;
    }

    ret = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (ret == 0)
    {
        ret = pthread_cond_init(&g_supervisor_client.changed, &attributes);
    }

    pthread_condattr_destroy(&attributes);
    if (ret != 0)
    {
        ret = -ret;
        goto close_pipe;
    }

    g_supervisor_client.running  = false;
    g_supervisor_client.stopping = false;
    ret = pthread_create(&g_supervisor_client.worker, NULL, _supervisor_client_worker, NULL);
    if (ret != 0)
    {
        pthread_cond_destroy(&g_supervisor_client.changed);
        ret = -ret;
        goto close_pipe;
    }

    g_supervisor_client.initialized = true;
    return 0;

close_pipe:
    close(g_supervisor_client.pipe_fd);
    g_supervisor_client.pipe_fd = -1;
    return ret;
}

/**
 * @brief 停止唯一心跳线程并关闭写端，供正常退出或系统重启前调用。
 */
void linkg_supervisor_client_deinit(void)
{
    if (!g_supervisor_client.initialized)
    {
        return;
    }

    pthread_mutex_lock(&g_supervisor_client.lock);
    g_supervisor_client.running  = false;
    g_supervisor_client.stopping = true;
    pthread_cond_signal(&g_supervisor_client.changed);
    pthread_mutex_unlock(&g_supervisor_client.lock);

    pthread_join(g_supervisor_client.worker, NULL);
    close(g_supervisor_client.pipe_fd);
    pthread_cond_destroy(&g_supervisor_client.changed);

    g_supervisor_client.pipe_fd     = -1;
    g_supervisor_client.initialized = false;
}

/****************************** 状态通知 ******************************/

/**
 * @brief 原子发送READY并允许周期心跳。
 */
int linkg_supervisor_client_ready(void)
{
    int ret;

    if (!g_supervisor_client.initialized)
    {
        return 0;
    }

    pthread_mutex_lock(&g_supervisor_client.lock);
    ret = _supervisor_client_send_locked(LINKG_SUPERVISOR_READY);
    g_supervisor_client.running = ret == 0;
    pthread_cond_signal(&g_supervisor_client.changed);
    pthread_mutex_unlock(&g_supervisor_client.lock);
    return ret;
}

/**
 * @brief 停止周期心跳后发送RESTARTING，避免掩盖服务清理或启动卡死。
 */
int linkg_supervisor_client_restarting(void)
{
    int ret;

    if (!g_supervisor_client.initialized)
    {
        return 0;
    }

    pthread_mutex_lock(&g_supervisor_client.lock);
    g_supervisor_client.running = false;
    ret = _supervisor_client_send_locked(LINKG_SUPERVISOR_RESTARTING);
    pthread_cond_signal(&g_supervisor_client.changed);
    pthread_mutex_unlock(&g_supervisor_client.lock);
    return ret;
}
