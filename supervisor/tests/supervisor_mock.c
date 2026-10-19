/**
 * @file supervisor_mock.c
 * @brief 模拟LinkG生命周期及故障的可执行测试子进程
 * @author Dawn
 * @version 0.1.0
 * @date 2026-10-19
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

#include "linkg_supervisor_client.h"
#include "linkg_supervisor_protocol.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 休眠指定毫秒并处理信号打断。
 */
static void _mock_sleep(unsigned int milliseconds)
{
    struct timespec delay;

    delay.tv_sec  = milliseconds / 1000U;
    delay.tv_nsec = (long)(milliseconds % 1000U) * 1000000L;
    while (nanosleep(&delay, &delay) != 0 && errno == EINTR)
    {
    }
}

/**
 * @brief 统计当前线程数，用于验证多轮重启没有新增线程。
 */
static unsigned int _mock_thread_count(void)
{
    struct dirent *entry;
    DIR           *directory;
    unsigned int   count;

    directory = opendir("/proc/self/task");
    if (directory == NULL)
    {
        return 0U;
    }

    count = 0U;
    while ((entry = readdir(directory)) != NULL)
    {
        if (entry->d_name[0] != '.')
        {
            count++;
        }
    }

    closedir(directory);
    return count;
}

/**
 * @brief 发送裸协议字节以模拟失控或不完整的客户端。
 */
static int _mock_send(int descriptor, uint8_t message)
{
    return write(descriptor, &message, sizeof(message)) == (ssize_t)sizeof(message) ? 0 : -errno;
}

/**
 * @brief 使用真实客户端验证同一PID中的多轮服务重启。
 */
static int _mock_client(const char *mode)
{
    const char  *value;
    unsigned int index;
    int          descriptor;
    int          ret;

    value      = getenv(LINKG_SUPERVISOR_FD_ENV);
    descriptor = value != NULL ? atoi(value) : -1;
    ret        = linkg_supervisor_client_init();
    if (ret == 0)
    {
        ret = linkg_supervisor_client_ready();
    }

    if (strcmp(mode, "probe") == 0)
    {
        printf("client_result=%d threads=%u\n", ret, _mock_thread_count());
        if (ret == 0 && descriptor >= 3)
        {
            printf("client_flags cloexec=%d nonblock=%d environment_removed=%d\n",
                   (fcntl(descriptor, F_GETFD) & FD_CLOEXEC) != 0,
                   (fcntl(descriptor, F_GETFL) & O_NONBLOCK) != 0,
                   getenv(LINKG_SUPERVISOR_FD_ENV) == NULL);
        }

        linkg_supervisor_client_deinit();
        // pthread_join已完成后，procfs任务条目的移除仍可能短暂滞后。
        for (index = 0U; index < 20U && _mock_thread_count() != 1U; index++)
        {
            _mock_sleep(5U);
        }

        printf("client_closed threads=%u\n", _mock_thread_count());
        return 0;
    }

    if (ret != 0)
    {
        linkg_supervisor_client_deinit();
        return 10;
    }

    if (strcmp(mode, "restart") == 0 || strcmp(mode, "restart30") == 0 || strcmp(mode, "cycles") == 0)
    {
        for (index = 0U; index < (strcmp(mode, "cycles") == 0 ? 4U : 1U); index++)
        {
            _mock_sleep(150U);
            if (linkg_supervisor_client_restarting() != 0)
            {
                linkg_supervisor_client_deinit();
                return 11;
            }

            _mock_sleep(strcmp(mode, "restart30") == 0 ? 30000U : 600U);
            if (linkg_supervisor_client_ready() != 0)
            {
                return 12;
            }

            printf("mock_cycle=%u threads=%u pid=%ld\n", index, _mock_thread_count(), (long)getpid());
        }
    }

    if (strcmp(mode, "exit") == 0)
    {
        _mock_sleep(100U);
        return 17;
    }

    if (strcmp(mode, "crash") == 0)
    {
        _mock_sleep(100U);
        raise(SIGSEGV);
        return 18;
    }

    if (strcmp(mode, "recover-stable") == 0)
    {
        _mock_sleep(1000U);
        return 17;
    }

    if (strcmp(mode, "clean") == 0 || strcmp(mode, "cleanup") == 0 || strcmp(mode, "cleanup-long") == 0)
    {
        _mock_sleep(150U);
        linkg_supervisor_client_deinit();
        _mock_sleep(strcmp(mode, "cleanup-long") == 0 ? 6000U : strcmp(mode, "cleanup") == 0 ? 600U : 0U);
        return 0;
    }

    for (;;)
    {
        pause();
    }
}

/****************************** 主程序 ******************************/

/**
 * @brief 运行指定场景，同时以独立文件锁检测重复子进程。
 */
int main(int argc, char **argv)
{
    const struct rlimit no_core = {0, 0};
    const char         *mode;
    const char         *value;
    int                 descriptor;
    int                 overlap_lock;

    setrlimit(RLIMIT_CORE, &no_core);
    prctl(PR_SET_DUMPABLE, 0);
    setvbuf(stdout, NULL, _IOLBF, 0);
    mode = argc > 1 ? argv[1] : "healthy";

    value = getenv("SUPERVISOR_MOCK_LOCK");
    if (value != NULL)
    {
        overlap_lock = open(value, O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (overlap_lock < 0 || flock(overlap_lock, LOCK_EX | LOCK_NB) != 0)
        {
            fprintf(stderr, "MOCK_OVERLAP\n");
            return 90;
        }
    }

    printf("mock_pid=%ld mode=%s\n", (long)getpid(), mode);
    if (strcmp(mode, "recover-stable") == 0)
    {
        value = getenv("SUPERVISOR_MOCK_STATE");
        descriptor = value != NULL ? open(value, O_CREAT | O_EXCL | O_WRONLY, 0600) : -1;
        if (descriptor >= 0)
        {
            close(descriptor);
            return 17;
        }
    }

    if (strcmp(mode, "early-clean") == 0)
    {
        return 0;
    }

    if (strcmp(mode, "healthy") == 0 || strcmp(mode, "restart") == 0 || strcmp(mode, "restart30") == 0 || strcmp(mode, "cycles") == 0 ||
        strcmp(mode, "exit") == 0 || strcmp(mode, "crash") == 0 || strcmp(mode, "clean") == 0 ||
        strcmp(mode, "cleanup") == 0 || strcmp(mode, "cleanup-long") == 0 || strcmp(mode, "probe") == 0 || strcmp(mode, "recover-stable") == 0)
    {
        return _mock_client(mode);
    }

    value = getenv(LINKG_SUPERVISOR_FD_ENV);
    if (value == NULL)
    {
        return 19;
    }

    descriptor = atoi(value);
    if (strcmp(mode, "ignore-term") == 0)
    {
        signal(SIGTERM, SIG_IGN);
    }

    if (strcmp(mode, "startup") != 0 && strcmp(mode, "startup-heartbeat") != 0 &&
        strcmp(mode, "stopping-startup") != 0 && strcmp(mode, "eof-startup") != 0)
    {
        _mock_send(descriptor, LINKG_SUPERVISOR_READY);
    }

    if (strcmp(mode, "pipe-eof") == 0)
    {
        close(descriptor);
    }

    if (strcmp(mode, "stopping-startup") == 0)
    {
        _mock_sleep(300U);
        _mock_send(descriptor, LINKG_SUPERVISOR_STOPPING);
        close(descriptor);
    }

    if (strcmp(mode, "stopping-restart") == 0)
    {
        _mock_send(descriptor, LINKG_SUPERVISOR_RESTARTING);
        _mock_sleep(400U);
        _mock_send(descriptor, LINKG_SUPERVISOR_STOPPING);
        close(descriptor);
    }

    if (strcmp(mode, "stopping-repeat") == 0)
    {
        _mock_send(descriptor, LINKG_SUPERVISOR_STOPPING);
    }

    if (strcmp(mode, "eof-startup") == 0 || strcmp(mode, "eof-restart") == 0)
    {
        if (strcmp(mode, "eof-restart") == 0)
        {
            _mock_send(descriptor, LINKG_SUPERVISOR_RESTARTING);
        }

        close(descriptor);
    }

    if (strcmp(mode, "fast-clean") == 0)
    {
        close(descriptor);
        return 0;
    }

    if (strcmp(mode, "invalid") == 0)
    {
        _mock_send(descriptor, 255U);
    }

    if (strcmp(mode, "restart-timeout") == 0 || strcmp(mode, "restart-repeat") == 0)
    {
        _mock_send(descriptor, LINKG_SUPERVISOR_RESTARTING);
    }

    for (;;)
    {
        if (strcmp(mode, "startup-heartbeat") == 0)
        {
            _mock_send(descriptor, LINKG_SUPERVISOR_HEARTBEAT);
        }

        if (strcmp(mode, "restart-repeat") == 0)
        {
            _mock_send(descriptor, LINKG_SUPERVISOR_RESTARTING);
            _mock_send(descriptor, LINKG_SUPERVISOR_HEARTBEAT);
        }

        if (strcmp(mode, "ready-repeat") == 0)
        {
            _mock_send(descriptor, LINKG_SUPERVISOR_READY);
        }

        if (strcmp(mode, "stopping-repeat") == 0)
        {
            _mock_send(descriptor, LINKG_SUPERVISOR_STOPPING);
            _mock_send(descriptor, LINKG_SUPERVISOR_READY);
            _mock_send(descriptor, LINKG_SUPERVISOR_RESTARTING);
            _mock_send(descriptor, LINKG_SUPERVISOR_HEARTBEAT);
        }

        _mock_sleep(100U);
    }
}
