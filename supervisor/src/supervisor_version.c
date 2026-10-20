/**
 * @file supervisor_version.c
 * @brief Supervisor版本接口及Release目录验证
 * @author Dawn
 * @version 0.3.0
 * @date 2026-10-20
 */

#include "supervisor_version.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************** 状态文件 ******************************/

/**
 * @brief 使用共用严格解析器读取四字段或五字段版本状态。
 */
int supervisor_version_load(const char *path, supervisor_version_t *version)
{
    return linkg_release_load(path, version);
}

/**
 * @brief 原子保存完整状态，仅供已串行化的初始化操作使用。
 */
int supervisor_version_save(const char *path, const supervisor_version_t *version)
{
    return linkg_release_save(path, version);
}

/**
 * @brief 共享锁下比较并提交状态，避免覆盖LinkG刚写入的字段。
 */
int supervisor_version_replace(const char *path, const supervisor_version_t *expected, const supervisor_version_t *desired)
{
    return linkg_release_replace(path, expected, desired);
}

/**
 * @brief 比较映射、目标、稳定版本及升级状态。
 */
bool supervisor_version_equal(const supervisor_version_t *left, const supervisor_version_t *right)
{
    return linkg_release_equal(left, right);
}

/**
 * @brief 获取固定Release名称。
 */
const char *supervisor_version_name(unsigned int release)
{
    return linkg_release_name(release);
}

/**
 * @brief 获取固定Kernel名称。
 */
const char *supervisor_version_kernel_name(supervisor_kernel_t kernel)
{
    return linkg_release_kernel_name(kernel);
}

/****************************** 版本选择 ******************************/

/**
 * @brief 验证App和Web安装目录以及LinkG可执行文件。
 */
int supervisor_version_check(const char *app_root, const char *web_root, unsigned int release)
{
    const char *roots[2] = {app_root, web_root};
    struct stat information;
    char        path[PATH_MAX];
    int         index;
    int         length;

    if (release >= 2U)
    {
        return -EINVAL;
    }

    for (index = 0; index < 2; index++)
    {
        length = snprintf(path, sizeof(path), "%s/releases/%s", roots[index], supervisor_version_name(release));
        if (length < 0 || (size_t)length >= sizeof(path))
        {
            return -ENAMETOOLONG;
        }

        if (stat(path, &information) != 0)
        {
            return -errno;
        }

        if (!S_ISDIR(information.st_mode))
        {
            return -ENOTDIR;
        }
    }

    length = snprintf(path, sizeof(path), "%s/releases/%s/bin/linkg", app_root, supervisor_version_name(release));
    if (length < 0 || (size_t)length >= sizeof(path))
    {
        return -ENAMETOOLONG;
    }

    if (stat(path, &information) != 0)
    {
        return -errno;
    }

    if (!S_ISREG(information.st_mode))
    {
        return -EINVAL;
    }

    return access(path, X_OK) == 0 ? 0 : -errno;
}

/**
 * @brief 构造唯一current程序路径，供进程模块复用原有exec流程。
 */
int supervisor_version_program(const char *app_root, char *program, size_t capacity)
{
    int length;

    length = snprintf(program, capacity, "%s/current/bin/linkg", app_root);
    return length >= 0 && (size_t)length < capacity ? 0 : -ENAMETOOLONG;
}
