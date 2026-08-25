/**
 * @file linkg_os.c
 * @brief LinkG操作系统命令执行实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-28
 */

#define _GNU_SOURCE

#include "linkg_os.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <signal.h>
#include <stdint.h>
#include <sys/prctl.h>

#include "linkg_log.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_OS_MAX_ARGC        32U   // 外部程序最大参数数量
#define LINKG_OS_SHELL_MAX       1024U // Shell命令最大长度
#define LINKG_OS_PROCESS_POLL_MS 100U  // 进程退出轮询间隔

/****************************** 日志定义 ******************************/

#define LINKG_OS_LOG_TAG "OS" // 操作系统工具日志标签

#define LINKG_OS_ERROR(fmt, ...) LINKG_LOG_ERROR("%s: " fmt, LINKG_OS_LOG_TAG, ##__VA_ARGS__) // 系统命令错误日志
#define LINKG_OS_WARN(fmt, ...)  LINKG_LOG_WARN("%s: " fmt, LINKG_OS_LOG_TAG, ##__VA_ARGS__)  // 系统命令警告日志
/****************************** 内部辅助 ******************************/

/**
 * @brief 将标准错误重定向到空设备。
 */
static void _redirect_stderr_to_null(void)
{
    int descriptor;

    descriptor = open("/dev/null", O_WRONLY | O_CLOEXEC);
    if (descriptor < 0)
    {
        return;
    }

    if (dup2(descriptor, STDERR_FILENO) < 0)
    {
        (void)close(descriptor);
        return;
    }

    if (descriptor != STDERR_FILENO)
    {
        (void)close(descriptor);
    }
}

/**
 * @brief 阻塞等待并回收指定子进程。
 */
static int _wait_process(pid_t process_id, int *status)
{
    int   local_status;
    pid_t wait_result;

    if (process_id <= 1)
    {
        return -EINVAL;
    }

    if (status == NULL)
    {
        status = &local_status;
    }

    for (;;)
    {
        wait_result = waitpid(process_id, status, 0);
        if (wait_result == process_id)
        {
            return 0;
        }

        if (wait_result < 0 && errno == EINTR)
        {
            continue;
        }

        return wait_result < 0 && errno != 0 ? -errno : -EIO;
    }
}

/****************************** 进程启动辅助 ******************************/

/**
 * @brief 异步执行参数数组指定的外部程序。
 */
static int _spawn_argv(pid_t *process_id, const char *file, char *const argv[])
{
    int      error_pipe[2];
    int      child_error;
    int      result;
    int      status;
    pid_t    child_id;
    ssize_t  io_length;
    pid_t    parent_id;
    sigset_t empty_mask;
    if (process_id == NULL || file == NULL || file[0] == '\0' ||
        argv == NULL || argv[0] == NULL)
    {
        LINKG_OS_ERROR("%s", "invalid spawn arguments");
        return -EINVAL;
    }

    *process_id = (pid_t)-1;

    if (pipe2(error_pipe, O_CLOEXEC) != 0)
    {
        result = errno != 0 ? -errno : -EIO;
        LINKG_OS_ERROR("create exec error pipe failed, file=%s, error=%d", file, result);
        return result;
    }

    parent_id = getpid();

    child_id = fork();
    if (child_id < 0)
    {
        result = errno != 0 ? -errno : -EIO;

        (void)close(error_pipe[0]);
        (void)close(error_pipe[1]);

        LINKG_OS_ERROR("fork failed, file=%s, error=%d", file, result);
        return result;
    }

    if (child_id == 0)
    {
        (void)close(error_pipe[0]);

        /**
         * LinkG父线程终止时，由内核向子进程发送SIGTERM。
         */
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
        {
            child_error = errno != 0 ? errno : EIO;

            do
            {
                io_length = write(error_pipe[1], &child_error, sizeof(child_error));
            }
            while (io_length < 0 && errno == EINTR);

            _exit(127);
        }

        /**
         * 防止父进程在fork完成后、PR_SET_PDEATHSIG设置前已经退出。
         */
        if (getppid() != parent_id)
        {
            child_error = ECHILD;

            do
            {
                io_length = write(error_pipe[1], &child_error, sizeof(child_error));
            }
            while (io_length < 0 && errno == EINTR);

            _exit(127);
        }

        /**
         * 创建独立会话和进程组，避免终端Ctrl+C等信号直接发送给子进程。
         */
        if (setsid() < 0)
        {
            child_error = errno != 0 ? errno : EIO;

            do
            {
                io_length = write(error_pipe[1], &child_error, sizeof(child_error));
            }
            while (io_length < 0 && errno == EINTR);

            _exit(127);
        }

        /**
         * 子进程已经脱离LinkG前台进程组，可以清除继承的信号屏蔽。
         */
        sigemptyset(&empty_mask);
        if (sigprocmask(SIG_SETMASK, &empty_mask, NULL) != 0)
        {
            child_error = errno != 0 ? errno : EIO;

            do
            {
                io_length = write(error_pipe[1], &child_error, sizeof(child_error));
            }
            while (io_length < 0 && errno == EINTR);

            _exit(127);
        }

        execvp(file, argv);

        child_error = errno != 0 ? errno : EIO;

        do
        {
            io_length = write(error_pipe[1], &child_error, sizeof(child_error));
        }
        while (io_length < 0 && errno == EINTR);

        _exit(127);
    }

    (void)close(error_pipe[1]);

    // 等待子进程exec成功或返回明确失败原因。
    do
    {
        io_length = read(error_pipe[0], &child_error, sizeof(child_error));
    }
    while (io_length < 0 && errno == EINTR);

    result = io_length < 0 && errno != 0 ? -errno : -EIO;

    (void)close(error_pipe[0]);

    /**
     * exec成功后，FD_CLOEXEC会自动关闭子进程中的写端，
     * 父进程read返回0。
     */
    if (io_length == 0)
    {
        *process_id = child_id;
        return 0;
    }

    if (io_length == (ssize_t)sizeof(child_error))
    {
        (void)_wait_process(child_id, &status);

        result = child_error != 0 ? -child_error : -EIO;
        LINKG_OS_ERROR("exec failed, file=%s, error=%d", file, result);
        return result;
    }

    /**
     * 通信异常时不能丢失子进程管理权，强制终止并回收。
     */
    (void)kill(child_id, SIGKILL);
    (void)_wait_process(child_id, &status);

    LINKG_OS_ERROR("exec status handshake failed, file=%s, error=%d", file, result);
    return result;
}

/**
 * @brief 异步执行可变参数指定的外部程序。
 */
static int _spawn_va(pid_t *process_id, const char *file, va_list arguments)
{
    char       *argv[LINKG_OS_MAX_ARGC + 1U];
    const char *argument;
    size_t      argc;

    if (process_id == NULL || file == NULL || file[0] == '\0')
    {
        LINKG_OS_ERROR("%s", "invalid spawn file");
        return -EINVAL;
    }

    argc         = 0U;
    argv[argc++] = (char *)file;

    while (argc < LINKG_OS_MAX_ARGC)
    {
        argument = va_arg(arguments, const char *);
        if (argument == NULL)
        {
            argv[argc] = NULL;
            return _spawn_argv(process_id, file, argv);
        }

        argv[argc++] = (char *)argument;
    }

    LINKG_OS_ERROR("too many process arguments, file=%s, max=%u", file, LINKG_OS_MAX_ARGC);
    return -E2BIG;
}

/****************************** 同步执行辅助 ******************************/

/**
 * @brief 执行参数数组指定的外部程序。
 *
 * @note 子进程执行前不调用日志、malloc、stdio等非异步信号安全接口。
 */
static int _run_argv(const char *file, char *const argv[], bool ignore_result)
{
    pid_t process_id;
    int   exit_code;
    int   status;
    int   result;
    int   wait_result;

    if (file == NULL || file[0] == '\0' || argv == NULL || argv[0] == NULL)
    {
        if (!ignore_result)
        {
            LINKG_OS_ERROR("%s", "invalid process arguments");
        }

        return -EINVAL;
    }

    process_id = fork();
    if (process_id < 0) // 子进程创建失败。
    {
        result = (errno != 0) ? -errno : -EIO;

        if (!ignore_result)
        {
            LINKG_OS_ERROR("fork failed, file=%s, error=%d", file, result);
        }

        return result;
    }

    if (process_id == 0) // 子进程执行分支。
    {
        if (ignore_result)
        {
            _redirect_stderr_to_null();
        }

        execvp(file, argv);
        _exit(127);
    }

    for (;;)
    {
        wait_result = waitpid(process_id, &status, 0);

        if (wait_result == process_id)
        {
            // 子进程已经结束，status中包含结束状态。
            break;
        }

        if (wait_result < 0 && errno == EINTR)
        {
            // 等待被信号打断，重新等待。
            continue;
        }

        result = (wait_result < 0 && errno != 0) ? -errno : -EIO;

        if (!ignore_result)
        {
            LINKG_OS_ERROR("wait process failed, file=%s, pid=%ld, error=%d",
                        file,
                        (long)process_id,
                        result);
        }

        return result;
    }

    if (WIFEXITED(status))
    {
        exit_code = WEXITSTATUS(status);
        if (exit_code == 0)
        {
            return 0;
        }

        if (!ignore_result)
        {
            LINKG_OS_ERROR("process failed, file=%s, exit=%d", file, exit_code);
        }

        return (exit_code == 127) ? -ENOENT : -EIO;
    }

    if (WIFSIGNALED(status))
    {
        if (!ignore_result)
        {
            LINKG_OS_ERROR("process terminated, file=%s, signal=%d",
                           file,
                           WTERMSIG(status));
        }

        return -EIO;
    }

    if (!ignore_result)
{
    LINKG_OS_ERROR("unexpected process status, file=%s, status=0x%x", file, (unsigned int)status);
}


    return -EIO;
}

/**
 * @brief 执行可变参数指定的外部程序。
 */
static int _run_va(const char *file, va_list arguments, bool ignore_result)
{
    char       *argv[LINKG_OS_MAX_ARGC + 1U];
    const char *argument;
    size_t      argc;

    if (file == NULL || file[0] == '\0')
    {
        if (!ignore_result)
        {
            LINKG_OS_ERROR("%s", "invalid process file");
        }

        return -EINVAL;
    }

    argc         = 0U;
    argv[argc++] = (char *)file;

    while (argc < LINKG_OS_MAX_ARGC)
    {
        argument = va_arg(arguments, const char *);
        if (argument == NULL)
        {
            argv[argc] = NULL;
            return _run_argv(file, argv, ignore_result);
        }

        argv[argc++] = (char *)argument;
    }

    if (!ignore_result)
    {
        LINKG_OS_ERROR("too many process arguments, file=%s, max=%u",
                       file,
                       LINKG_OS_MAX_ARGC);
    }

    return -E2BIG;
}

/****************************** 程序执行 ******************************/

/**
 * @brief 执行外部程序并等待其结束。
 */
int  linkg_os_run(const char *file, ...)
{
    va_list arguments;
    int     result;

    va_start(arguments, file);
    result = _run_va(file, arguments, false);
    va_end(arguments);

    return result;
}

/**
 * @brief 执行外部程序并忽略退出状态。
 */
void linkg_os_run_ignore(const char *file, ...)
{
    va_list arguments;

    va_start(arguments, file);
    (void)_run_va(file, arguments, true);
    va_end(arguments);
}

/**
 * @brief 通过Shell执行格式化命令。
 */
int  linkg_os_shellf(const char *format, ...)
{
    char    command[LINKG_OS_SHELL_MAX];
    char   *argv[4];
    va_list arguments;
    int     formatted_length;

    if (format == NULL)
    {
        LINKG_OS_ERROR("%s", "invalid shell command format");
        return -EINVAL;
    }

    va_start(arguments, format);
    formatted_length = vsnprintf(command, sizeof(command), format, arguments);
    va_end(arguments);

    if (formatted_length < 0)
    {
        LINKG_OS_ERROR("%s", "format shell command failed");
        return -EIO;
    }

    if ((size_t)formatted_length >= sizeof(command))
    {
        LINKG_OS_ERROR("shell command too long, max=%u", LINKG_OS_SHELL_MAX - 1U);
        return -E2BIG;
    }

    argv[0] = "sh";
    argv[1] = "-c";
    argv[2] = command;
    argv[3] = NULL;

    return _run_argv("/bin/sh", argv, false);
}

/**
 * @brief 异步启动外部程序。
 */
int  linkg_os_spawn(pid_t *process_id, const char *file, ...)
{
    va_list arguments;
    int     result;

    va_start(arguments, file);
    result = _spawn_va(process_id, file, arguments);
    va_end(arguments);

    return result;
}

/****************************** 进程管理 ******************************/

/**
 * @brief 检查子进程是否仍在运行。
 */
int  linkg_os_process_running(pid_t *process_id, bool *running)
{
    int   status;
    int   result;
    pid_t wait_result;

    if (process_id == NULL || running == NULL)
    {
        LINKG_OS_ERROR("%s", "invalid process state arguments");
        return -EINVAL;
    }

    *running = false;

    if (*process_id <= 1)
    {
        *process_id = (pid_t)-1;
        return 0;
    }

    for (;;)
    {
        wait_result = waitpid(*process_id, &status, WNOHANG);
        if (wait_result == 0)
        {
            *running = true;
            return 0;
        }

        if (wait_result == *process_id)
        {
            *process_id = (pid_t)-1;
            return 0;
        }

        if (wait_result < 0 && errno == EINTR)
        {
            continue;
        }

        if (wait_result < 0 && errno == ECHILD)
        {
            LINKG_OS_WARN("process is no longer a child, pid=%ld", (long)*process_id);
            *process_id = (pid_t)-1;
            return 0;
        }

        result = wait_result < 0 && errno != 0 ? -errno : -EIO;

        LINKG_OS_ERROR("check process failed, pid=%ld, error=%d",
                       (long)*process_id,
                       result);

        return result;
    }
}

/**
 * @brief 停止并回收子进程。
 */
int  linkg_os_process_stop(pid_t *process_id, uint32_t timeout_ms)
{
    bool     running;
    uint32_t elapsed_ms;
    uint32_t sleep_ms;
    pid_t    target_id;
    int      result;
    int      status;

    if (process_id == NULL)
    {
        LINKG_OS_ERROR("%s", "invalid process ID pointer");
        return -EINVAL;
    }

    result = linkg_os_process_running(process_id, &running);
    if (result != 0 || !running)
    {
        return result;
    }

    target_id = *process_id;

    if (kill(target_id, SIGTERM) != 0 && errno != ESRCH)
    {
        result = errno != 0 ? -errno : -EIO;

        LINKG_OS_ERROR("send SIGTERM failed, pid=%ld, error=%d",
                       (long)target_id,
                       result);

        return result;
    }

    elapsed_ms = 0U;

    while (elapsed_ms < timeout_ms)
    {
        result = linkg_os_process_running(process_id, &running);
        if (result != 0)
        {
            return result;
        }

        if (!running)
        {
            return 0;
        }

        sleep_ms = timeout_ms - elapsed_ms;
        if (sleep_ms > LINKG_OS_PROCESS_POLL_MS)
        {
            sleep_ms = LINKG_OS_PROCESS_POLL_MS;
        }

        result = linkg_time_sleep_ms(sleep_ms);
        if (result != 0)
        {
            return result;
        }

        elapsed_ms += sleep_ms;
    }

    /**
     * 超时边界再检查一次，避免进程恰好已经退出却发送SIGKILL。
     */
    result = linkg_os_process_running(process_id, &running);
    if (result != 0 || !running)
    {
        return result;
    }

    target_id = *process_id;

    LINKG_OS_WARN("process did not exit after SIGTERM, forcing stop, pid=%ld",
                  (long)target_id);

    if (kill(target_id, SIGKILL) != 0 && errno != ESRCH)
    {
        result = errno != 0 ? -errno : -EIO;

        LINKG_OS_ERROR("send SIGKILL failed, pid=%ld, error=%d",
                       (long)target_id,
                       result);

        return result;
    }

    result = _wait_process(target_id, &status);
    if (result != 0 && result != -ECHILD)
    {
        LINKG_OS_ERROR("reap process failed, pid=%ld, error=%d",
                       (long)target_id,
                       result);

        return result;
    }

    *process_id = (pid_t)-1;
    return 0;
}
