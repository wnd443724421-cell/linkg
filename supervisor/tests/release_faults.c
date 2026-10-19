/**
 * @file release_faults.c
 * @brief 仅测试使用的JSON及软链接持久化故障注入
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-19
 */

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
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
