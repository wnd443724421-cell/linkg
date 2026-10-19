/**
 * @file supervisor_boot.c
 * @brief 实际Kernel识别、Tina冗余ENV批量交互及重启
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-19
 */

#include "supervisor_boot.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "supervisor_storage.h"

/****************************** 模块常量 ******************************/

#define SUPERVISOR_BOOT_OUTPUT_SIZE 4096U // ENV及cmdline读取上限
#define SUPERVISOR_BOOT_COMMAND_MS  1000U // 单个ENV工具的最长执行时间

/****************************** 内部类型 ******************************/

typedef struct
{
    supervisor_kernel_t target;    // ENV中的下次启动目标，不用于识别实际Kernel
    supervisor_kernel_t stable;    // ENV中的稳定Kernel
    unsigned long       count;     // U-Boot维护的跨复位计数
    unsigned long       upgrading; // 计数持久化及试启动保护开关
} supervisor_boot_env_t;

/****************************** 全局上下文 ******************************/

static supervisor_boot_progress_t g_supervisor_boot_progress = NULL; // 仅确认事务内借用的同步检查回调
static void                      *g_supervisor_boot_context  = NULL; // 回调上下文，不跨事务持有

/****************************** 内部辅助 ******************************/

/**
 * @brief 仅测试构建读取替身路径，缺少替身时拒绝使用真实设备接口。
 */
static const char *_supervisor_boot_path(const char *test_name, const char *production_path)
{
#if defined(LINKG_SUPERVISOR_TEST_BUILD) || defined(LINKG_SUPERVISOR_TEST_IO)
    const char *value;

    (void)production_path;
    value = getenv(test_name);
    return value != NULL && value[0] == '/' ? value : NULL;
#else
    (void)test_name;
    return production_path;
#endif
}

/**
 * @brief 获取辅助命令超时使用的单调时钟。
 */
static uint64_t _supervisor_boot_now(void)
{
    struct timespec value;

    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0)
    {
        return 0U;
    }

    return (uint64_t)value.tv_sec * 1000U + (uint64_t)value.tv_nsec / 1000000U;
}

/**
 * @brief 无Shell执行ENV工具，有界捕获输出并回收辅助进程。
 */
static int _supervisor_boot_command(char *const arguments[], char *output, size_t capacity)
{
    struct pollfd descriptor;
    siginfo_t     information;
    uint64_t      started_ms;
    size_t        length;
    ssize_t       count;
    pid_t         child;
    pid_t         result;
    pid_t         parent;
    int           descriptors[2];
    int           status;
    int           ret;
    int           progress_ret;
    int           progress_error;

    if (arguments[0] == NULL || arguments[0][0] != '/')
    {
        return -EINVAL;
    }

    ret = supervisor_boot_checkpoint();
    if (ret != 0)
    {
        return ret;
    }

    if (pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        return -errno;
    }

    parent = getpid();
    child  = fork();
    if (child == 0)
    {
        close(descriptors[0]);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent ||
            dup2(descriptors[1], STDOUT_FILENO) < 0)
        {
            _exit(127);
        }

        if (descriptors[1] != STDOUT_FILENO)
        {
            close(descriptors[1]);
        }

        execv(arguments[0], arguments);
        _exit(127);
    }

    ret = -errno;
    close(descriptors[1]);
    if (child < 0)
    {
        close(descriptors[0]);
        return ret;
    }

    length             = 0U;
    ret                = 0;
    result             = 0;
    progress_error     = 0;
    started_ms         = _supervisor_boot_now();
    descriptor.fd      = descriptors[0];
    descriptor.events  = POLLIN;
    descriptor.revents = 0;
    for (;;)
    {
        progress_ret = supervisor_boot_checkpoint();
        if (progress_ret != 0 && progress_error == 0)
        {
            // 继续等待本次工具结束，避免因控制通知在ENV擦写途中强杀辅助进程。
            progress_error = progress_ret;
        }

        count = read(descriptors[0], output + length, capacity - 1U - length);
        if (count > 0)
        {
            length += (size_t)count;
            if (length == capacity - 1U)
            {
                ret = -EOVERFLOW;
                break;
            }
        }
        else if (count < 0 && errno != EINTR && errno != EAGAIN)
        {
            ret = -errno;
            break;
        }

        result = waitpid(child, &status, WNOHANG);
        if (result == child)
        {
            while ((count = read(descriptors[0], output + length, capacity - 1U - length)) > 0)
            {
                length += (size_t)count;
            }

            ret = WIFEXITED(status) && WEXITSTATUS(status) == 0 && length < capacity - 1U ? 0 : -EIO;
            if (ret == 0)
            {
                ret = progress_error;
            }
            break;
        }

        if (result < 0 && errno != EINTR)
        {
            ret = -errno;
            break;
        }

        if (started_ms == 0U || _supervisor_boot_now() - started_ms >= SUPERVISOR_BOOT_COMMAND_MS)
        {
            ret = -ETIMEDOUT;
            break;
        }

        poll(&descriptor, 1U, 10);
    }

    if (ret != 0 && result != child)
    {
        memset(&information, 0, sizeof(information));
        result = waitid(P_PID, (id_t)child, &information, WEXITED | WNOHANG | WNOWAIT);
        if (result == 0)
        {
            // 仅向仍归本进程所有且尚未退出的辅助进程发送信号，避免PID误操作。
            if (information.si_pid == 0)
            {
                kill(child, SIGKILL);
            }

            do
            {
                result = waitpid(child, &status, 0);
            }
            while (result < 0 && errno == EINTR);
        }
    }

    output[length] = '\0';
    close(descriptors[0]);
    return ret;
}

/**
 * @brief 解析白名单Kernel名称。
 */
static supervisor_kernel_t _supervisor_boot_kernel(const char *text)
{
    if (strcmp(text, "boot_a") == 0)
    {
        return SUPERVISOR_KERNEL_A;
    }

    return strcmp(text, "boot_b") == 0 ? SUPERVISOR_KERNEL_B : SUPERVISOR_KERNEL_NONE;
}

/**
 * @brief 解析非负十进制计数，不接受溢出或尾随字符。
 */
static int _supervisor_boot_number(const char *text, unsigned long *value)
{
    char *end;

    if (text[0] < '0' || text[0] > '9')
    {
        return -EINVAL;
    }

    errno  = 0;
    *value = strtoul(text, &end, 10);
    return errno == 0 && *end == '\0' ? 0 : -EINVAL;
}

/**
 * @brief 一次读取并验证当前Tina启动命令和全部相关ENV，不改bootlimit。
 */
static int _supervisor_boot_read(supervisor_boot_env_t *environment)
{
    char          output[SUPERVISOR_BOOT_OUTPUT_SIZE];
    char         *arguments[] = {NULL, "boot_partition", "stable_boot_partition", "bootcount", "upgrade_available",
                                 "bootlimit", "bootcmd", "boot_normal", "altbootcmd", NULL};
    char         *line;
    char         *save;
    char         *value;
    unsigned int  found;
    unsigned long limit;
    int           ret;

    arguments[0] = (char *)_supervisor_boot_path("LINKG_SUPERVISOR_TEST_PRINTENV", "/usr/sbin/fw_printenv");
    ret = _supervisor_boot_command(arguments, output, sizeof(output));
    if (ret != 0)
    {
        return ret;
    }

    memset(environment, 0, sizeof(*environment));
    found = 0U;
    for (line = strtok_r(output, "\n", &save); line != NULL; line = strtok_r(NULL, "\n", &save))
    {
        value = strchr(line, '=');
        if (value == NULL)
        {
            return -EINVAL;
        }

        *value++ = '\0';
        if (strcmp(line, "boot_partition") == 0)
        {
            environment->target = _supervisor_boot_kernel(value);
            found |= 1U;
        }
        else if (strcmp(line, "stable_boot_partition") == 0)
        {
            environment->stable = _supervisor_boot_kernel(value);
            found |= 2U;
        }
        else if (strcmp(line, "bootcount") == 0 && _supervisor_boot_number(value, &environment->count) == 0)
        {
            found |= 4U;
        }
        else if (strcmp(line, "upgrade_available") == 0 && _supervisor_boot_number(value, &environment->upgrading) == 0 &&
                 environment->upgrading <= 1UL)
        {
            found |= 8U;
        }
        else if (strcmp(line, "bootlimit") == 0 && _supervisor_boot_number(value, &limit) == 0 && limit > 0UL)
        {
            found |= 16U;
        }
        else if (strcmp(line, "bootcmd") == 0 && strcmp(value, "run setargs_nor boot_normal") == 0)
        {
            found |= 32U;
        }
        else if (strcmp(line, "boot_normal") == 0 &&
                 strcmp(value, "sunxi_flash read 43000000 ${boot_partition};bootm 43000000") == 0)
        {
            found |= 64U;
        }
        else if (strcmp(line, "altbootcmd") == 0 && strstr(value, "setenv boot_partition ${stable_boot_partition}") != NULL &&
                 strstr(value, "setenv upgrade_available 0") != NULL && strstr(value, "setenv bootcount 0") != NULL &&
                 strstr(value, "saveenv") != NULL && strstr(value, "run setargs_nor boot_normal") != NULL)
        {
            found |= 128U;
        }
        else
        {
            return -EPROTO;
        }
    }

    return found == 255U && environment->target != SUPERVISOR_KERNEL_NONE &&
           environment->stable != SUPERVISOR_KERNEL_NONE ? 0 : -EPROTO;
}

/**
 * @brief 用fw_setenv批处理一次提交四项ENV，并读回验证实际持久化值。
 */
static int _supervisor_boot_write(const supervisor_boot_env_t *desired)
{
    supervisor_boot_env_t current;
    char                  temporary[] = "/tmp/linkg-supervisor-env-XXXXXX";
    char                  script[256];
    char                  output[SUPERVISOR_BOOT_OUTPUT_SIZE];
    char                 *arguments[] = {NULL, "-s", temporary, NULL};
    FILE                 *stream;
    int                   descriptor;
    int                   ret;

    ret = _supervisor_boot_read(&current);
    if (ret != 0)
    {
        return ret;
    }

    if (current.target == desired->target && current.stable == desired->stable &&
        current.count == desired->count && current.upgrading == desired->upgrading)
    {
        return 0;
    }

    snprintf(script, sizeof(script), "boot_partition %s\nstable_boot_partition %s\nbootcount %lu\nupgrade_available %lu\n",
             supervisor_version_kernel_name(desired->target), supervisor_version_kernel_name(desired->stable),
             desired->count, desired->upgrading);
    descriptor = mkostemp(temporary, O_CLOEXEC);
    if (descriptor < 0)
    {
        return -errno;
    }

    stream = fdopen(descriptor, "w");
    if (stream == NULL)
    {
        ret = -errno;
        close(descriptor);
        unlink(temporary);
        return ret;
    }

    ret = fputs(script, stream) >= 0 ? 0 : -EIO;
    if (fclose(stream) != 0)
    {
        ret = -errno;
    }

    if (ret == 0)
    {
        arguments[0] = (char *)_supervisor_boot_path("LINKG_SUPERVISOR_TEST_SETENV", "/usr/sbin/fw_setenv");
        ret = _supervisor_boot_command(arguments, output, sizeof(output));
    }

    unlink(temporary);
    if (ret == 0)
    {
        ret = _supervisor_boot_read(&current);
    }

    if (ret == 0 && (current.target != desired->target || current.stable != desired->stable ||
                    current.count != desired->count || current.upgrading != desired->upgrading))
    {
        ret = -EIO;
    }

    return ret;
}

/****************************** 启动状态 ******************************/

/**
 * @brief 设置仅在同步确认事务内有效的进程健康检查，返回后必须清除。
 */
void supervisor_boot_set_progress(supervisor_boot_progress_t progress, void *context)
{
    g_supervisor_boot_progress = progress;
    g_supervisor_boot_context  = context;
}

/**
 * @brief ENV等待期间及稳定JSON提交前重新检查进程状态和心跳。
 */
int supervisor_boot_checkpoint(void)
{
    return g_supervisor_boot_progress == NULL ? 0 : g_supervisor_boot_progress(g_supervisor_boot_context);
}

/**
 * @brief 只从实际内核cmdline识别槽位，缺失或重复时拒绝猜测。
 */
int supervisor_boot_actual(supervisor_kernel_t *kernel)
{
    char        text[SUPERVISOR_BOOT_OUTPUT_SIZE];
    const char *path;
    char       *token;
    char       *save;
    bool        found;
    int         ret;

    *kernel = SUPERVISOR_KERNEL_NONE;
    path = _supervisor_boot_path("LINKG_SUPERVISOR_TEST_CMDLINE", "/proc/cmdline");
    if (path == NULL)
    {
        return -EINVAL;
    }

    ret = supervisor_storage_read(path, text, sizeof(text));
    if (ret != 0)
    {
        return ret;
    }

    found = false;
    for (token = strtok_r(text, " \t\r\n", &save); token != NULL; token = strtok_r(NULL, " \t\r\n", &save))
    {
        if (strncmp(token, "kernel_slot=", 12U) == 0)
        {
            if (found)
            {
                return -EINVAL;
            }

            found   = true;
            *kernel = _supervisor_boot_kernel(token + 12U);
        }
    }

    return found && *kernel != SUPERVISOR_KERNEL_NONE ? 0 : -ENODATA;
}

/**
 * @brief 保持候选Kernel保护及既有bootcount，不在进程重启时清零计数。
 */
int supervisor_boot_trial(supervisor_kernel_t target, supervisor_kernel_t stable)
{
    supervisor_boot_env_t desired;
    int                   ret;

    if (supervisor_version_kernel_name(target) == NULL || supervisor_version_kernel_name(stable) == NULL)
    {
        return -EINVAL;
    }

    ret = _supervisor_boot_read(&desired);
    if (ret != 0)
    {
        return ret;
    }

    desired.target = target;
    desired.stable = stable;
    if (target != stable)
    {
        desired.upgrading = 1UL;
    }

    return _supervisor_boot_write(&desired);
}

/**
 * @brief 确认或恢复稳定Kernel，一次更新槽位、稳定指针及试启动标志。
 */
int supervisor_boot_stable(supervisor_kernel_t kernel)
{
    supervisor_boot_env_t desired;

    if (supervisor_version_kernel_name(kernel) == NULL)
    {
        return -EINVAL;
    }

    memset(&desired, 0, sizeof(desired));
    desired.target = kernel;
    desired.stable = kernel;
    return _supervisor_boot_write(&desired);
}

/**
 * @brief 仅完整稳定ENV快照可证明跨Kernel确认已提交，不能只检查boot_partition。
 */
int supervisor_boot_is_stable(supervisor_kernel_t kernel, bool *stable)
{
    supervisor_boot_env_t environment;
    int                   ret;

    *stable = false;
    ret = _supervisor_boot_read(&environment);
    if (ret == 0)
    {
        *stable = environment.target == kernel && environment.stable == kernel &&
                  environment.count == 0UL && environment.upgrading == 0UL;
    }

    return ret;
}

/**
 * @brief 持久化完成后同步并重启，测试构建只写独立标记。
 */
int supervisor_boot_reboot(void)
{
#if defined(LINKG_SUPERVISOR_TEST_BUILD) || defined(LINKG_SUPERVISOR_TEST_IO)
    const char *path;
    int         ret;

    path = _supervisor_boot_path("LINKG_SUPERVISOR_TEST_REBOOT", NULL);
    if (path == NULL)
    {
        return -EINVAL;
    }

    ret = supervisor_storage_write(path, "reboot\n", 7U);
    return ret == 0 ? 1 : ret;
#else
    sync();
    return reboot(RB_AUTOBOOT) == 0 ? 1 : -errno;
#endif
}
