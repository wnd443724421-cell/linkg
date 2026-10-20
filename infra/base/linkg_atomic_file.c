/**
 * @file linkg_atomic_file.c
 * @brief 共享状态文件限长读取与原子持久化
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-20
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "linkg_atomic_file.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************** 文件操作 ******************************/

/**
 * @brief 读取完整的有界文本，拒绝软链接、非普通文件及内嵌零字节。
 */
int linkg_atomic_read(const char *path, char *buffer, size_t capacity)
{
    struct stat information;
    size_t      length;
    ssize_t     count;
    int         descriptor;
    int         ret;

    if (path == NULL || buffer == NULL || capacity < 2U)
    {
        return -EINVAL;
    }

    descriptor = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (descriptor < 0)
    {
        return -errno;
    }

    ret = fstat(descriptor, &information) == 0 ? 0 : -errno;
    if (ret == 0 && !S_ISREG(information.st_mode))
    {
        ret = -EINVAL;
    }

    length = 0U;
    while (ret == 0 && length < capacity - 1U)
    {
        count = read(descriptor, buffer + length, capacity - 1U - length);
        if (count < 0)
        {
            if (errno != EINTR)
            {
                ret = -errno;
            }

            continue;
        }

        if (count == 0)
        {
            break;
        }

        length += (size_t)count;
    }

    if (ret == 0 && length == capacity - 1U)
    {
        ret = -EFBIG;
    }

    if (ret == 0 && memchr(buffer, '\0', length) != NULL)
    {
        ret = -EINVAL;
    }

    close(descriptor);
    buffer[length] = '\0';
    return ret;
}

/**
 * @brief 同步文件所在目录，使rename或unlink跨断电持久化。
 */
int linkg_atomic_sync_parent(const char *path)
{
    char  parent[PATH_MAX];
    char *separator;
    int   descriptor;
    int   ret;

    if (path == NULL || path[0] != '/' || strlen(path) >= sizeof(parent))
    {
        return -EINVAL;
    }

    strcpy(parent, path);
    separator = strrchr(parent, '/');
    if (separator == parent)
    {
        separator[1] = '\0';
    }
    else
    {
        *separator = '\0';
    }
    descriptor = open(parent, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (descriptor < 0)
    {
        return -errno;
    }

    ret = fsync(descriptor) == 0 ? 0 : -errno;
    close(descriptor);
    return ret;
}

/**
 * @brief 在同一目录写临时文件、同步并原子替换，最后同步目录。
 */
int linkg_atomic_write(const char *path, const char *buffer, size_t length)
{
    char    temporary[PATH_MAX];
    size_t  offset;
    ssize_t count;
    int     descriptor;
    int     ret;

    ret = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (ret < 0 || (size_t)ret >= sizeof(temporary) || path[0] != '/')
    {
        return -ENAMETOOLONG;
    }

    descriptor = mkostemp(temporary, O_CLOEXEC);
    if (descriptor < 0)
    {
        return -errno;
    }

    ret    = 0;
    offset = 0U;
    while (offset < length)
    {
        count = write(descriptor, buffer + offset, length - offset);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }

        if (count <= 0)
        {
            ret = count == 0 ? -EIO : -errno;
            break;
        }

        offset += (size_t)count;
    }

    if (ret == 0 && fsync(descriptor) != 0)
    {
        ret = -errno;
    }

    if (close(descriptor) != 0 && ret == 0)
    {
        ret = -errno;
    }

    if (ret == 0 && rename(temporary, path) != 0)
    {
        ret = -errno;
    }

    if (ret != 0)
    {
        unlink(temporary);
        return ret;
    }

    // 目录同步失败时rename可能已经生效，调用者必须重新读取并恢复事务。
    return linkg_atomic_sync_parent(path);
}

/**
 * @brief 删除事务文件并同步目录，文件已经不存在时仍执行目录同步。
 */
int linkg_atomic_remove(const char *path)
{
    if (unlink(path) != 0 && errno != ENOENT)
    {
        return -errno;
    }

    return linkg_atomic_sync_parent(path);
}
