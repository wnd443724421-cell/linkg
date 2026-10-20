/**
 * @file release_faults.c
 * @brief 仅测试使用的持久化及ENV辅助等待故障注入
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-20
 */

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查独立测试目录的故障标记，不接触真实产品路径。
 */
static bool _release_fault_enabled(const char *marker, const char *path, const char *suffix)
{
    const char *root;
    char        expected[PATH_MAX];
    char        control[PATH_MAX];

    root = getenv("LINKG_RELEASE_TEST_ROOT");
    if (root == NULL)
    {
        return false;
    }

    snprintf(expected, sizeof(expected), "%s%s", root, suffix);
    snprintf(control, sizeof(control), "%s/%s", root, marker);
    return strcmp(path, expected) == 0 && access(control, F_OK) == 0;
}

/****************************** 故障注入 ******************************/

/**
 * @brief 仅对指定写工具注入一次waitpid错误，不影响LinkG回收。
 */
pid_t waitpid(pid_t process_id, int *status, int options)
{
    static bool injected = false; // 每个测试Supervisor只注入一次辅助写进程等待错误
    const char *root;
    char        path[PATH_MAX];
    FILE       *stream;
    long        writer;

    root = getenv("LINKG_RELEASE_TEST_ROOT");
    if (!injected && _release_fault_enabled("fail-env-wait", root, ""))
    {
        snprintf(path, sizeof(path), "%s/writer.pid", root);
        stream = fopen(path, "r");
        if (stream != NULL)
        {
            writer = -1;
            if (fscanf(stream, "%ld", &writer) == 1 && writer == (long)process_id)
            {
                injected = true;
            }

            fclose(stream);
            if (injected)
            {
                errno = EIO;
                return -1;
            }
        }
    }

    return (pid_t)syscall(SYS_wait4, process_id, status, options, NULL);
}

/**
 * @brief 模拟写工具等待时poll失败，要求产品路径继续等待且不忙循环。
 */
int poll(struct pollfd *descriptors, nfds_t count, int timeout)
{
    const char *root;

    root = getenv("LINKG_RELEASE_TEST_ROOT");
    if (count == 0U && _release_fault_enabled("fail-env-poll", root, "") &&
        _release_fault_enabled("write-active", root, ""))
    {
        errno = EIO;
        return -1;
    }

    return (int)syscall(SYS_poll, descriptors, count, timeout);
}

/**
 * @brief 在指定持久化边界返回错误，软链接只阻断向v2的切换而允许恢复v1。
 */
int rename(const char *source, const char *destination)
{
    char    target[PATH_MAX];
    ssize_t length;

    if (_release_fault_enabled("fail-json", destination, "/app/version_state.json"))
    {
        errno = EIO;
        return -1;
    }

    if (_release_fault_enabled("fail-link", destination, "/www/current"))
    {
        length = readlink(source, target, sizeof(target) - 1U);
        if (length > 0)
        {
            target[length] = '\0';
            if (length >= 3 && strcmp(target + length - 3, "/v2") == 0)
            {
                errno = EIO;
                return -1;
            }
        }
    }

    return (int)syscall(SYS_rename, source, destination);
}

/**
 * @brief 模拟rename已经生效但父目录同步失败。
 */
int fsync(int descriptor)
{
    char    descriptor_path[64];
    char    path[PATH_MAX];
    ssize_t length;

    snprintf(descriptor_path, sizeof(descriptor_path), "/proc/self/fd/%d", descriptor);
    length = readlink(descriptor_path, path, sizeof(path) - 1U);
    if (length >= 0)
    {
        path[length] = '\0';
        if (_release_fault_enabled("fail-dir-sync", path, "/app"))
        {
            errno = EIO;
            return -1;
        }
    }

    return (int)syscall(SYS_fsync, descriptor);
}
