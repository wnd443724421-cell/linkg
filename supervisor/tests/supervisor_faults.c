/**
 * @file supervisor_faults.c
 * @brief 仅测试使用的系统调用故障注入，不进入产品构建
 * @author Dawn
 * @version 0.1.0
 * @date 2026-10-19
 */

#include <errno.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/****************************** 全局上下文 ******************************/

static unsigned int g_fault_kill_count = 0U; // 当前Supervisor已注入的kill错误数
static unsigned int g_fault_wait_count = 0U; // 当前Supervisor已注入的waitpid错误数

/****************************** 内部辅助 ******************************/

/**
 * @brief 仅在独立测试标记存在时启用指定故障。
 */
static bool _fault_enabled(const char *expected)
{
    const char *mode;
    const char *marker;

    mode   = getenv("LINKG_SUPERVISOR_FAULT_MODE");
    marker = getenv("LINKG_SUPERVISOR_FAULT_MARKER");
    return mode != NULL && marker != NULL && strcmp(mode, expected) == 0 && access(marker, F_OK) == 0;
}

/****************************** 故障注入 ******************************/

/**
 * @brief 模拟可恢复的信号发送失败，并记录身份未确认时的实际信号操作。
 */
int kill(pid_t process_id, int signal_number)
{
    if (_fault_enabled("kill") && signal_number == SIGTERM && g_fault_kill_count < 2U)
    {
        g_fault_kill_count++;
        fprintf(stderr, "FAULT kill attempt=%u pid=%ld\n", g_fault_kill_count, (long)process_id);
        errno = EPERM;
        return -1;
    }

    if (_fault_enabled("unknown"))
    {
        fprintf(stderr, "FAULT unsafe_signal pid=%ld signal=%d\n", (long)process_id, signal_number);
    }

    return (int)syscall(SYS_kill, process_id, signal_number);
}

/**
 * @brief 模拟三次可恢复的waitpid错误，之后由内核执行真实回收。
 */
pid_t waitpid(pid_t process_id, int *status, int options)
{
    if (_fault_enabled("waitpid") && g_fault_wait_count < 3U)
    {
        g_fault_wait_count++;
        fprintf(stderr, "FAULT waitpid attempt=%u pid=%ld\n", g_fault_wait_count, (long)process_id);
        errno = EAGAIN;
        return -1;
    }

    return (pid_t)syscall(SYS_wait4, process_id, status, options, NULL);
}

/**
 * @brief 持续模拟无法确认子进程归属，直至测试撤销标记。
 */
int waitid(idtype_t type, id_t process_id, siginfo_t *information, int options)
{
    if (_fault_enabled("unknown"))
    {
        fprintf(stderr, "FAULT waitid unknown pid=%ld\n", (long)process_id);
        errno = ECHILD;
        return -1;
    }

    return (int)syscall(SYS_waitid, type, process_id, information, options, NULL);
}

/**
 * @brief 模拟控制消息非阻塞发送失败，验证各阶段边界。
 */
ssize_t write(int descriptor, const void *buffer, size_t count)
{
    if (count == 1U && ((*(const uint8_t *)buffer == 4U && _fault_enabled("stopping-write")) ||
                        (*(const uint8_t *)buffer == 1U && _fault_enabled("ready-write")) ||
                        (*(const uint8_t *)buffer == 3U && _fault_enabled("restarting-write"))))
    {
        errno = EAGAIN;
        return -1;
    }

    return (ssize_t)syscall(SYS_write, descriptor, buffer, count);
}
