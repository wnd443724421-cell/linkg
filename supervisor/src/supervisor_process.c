/**
 * @file supervisor_process.c
 * @brief LinkG子进程监控、通知状态机与有界故障恢复
 * @author Dawn
 * @version 0.1.0
 * @date 2026-10-19
 */

#include "supervisor_process.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "linkg_supervisor_protocol.h"

#include "supervisor_config.h"

/****************************** 内部类型 ******************************/

typedef enum
{
    SUPERVISOR_STATE_STARTING   = 0, // 等待首次READY
    SUPERVISOR_STATE_RUNNING,        // 监控RUN心跳
    SUPERVISOR_STATE_RESTARTING,     // 同一PID重启服务
    SUPERVISOR_STATE_STOPPING        // 写端关闭后等待受控退出
} supervisor_state_t;

typedef struct
{
    pid_t              child_pid;    // 仅管理自身fork的子进程，waitpid后清除
    int                pipe_fd;      // Supervisor唯一持有的通知读端
    supervisor_state_t state;        // 当前子进程状态
    uint64_t           deadline_ms;  // 当前状态固定或心跳期限
    uint64_t           running_ms;   // 本次连续RUN起始时间
    bool               stable;       // 本次RUN是否已清除连续故障计数
} supervisor_child_t;

/****************************** 全局上下文 ******************************/

static volatile sig_atomic_t g_supervisor_stop = 0; // 信号处理函数仅设置退出标志

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取不可被系统校时影响的毫秒时间，失败时终止守护进程。
 */
static uint64_t _supervisor_now_ms(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        // Linux支持该时钟；异常时停止继续作出错误的超时决策。
        perror("Supervisor CLOCK_MONOTONIC");
        abort();
    }

    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

/**
 * @brief 标记退出或唤醒被子进程退出信号中断的poll。
 */
static void _supervisor_signal(int signal_number)
{
    if (signal_number != SIGCHLD)
    {
        g_supervisor_stop = 1;
    }
}

/**
 * @brief 安装可中断等待的退出与子进程信号处理。
 */
static int _supervisor_install_signals(void)
{
    const int        signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGCHLD};
    struct sigaction action;
    sigset_t         unblock;
    size_t           index;

    memset(&action, 0, sizeof(action));
    action.sa_handler = _supervisor_signal;
    sigemptyset(&action.sa_mask);
    sigemptyset(&unblock);

    for (index = 0U; index < sizeof(signals) / sizeof(signals[0]); index++)
    {
        if (sigaction(signals[index], &action, NULL) != 0)
        {
            return -errno;
        }

        sigaddset(&unblock, signals[index]);
    }

    return sigprocmask(SIG_UNBLOCK, &unblock, NULL) == 0 ? 0 : -errno;
}

/**
 * @brief 获取持有到退出的进程锁，锁文件保留以避免unlink导致双实例。
 */
static int _supervisor_lock(const char *path)
{
    int descriptor;
    int ret;

    descriptor = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
    {
        return -errno;
    }

    if (flock(descriptor, LOCK_EX | LOCK_NB) != 0)
    {
        ret = -errno;
        close(descriptor);
        return ret;
    }

    return descriptor;
}

/**
 * @brief 将管道描述符移到标准输入输出之外，确保exec后通道不被覆盖。
 */
static int _supervisor_move_descriptor(int *descriptor)
{
    int duplicate;

    if (*descriptor >= 3)
    {
        return 0;
    }

    duplicate = fcntl(*descriptor, F_DUPFD_CLOEXEC, 3);
    if (duplicate < 0)
    {
        return -errno;
    }

    close(*descriptor);
    *descriptor = duplicate;
    return 0;
}

/**
 * @brief 在单线程fork子进程中建立通知环境并执行指定程序。
 */
static void _supervisor_exec(char *const arguments[], int read_fd, int write_fd, pid_t parent_pid)
{
    const int        signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGCHLD};
    struct sigaction action;
    sigset_t         empty;
    char             descriptor_text[32];
    size_t           index;

    close(read_fd);
    memset(&action, 0, sizeof(action));
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);

    for (index = 0U; index < sizeof(signals) / sizeof(signals[0]); index++)
    {
        if (sigaction(signals[index], &action, NULL) != 0)
        {
            _exit(127);
        }
    }

    sigemptyset(&empty);
    if (sigprocmask(SIG_SETMASK, &empty, NULL) != 0 || prctl(PR_SET_PDEATHSIG, SIGTERM) != 0 || getppid() != parent_pid)
    {
        _exit(127);
    }

    snprintf(descriptor_text, sizeof(descriptor_text), "%d", write_fd);
    if (fcntl(write_fd, F_SETFD, 0) != 0 || setenv(LINKG_SUPERVISOR_FD_ENV, descriptor_text, 1) != 0)
    {
        _exit(127);
    }

    execv(arguments[0], arguments);
    perror("Supervisor execv");
    _exit(127);
}

/**
 * @brief 创建非阻塞管道并启动一个受监控子进程。
 */
static int _supervisor_spawn(supervisor_child_t *child, char *const arguments[])
{
    int   descriptors[2];
    int   ret;
    pid_t parent_pid;
    pid_t child_pid;

    if (pipe2(descriptors, O_NONBLOCK | O_CLOEXEC) != 0)
    {
        return -errno;
    }

    ret = _supervisor_move_descriptor(&descriptors[0]);
    if (ret == 0)
    {
        ret = _supervisor_move_descriptor(&descriptors[1]);
    }

    if (ret != 0)
    {
        close(descriptors[0]);
        close(descriptors[1]);
        return ret;
    }

    parent_pid = getpid();
    child_pid  = fork();
    if (child_pid == 0)
    {
        _supervisor_exec(arguments, descriptors[0], descriptors[1], parent_pid);
    }

    ret = -errno;
    close(descriptors[1]);
    if (child_pid < 0)
    {
        close(descriptors[0]);
        return ret;
    }

    child->child_pid   = child_pid;
    child->pipe_fd     = descriptors[0];
    child->state       = SUPERVISOR_STATE_STARTING;
    child->deadline_ms = _supervisor_now_ms() + SUPERVISOR_START_TIMEOUT_MS;
    child->running_ms  = 0U;
    child->stable      = false;

    fprintf(stderr, "Supervisor t=%llu pid=%ld state=STARTING\n", (unsigned long long)_supervisor_now_ms(), (long)child_pid);
    return 0;
}

/**
 * @brief 记录退出码或终止信号，供现场区分退出与故障。
 */
static void _supervisor_log_exit(pid_t child_pid, int status)
{
    if (WIFEXITED(status))
    {
        fprintf(stderr, "Supervisor pid=%ld exited code=%d\n", (long)child_pid, WEXITSTATUS(status));
    }
    else if (WIFSIGNALED(status))
    {
        fprintf(stderr, "Supervisor pid=%ld exited signal=%d\n", (long)child_pid, WTERMSIG(status));
    }
}

/**
 * @brief 查询并回收真实子进程，绝不使用管道EOF代替waitpid。
 */
static int _supervisor_reap(supervisor_child_t *child, int *status)
{
    pid_t result;

    do
    {
        result = waitpid(child->child_pid, status, WNOHANG);
    }
    while (result < 0 && errno == EINTR);

    if (result == 0)
    {
        return 0;
    }

    if (result < 0)
    {
        return -errno;
    }

    _supervisor_log_exit(child->child_pid, *status);
    return 1;
}

/**
 * @brief TERM等待到期后KILL并阻塞回收，旧进程存活期间不返回成功。
 */
static int _supervisor_terminate(supervisor_child_t *child)
{
    uint64_t deadline;
    pid_t    result;
    int      status;
    int      ret;

    if (child->pipe_fd >= 0)
    {
        close(child->pipe_fd);
        child->pipe_fd = -1;
    }

    if (child->child_pid <= 0)
    {
        return 0;
    }

    fprintf(stderr, "Supervisor pid=%ld stopping signal=SIGTERM\n", (long)child->child_pid);
    if (kill(child->child_pid, SIGTERM) != 0 && errno != ESRCH)
    {
        return -errno;
    }

    deadline = _supervisor_now_ms() + SUPERVISOR_TERMINATE_MS;
    while (_supervisor_now_ms() < deadline)
    {
        ret = _supervisor_reap(child, &status);
        if (ret != 0)
        {
            if (ret > 0)
            {
                child->child_pid = -1;
                return 0;
            }

            return ret;
        }

        poll(NULL, 0U, SUPERVISOR_POLL_MS);
    }

    fprintf(stderr, "Supervisor pid=%ld stopping signal=SIGKILL\n", (long)child->child_pid);
    if (kill(child->child_pid, SIGKILL) != 0 && errno != ESRCH)
    {
        return -errno;
    }

    do
    {
        result = waitpid(child->child_pid, &status, 0);
    }
    while (result < 0 && errno == EINTR);

    if (result < 0)
    {
        return -errno;
    }

    _supervisor_log_exit(child->child_pid, status);
    child->child_pid = -1;
    return 0;
}

/**
 * @brief 应用通知状态迁移，重复READY或RESTARTING不延长固定期限。
 */
static int _supervisor_message(supervisor_child_t *child, uint8_t message, uint64_t now_ms)
{
    if (message == LINKG_SUPERVISOR_READY)
    {
        if (child->state == SUPERVISOR_STATE_STARTING || child->state == SUPERVISOR_STATE_RESTARTING)
        {
            child->state       = SUPERVISOR_STATE_RUNNING;
            child->deadline_ms = now_ms + SUPERVISOR_HEARTBEAT_MS;
            child->running_ms  = now_ms;
            fprintf(stderr, "Supervisor pid=%ld state=RUNNING\n", (long)child->child_pid);
        }

        return 0;
    }

    if (message == LINKG_SUPERVISOR_HEARTBEAT)
    {
        if (child->state == SUPERVISOR_STATE_RUNNING)
        {
            child->deadline_ms = now_ms + SUPERVISOR_HEARTBEAT_MS;
        }

        return 0;
    }

    if (message == LINKG_SUPERVISOR_RESTARTING)
    {
        if (child->state == SUPERVISOR_STATE_RUNNING)
        {
            child->state       = SUPERVISOR_STATE_RESTARTING;
            child->deadline_ms = now_ms + SUPERVISOR_RESTART_TIMEOUT_MS;
            child->stable      = false;
            fprintf(stderr, "Supervisor pid=%ld state=RESTARTING\n", (long)child->child_pid);
        }

        return 0;
    }

    return -EPROTO;
}

/**
 * @brief 有界读取通知，避免大量输入饿死waitpid与超时判断。
 */
static int _supervisor_receive(supervisor_child_t *child)
{
    uint8_t messages[128];
    ssize_t count;
    ssize_t index;
    int     ret;

    count = read(child->pipe_fd, messages, sizeof(messages));
    if (count < 0)
    {
        return errno == EINTR || errno == EAGAIN ? 0 : -errno;
    }

    if (count == 0)
    {
        close(child->pipe_fd);
        child->pipe_fd = -1;

        // 正常退出和reboot前客户端关闭通道，给予有上限的清理窗口。
        if (child->state == SUPERVISOR_STATE_RUNNING)
        {
            child->state       = SUPERVISOR_STATE_STOPPING;
            child->deadline_ms = _supervisor_now_ms() + SUPERVISOR_RESTART_TIMEOUT_MS;
        }

        fprintf(stderr, "Supervisor pid=%ld pipe=EOF waiting_for_waitpid\n", (long)child->child_pid);
        return 0;
    }

    for (index = 0; index < count; index++)
    {
        ret = _supervisor_message(child, messages[index], _supervisor_now_ms());
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 监控单个子进程，任何子进程退出均通知恢复，守护停止时返回零。
 */
static int _supervisor_monitor(supervisor_child_t *child, unsigned int *failures)
{
    struct pollfd descriptor;
    uint64_t      now_ms;
    int           status;
    int           ret;

    while (!g_supervisor_stop)
    {
        ret = _supervisor_reap(child, &status);
        if (ret != 0)
        {
            if (ret < 0)
            {
                return ret;
            }

            child->child_pid = -1;
            return -ECHILD;
        }

        now_ms = _supervisor_now_ms();
        if (now_ms >= child->deadline_ms)
        {
            fprintf(stderr, "Supervisor pid=%ld timeout=%s\n", (long)child->child_pid,
                    child->state == SUPERVISOR_STATE_STARTING ? "READY" :
                    child->state == SUPERVISOR_STATE_RUNNING ? "HEARTBEAT" :
                    child->state == SUPERVISOR_STATE_RESTARTING ? "RESTARTING" : "STOPPING");
            return -ETIMEDOUT;
        }

        if (child->state == SUPERVISOR_STATE_RUNNING && !child->stable && now_ms - child->running_ms >= SUPERVISOR_STABLE_MS)
        {
            *failures     = 0U;
            child->stable = true;
            fprintf(stderr, "Supervisor pid=%ld stable failures=0\n", (long)child->child_pid);
        }

        descriptor.fd      = child->pipe_fd;
        descriptor.events  = POLLIN;
        descriptor.revents = 0;
        ret = poll(&descriptor, 1U, SUPERVISOR_POLL_MS);
        if (ret < 0 && errno != EINTR)
        {
            return -errno;
        }

        if (ret > 0)
        {
            if ((descriptor.revents & (POLLERR | POLLNVAL)) != 0)
            {
                return -EIO;
            }

            if ((descriptor.revents & (POLLIN | POLLHUP)) != 0)
            {
                // poll期间到期的消息不能挽救已经超时的状态。
                if (_supervisor_now_ms() >= child->deadline_ms)
                {
                    continue;
                }

                ret = _supervisor_receive(child);
                if (ret != 0)
                {
                    return ret;
                }
            }
        }
    }

    return 0;
}

/**
 * @brief 计算饱和退避并保持退出信号可响应。
 */
static void _supervisor_backoff(unsigned int *failures)
{
    uint64_t     deadline;
    unsigned int delay_ms;
    unsigned int index;

    if (*failures < 32U)
    {
        (*failures)++;
    }

    delay_ms = SUPERVISOR_RETRY_INITIAL_MS;
    for (index = 1U; index < *failures && delay_ms < SUPERVISOR_RETRY_MAX_MS; index++)
    {
        delay_ms *= 2U;
        if (delay_ms > SUPERVISOR_RETRY_MAX_MS)
        {
            delay_ms = SUPERVISOR_RETRY_MAX_MS;
        }
    }

    fprintf(stderr, "Supervisor failures=%u retry_ms=%u\n", *failures, delay_ms);
    deadline = _supervisor_now_ms() + delay_ms;
    while (!g_supervisor_stop && _supervisor_now_ms() < deadline)
    {
        poll(NULL, 0U, SUPERVISOR_POLL_MS);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 串行启动、监控、回收和恢复子进程，正常退出时释放进程锁。
 */
int supervisor_process_run(char *const child_arguments[], const char *lock_path)
{
    supervisor_child_t child;
    unsigned int       failures;
    int                lock_fd;
    int                stop_ret;
    int                ret;

    setvbuf(stderr, NULL, _IOLBF, 0);
    lock_fd = _supervisor_lock(lock_path);
    if (lock_fd < 0)
    {
        fprintf(stderr, "Supervisor lock failed path=%s error=%d\n", lock_path, lock_fd);
        return lock_fd;
    }

    g_supervisor_stop = 0;
    ret = _supervisor_install_signals();
    if (ret != 0)
    {
        close(lock_fd);
        return ret;
    }

    failures         = 0U;
    child.child_pid  = -1;
    child.pipe_fd    = -1;

    while (!g_supervisor_stop)
    {
        if (child.child_pid <= 0)
        {
            ret = _supervisor_spawn(&child, child_arguments);
            if (ret == 0)
            {
                ret = _supervisor_monitor(&child, &failures);
            }
        }

        stop_ret = _supervisor_terminate(&child);
        if (stop_ret != 0)
        {
            // 回收失败时保持常驻并退避重试，旧PID尚在时禁止启动第二个LinkG。
            fprintf(stderr, "Supervisor cleanup failed error=%d\n", stop_ret);
            ret = stop_ret;
            if (g_supervisor_stop)
            {
                break;
            }

            _supervisor_backoff(&failures);
            continue;
        }

        if (g_supervisor_stop)
        {
            ret = 0;
            break;
        }

        fprintf(stderr, "Supervisor recovery error=%d\n", ret);
        _supervisor_backoff(&failures);
    }

    close(lock_fd);
    return g_supervisor_stop && child.child_pid <= 0 ? 0 : ret;
}
