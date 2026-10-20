/**
 * @file upgrade_archive.c
 * @brief 使用系统tar解压LinkG升级包
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#define _GNU_SOURCE

#include "upgrade_archive.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/****************************** 模块常量 ******************************/

#define UPGRADE_ARCHIVE_TAR_PATH      "/bin/tar"              // Tina Linux的BusyBox tar入口
#define UPGRADE_ARCHIVE_ROOT_NAME     "upgrade/"             // 升级包唯一顶层目录
#define UPGRADE_ARCHIVE_MANIFEST_NAME "upgrade/FILES.txt"   // 升级包清单文件
#define UPGRADE_ARCHIVE_MAX_PACKAGE   (64U * 1024U * 1024U) // 压缩包大小上限
#define UPGRADE_ARCHIVE_MAX_ENTRIES   2048U                  // 归档成员数量上限

/****************************** 内部辅助 ******************************/

/**
 * @brief 等待tar进程结束并检查退出状态。
 */
static int _upgrade_archive_wait(pid_t pid)
{
    int status;
    int ret;

    do
    {
        ret = waitpid(pid, &status, 0);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        return -errno;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EBADMSG;
}

/**
 * @brief 检查归档成员路径必须位于upgrade目录内，且不能包含危险路径段。
 */
static bool _upgrade_archive_valid_name(const char *name)
{
    const char *segment;
    const char *cursor;
    size_t      length;

    if (strncmp(name, UPGRADE_ARCHIVE_ROOT_NAME, sizeof(UPGRADE_ARCHIVE_ROOT_NAME) - 1U) != 0)
    {
        return false;
    }

    segment = name + sizeof(UPGRADE_ARCHIVE_ROOT_NAME) - 1U;
    if (*segment == '\0')
    {
        return true;
    }

    for (cursor = segment; ; ++cursor)
    {
        if (*cursor != '\0' && (unsigned char)*cursor < 32U)
        {
            return false;
        }

        if (*cursor != '/' && *cursor != '\0')
        {
            continue;
        }

        length = (size_t)(cursor - segment);
        if ((length == 0U && *cursor != '\0') ||
            (length == 1U && segment[0] == '.') ||
            (length == 2U && segment[0] == '.' && segment[1] == '.'))
        {
            return false;
        }

        if (*cursor == '\0')
        {
            return true;
        }

        segment = cursor + 1;
        if (*segment == '\0')
        {
            return true;
        }
    }
}

/**
 * @brief 调用tar列出成员，确认升级包的顶层目录及基本路径结构。
 */
static int _upgrade_archive_check(const char *package_path)
{
    char          line[PATH_MAX + 2U];
    struct stat   info;
    FILE         *stream;
    pid_t         pid;
    size_t        length;
    unsigned int  count;
    bool          root_found;
    bool          manifest_found;
    int           descriptors[2];
    int           ret;
    int           child_ret;

    if (lstat(package_path, &info) != 0)
    {
        return -errno;
    }

    if (!S_ISREG(info.st_mode) || info.st_size <= 0)
    {
        return -EINVAL;
    }

    if ((uint64_t)info.st_size > UPGRADE_ARCHIVE_MAX_PACKAGE)
    {
        return -EFBIG;
    }

    if (pipe(descriptors) != 0)
    {
        return -errno;
    }

    pid = fork();
    if (pid < 0)
    {
        ret = -errno;
        close(descriptors[0]);
        close(descriptors[1]);
        return ret;
    }

    if (pid == 0)
    {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0)
        {
            _exit(127);
        }

        close(descriptors[1]);
        execl(UPGRADE_ARCHIVE_TAR_PATH, "tar", "-tzf", package_path, (char *)NULL);
        _exit(127);
    }

    close(descriptors[1]);
    stream = fdopen(descriptors[0], "r");
    if (stream == NULL)
    {
        ret = -errno;
        close(descriptors[0]);
        (void)_upgrade_archive_wait(pid);
        return ret;
    }

    ret            = 0;
    count          = 0U;
    root_found     = false;
    manifest_found = false;

    while (fgets(line, sizeof(line), stream) != NULL)
    {
        length = strlen(line);
        if (length == 0U || line[length - 1U] != '\n')
        {
            ret = -EBADMSG;
            break;
        }

        line[length - 1U] = '\0';
        if (!_upgrade_archive_valid_name(line) || ++count > UPGRADE_ARCHIVE_MAX_ENTRIES)
        {
            ret = -EBADMSG;
            break;
        }

        root_found     |= strcmp(line, UPGRADE_ARCHIVE_ROOT_NAME) == 0;
        manifest_found |= strcmp(line, UPGRADE_ARCHIVE_MANIFEST_NAME) == 0;
    }

    if (ret == 0 && ferror(stream))
    {
        ret = -EIO;
    }

    fclose(stream);
    child_ret = _upgrade_archive_wait(pid);
    if (ret != 0)
    {
        return ret;
    }

    if (child_ret != 0)
    {
        return child_ret;
    }

    return root_found && manifest_found ? 0 : -EBADMSG;
}

/**
 * @brief 调用BusyBox tar解压到指定目录。
 */
static int _upgrade_archive_unpack(const char *package_path, const char *output_dir)
{
    pid_t pid;

    pid = fork();
    if (pid < 0)
    {
        return -errno;
    }

    if (pid == 0)
    {
        execl(UPGRADE_ARCHIVE_TAR_PATH, "tar", "-xzof", package_path,
              "-C", output_dir, "-k", (char *)NULL);
        _exit(127);
    }

    return _upgrade_archive_wait(pid);
}

/****************************** 归档解压 ******************************/

/**
 * @brief 检查升级包根目录并解压，在output_dir下生成upgrade目录。
 *
 * output_dir必须为已存在的可信绝对目录；其中不能存在upgrade对象。
 * 本模块不验证FILES.txt的SHA-256，也不负责清理解压产物。
 */
int upgrade_archive_extract(const char *package_path, const char *output_dir)
{
    char        root_path[PATH_MAX];
    struct stat info;
    int         length;
    int         ret;

    if (package_path == NULL || output_dir == NULL || output_dir[0] != '/')
    {
        return -EINVAL;
    }

    if (stat(output_dir, &info) != 0)
    {
        return -errno;
    }

    if (!S_ISDIR(info.st_mode))
    {
        return -ENOTDIR;
    }

    length = snprintf(root_path, sizeof(root_path), "%s/upgrade", output_dir);
    if (length < 0 || (size_t)length >= sizeof(root_path))
    {
        return -ENAMETOOLONG;
    }

    if (lstat(root_path, &info) == 0)
    {
        return -EEXIST;
    }

    if (errno != ENOENT)
    {
        return -errno;
    }

    ret = _upgrade_archive_check(package_path);
    if (ret != 0)
    {
        return ret;
    }

    ret = _upgrade_archive_unpack(package_path, output_dir);
    if (ret != 0)
    {
        return ret;
    }

    if (lstat(root_path, &info) != 0)
    {
        return -errno;
    }

    return S_ISDIR(info.st_mode) ? 0 : -EBADMSG;
}
