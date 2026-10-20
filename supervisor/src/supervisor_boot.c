/**
 * @file supervisor_boot.c
 * @brief 实际Kernel识别、Tina冗余ENV批量交互及重启
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-20
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
#include <sys/file.h>
#include <sys/prctl.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "supervisor_config.h"
#include "supervisor_storage.h"

/****************************** 模块常量 ******************************/

#define SUPERVISOR_BOOT_OUTPUT_SIZE 4096U // ENV及cmdline读取上限

/****************************** 内部类型 ******************************/

typedef enum
{
    SUPERVISOR_BOOT_READ  = 0, // 可超时终止的只读命令
    SUPERVISOR_BOOT_WRITE      // 必须等待退出的Flash写入命令
} supervisor_boot_command_t;

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
 * @brief 限制等待循环频率，poll失败时仍避免忙循环。
 */
static void _supervisor_boot_pause(void)
{
    struct timespec delay;

    if (poll(NULL, 0U, SUPERVISOR_ENV_POLL_MS) < 0 && errno != EINTR)
    {
        delay.tv_sec  = 0;
        delay.tv_nsec = (long)SUPERVISOR_ENV_POLL_MS * 1000000L;
        nanosleep(&delay, NULL);
    }
}

/**
 * @brief 获取跨实例ENV事务锁，遗留写入未结束时等待且不删除锁文件。
 */
static int _supervisor_boot_lock(void)
{
    struct stat status;
    const char *path;
    uint64_t    started_ms;
    uint64_t    last_warning_ms;
    uint64_t    now_ms;
    int         descriptor;
    int         duplicate;
    int         ret;
    bool        warned;

    path = _supervisor_boot_path("LINKG_SUPERVISOR_TEST_ENV_LOCK", SUPERVISOR_ENV_LOCK_FILE);
    if (path == NULL)
    {
        return -EINVAL;
    }

    descriptor = open(path, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (descriptor < 0)
    {
        return -errno;
    }

    if (descriptor <= STDERR_FILENO)
    {
        duplicate = fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        ret = -errno;
        close(descriptor);
        if (duplicate < 0)
        {
            return ret;
        }

        descriptor = duplicate;
    }

    if (fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode))
    {
        close(descriptor);
        return -EINVAL;
    }

    started_ms      = _supervisor_boot_now();
    last_warning_ms = started_ms;
    warned          = false;
    for (;;)
    {
        ret = supervisor_boot_checkpoint();
        if (ret != 0)
        {
            close(descriptor);
            return ret;
        }

        if (flock(descriptor, LOCK_EX | LOCK_NB) == 0)
        {
            return descriptor;
        }

        ret = -errno;
        if (errno != EWOULDBLOCK && errno != EAGAIN && errno != EINTR)
        {
            close(descriptor);
            return ret;
        }

        now_ms = _supervisor_boot_now();
        if ((!warned && now_ms - started_ms >= SUPERVISOR_ENV_WRITE_WARN_MS) ||
            (warned && now_ms - last_warning_ms >= SUPERVISOR_ENV_WAIT_LOG_MS))
        {
            fprintf(stderr, "Supervisor WARN ENV lock waiting; previous operation still owns lock\n");
            last_warning_ms = now_ms;
            warned = true;
        }

        _supervisor_boot_pause();
    }
}

/**
 * @brief 写入工具继承锁且不依赖父进程或输出管道，读取工具保留死亡清理。
 */
static void _supervisor_boot_exec(char *const arguments[], supervisor_boot_command_t type, int lock_fd, int read_fd, int write_fd, pid_t parent)
{
    const int        signals[] = {SIGINT, SIGTERM, SIGQUIT, SIGPIPE};
    struct sigaction action;
    int              null_fd;
    size_t           index;

    if (type == SUPERVISOR_BOOT_READ)
    {
        close(read_fd);
        close(lock_fd);
        if (prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || getppid() != parent || dup2(write_fd, STDOUT_FILENO) < 0)
        {
            _exit(127);
        }

        if (write_fd != STDOUT_FILENO)
        {
            close(write_fd);
        }
    }
    else
    {
        // 不设置父进程死亡信号；父进程被强杀后，工具仍持有同一open-file-description的flock。
        if (prctl(PR_SET_PDEATHSIG, 0) != 0 || fcntl(lock_fd, F_SETFD, 0) != 0)
        {
            _exit(127);
        }

        memset(&action, 0, sizeof(action));
        action.sa_handler = SIG_IGN;
        sigemptyset(&action.sa_mask);
        for (index = 0U; index < sizeof(signals) / sizeof(signals[0]); index++)
        {
            if (sigaction(signals[index], &action, NULL) != 0)
            {
                _exit(127);
            }
        }

        // 写命令不需要解析输出，避免父进程退出后SIGPIPE或输出管道阻塞中断Flash操作。
        null_fd = open("/dev/null", O_RDWR);
        if (null_fd < 0 || dup2(null_fd, STDIN_FILENO) < 0 || dup2(null_fd, STDOUT_FILENO) < 0 || dup2(null_fd, STDERR_FILENO) < 0)
        {
            _exit(127);
        }

        if (null_fd > STDERR_FILENO)
        {
            close(null_fd);
        }
    }

    execv(arguments[0], arguments);
    _exit(127);
}

/**
 * @brief 写入只做耗时告警，持续检查LinkG并等待真实退出，绝不主动发送终止信号。
 */
static int _supervisor_boot_wait_write(pid_t child)
{
    uint64_t started_ms;
    uint64_t last_warning_ms;
    uint64_t now_ms;
    pid_t    result;
    int      status;
    int      ret;
    int      error;
    bool     wait_error;
    bool     warned;

    started_ms      = _supervisor_boot_now();
    last_warning_ms = started_ms;
    warned          = false;
    wait_error      = false;
    ret             = 0;
    for (;;)
    {
        error = supervisor_boot_checkpoint();
        if (error != 0 && ret == 0)
        {
            ret = error;
        }

        result = waitpid(child, &status, WNOHANG);
        if (result == child)
        {
            fprintf(stderr, "Supervisor ENV write finished pid=%ld elapsed_ms=%llu status=%d\n", (long)child,
                    (unsigned long long)(_supervisor_boot_now() - started_ms), status);
            return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? ret : -EIO;
        }

        if (result < 0 && errno != EINTR)
        {
            // 无法确认回收时保持锁和等待，不将监控错误转化为强杀或写入成功。
            error = -errno;
            if (!wait_error)
            {
                fprintf(stderr, "Supervisor WARN ENV write waitpid pid=%ld error=%d; retaining lock and waiting\n", (long)child, error);
                wait_error = true;
            }

            if (ret == 0)
            {
                ret = error;
            }
        }

        now_ms = _supervisor_boot_now();
        if ((!warned && now_ms - started_ms >= SUPERVISOR_ENV_WRITE_WARN_MS) ||
            (warned && now_ms - last_warning_ms >= SUPERVISOR_ENV_WAIT_LOG_MS))
        {
            fprintf(stderr, "Supervisor WARN ENV write still running pid=%ld elapsed_ms=%llu error=%d\n", (long)child,
                    (unsigned long long)(now_ms - started_ms), ret);
            last_warning_ms = now_ms;
            warned = true;
        }

        _supervisor_boot_pause();
    }
}

/**
 * @brief 读取命令有界捕获输出，超时或监控失败时安全终止并回收只读工具。
 */
static int _supervisor_boot_wait_read(pid_t child, int read_fd, char *output, size_t capacity)
{
    struct pollfd descriptor;
    siginfo_t     information;
    uint64_t      started_ms;
    size_t        length;
    ssize_t       count;
    pid_t         result;
    int           status;
    int           ret;
    int           progress_ret;
    int           progress_error;

    length             = 0U;
    ret                = 0;
    result             = 0;
    progress_error     = 0;
    started_ms         = _supervisor_boot_now();
    descriptor.fd      = read_fd;
    descriptor.events  = POLLIN;
    descriptor.revents = 0;
    for (;;)
    {
        progress_ret = supervisor_boot_checkpoint();
        if (progress_ret != 0 && progress_error == 0)
        {
            // 记录读取期间的健康变化，完成工具回收后再交由调用方处理。
            progress_error = progress_ret;
        }

        count = read(read_fd, output + length, capacity - 1U - length);
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
            while ((count = read(read_fd, output + length, capacity - 1U - length)) > 0)
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

        if (started_ms == 0U || _supervisor_boot_now() - started_ms >= SUPERVISOR_ENV_READ_MS)
        {
            ret = -ETIMEDOUT;
            fprintf(stderr, "Supervisor ENV read timeout pid=%ld\n", (long)child);
            break;
        }

        if (poll(&descriptor, 1U, 10) < 0 && errno != EINTR)
        {
            ret = -errno;
            break;
        }

        if ((descriptor.revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
        {
            _supervisor_boot_pause();
        }
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
    return ret;
}

/**
 * @brief 无Shell启动明确类型的ENV工具，锁由调用方持有到读回验证结束。
 */
static int _supervisor_boot_command(char *const arguments[], supervisor_boot_command_t type, int lock_fd, char *output, size_t capacity)
{
    int   descriptors[2] = {-1, -1};
    pid_t parent;
    pid_t child;
    int   ret;

    if (arguments[0] == NULL || arguments[0][0] != '/' || capacity == 0U)
    {
        return -EINVAL;
    }

    ret = supervisor_boot_checkpoint();
    if (ret != 0)
    {
        return ret;
    }

    if (type == SUPERVISOR_BOOT_READ && pipe2(descriptors, O_CLOEXEC | O_NONBLOCK) != 0)
    {
        return -errno;
    }

    parent = getpid();
    child  = fork();
    if (child == 0)
    {
        _supervisor_boot_exec(arguments, type, lock_fd, descriptors[0], descriptors[1], parent);
    }

    ret = -errno;
    if (descriptors[1] >= 0)
    {
        close(descriptors[1]);
    }

    if (child > 0)
    {
        ret = type == SUPERVISOR_BOOT_WRITE ? _supervisor_boot_wait_write(child) :
              _supervisor_boot_wait_read(child, descriptors[0], output, capacity);
    }

    if (descriptors[0] >= 0)
    {
        close(descriptors[0]);
    }

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
static int _supervisor_boot_read(supervisor_boot_env_t *environment, int lock_fd)
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
    ret = _supervisor_boot_command(arguments, SUPERVISOR_BOOT_READ, lock_fd, output, sizeof(output));
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
static int _supervisor_boot_write(const supervisor_boot_env_t *desired, int lock_fd)
{
    supervisor_boot_env_t current;
    char                  temporary[] = "/tmp/linkg-supervisor-env-XXXXXX";
    char                  script[256];
    char                  output[SUPERVISOR_BOOT_OUTPUT_SIZE];
    char                 *arguments[] = {NULL, "-s", temporary, NULL};
    FILE                 *stream;
    int                   descriptor;
    int                   ret;

    ret = _supervisor_boot_read(&current, lock_fd);
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
        ret = _supervisor_boot_command(arguments, SUPERVISOR_BOOT_WRITE, lock_fd, output, sizeof(output));
    }

    unlink(temporary);
    if (ret == 0)
    {
        ret = _supervisor_boot_read(&current, lock_fd);
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
    int                   lock_fd;
    int                   ret;

    if (supervisor_version_kernel_name(target) == NULL || supervisor_version_kernel_name(stable) == NULL)
    {
        return -EINVAL;
    }

    lock_fd = _supervisor_boot_lock();
    if (lock_fd < 0)
    {
        return lock_fd;
    }

    ret = _supervisor_boot_read(&desired, lock_fd);
    if (ret == 0)
    {
        desired.target = target;
        desired.stable = stable;
        if (target != stable)
        {
            desired.upgrading = 1UL;
        }

        ret = _supervisor_boot_write(&desired, lock_fd);
    }

    close(lock_fd);
    return ret;
}

/**
 * @brief 确认或恢复稳定Kernel，一次更新槽位、稳定指针及试启动标志。
 */
int supervisor_boot_stable(supervisor_kernel_t kernel)
{
    supervisor_boot_env_t desired;
    int                   lock_fd;
    int                   ret;

    if (supervisor_version_kernel_name(kernel) == NULL)
    {
        return -EINVAL;
    }

    memset(&desired, 0, sizeof(desired));
    desired.target = kernel;
    desired.stable = kernel;
    lock_fd = _supervisor_boot_lock();
    if (lock_fd < 0)
    {
        return lock_fd;
    }

    ret = _supervisor_boot_write(&desired, lock_fd);
    // 不显式LOCK_UN：写工具继承同一锁描述符，只有全部持有者关闭才释放。
    close(lock_fd);
    return ret;
}

/**
 * @brief 仅完整稳定ENV快照可证明跨Kernel确认已提交，不能只检查boot_partition。
 */
int supervisor_boot_is_stable(supervisor_kernel_t kernel, bool *stable)
{
    supervisor_boot_env_t environment;
    int                   lock_fd;
    int                   ret;

    *stable = false;
    lock_fd = _supervisor_boot_lock();
    if (lock_fd < 0)
    {
        return lock_fd;
    }

    ret = _supervisor_boot_read(&environment, lock_fd);
    if (ret == 0)
    {
        *stable = environment.target == kernel && environment.stable == kernel &&
                  environment.count == 0UL && environment.upgrading == 0UL;
    }

    close(lock_fd);
    return ret;
}

/**
 * @brief 持久化完成后同步并重启，测试构建只写独立标记。
 */
int supervisor_boot_reboot(void)
{
    int ret;

    ret = supervisor_boot_checkpoint();
    if (ret != 0)
    {
        return ret;
    }

#if defined(LINKG_SUPERVISOR_TEST_BUILD) || defined(LINKG_SUPERVISOR_TEST_IO)
    const char *path;

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
