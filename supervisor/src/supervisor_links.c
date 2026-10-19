/**
 * @file supervisor_links.c
 * @brief App及Web软链接原子替换和失败协调恢复
 * @author Dawn
 * @version 0.2.0
 * @date 2026-10-19
 */

#include "supervisor_links.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "supervisor_storage.h"
#include "supervisor_version.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    char path[PATH_MAX];     // current路径，仅替换链接，不删除目录
    char previous[PATH_MAX]; // 切换前的原始链接内容，允许相对路径
    char target[PATH_MAX];   // 本次目标Release绝对路径
    bool existed;            // 切换前是否存在链接
} supervisor_link_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 保存旧链接，拒绝覆盖同名普通文件或目录。
 */
static int _supervisor_links_prepare(supervisor_link_t *link, const char *root, unsigned int release)
{
    struct stat information;
    ssize_t     length;
    int         ret;

    memset(link, 0, sizeof(*link));
    ret = snprintf(link->path, sizeof(link->path), "%s/current", root);
    if (ret < 0 || (size_t)ret >= sizeof(link->path))
    {
        return -ENAMETOOLONG;
    }

    ret = snprintf(link->target, sizeof(link->target), "%s/releases/%s", root, supervisor_version_name(release));
    if (ret < 0 || (size_t)ret >= sizeof(link->target))
    {
        return -ENAMETOOLONG;
    }

    if (lstat(link->path, &information) != 0)
    {
        return errno == ENOENT ? 0 : -errno;
    }

    if (!S_ISLNK(information.st_mode))
    {
        return -EEXIST;
    }

    length = readlink(link->path, link->previous, sizeof(link->previous) - 1U);
    if (length < 0 || (size_t)length >= sizeof(link->previous) - 1U)
    {
        return length < 0 ? -errno : -ENAMETOOLONG;
    }

    link->previous[length] = '\0';
    link->existed          = true;
    return 0;
}

/**
 * @brief 在同目录创建临时软链接并rename替换，然后同步目录。
 */
static int _supervisor_links_replace(const char *path, const char *target)
{
    char temporary[PATH_MAX];
    int  descriptor;
    int  ret;

    if (target == NULL)
    {
        return supervisor_storage_remove(path);
    }

    ret = snprintf(temporary, sizeof(temporary), "%s.tmp.XXXXXX", path);
    if (ret < 0 || (size_t)ret >= sizeof(temporary))
    {
        return -ENAMETOOLONG;
    }

    descriptor = mkostemp(temporary, O_CLOEXEC);
    if (descriptor < 0)
    {
        return -errno;
    }

    close(descriptor);
    if (unlink(temporary) != 0)
    {
        return -errno;
    }

    if (symlink(target, temporary) != 0)
    {
        return -errno;
    }

    ret = rename(temporary, path) == 0 ? 0 : -errno;
    if (ret != 0)
    {
        unlink(temporary);
        return ret;
    }

    return supervisor_storage_sync_parent(path);
}

/****************************** 版本切换 ******************************/

/**
 * @brief 协调切换两个current，任何失败均恢复两个旧链接并禁止启动。
 */
int supervisor_links_select(const char *app_root, const char *web_root, unsigned int release)
{
    supervisor_link_t links[2];
    const char       *roots[2] = {app_root, web_root};
    int               index;
    int               restore_ret;
    int               ret;

    for (index = 0; index < 2; index++)
    {
        ret = _supervisor_links_prepare(&links[index], roots[index], release);
        if (ret != 0)
        {
            return ret;
        }
    }

    for (index = 0; index < 2; index++)
    {
        if (links[index].existed && strcmp(links[index].previous, links[index].target) == 0)
        {
            continue;
        }

        ret = _supervisor_links_replace(links[index].path, links[index].target);
        if (ret != 0)
        {
            // rename成功而目录同步失败也可能已改变链接，因此两条路径均恢复。
            for (index = 0; index < 2; index++)
            {
                restore_ret = _supervisor_links_replace(links[index].path, links[index].existed ? links[index].previous : NULL);
                if (restore_ret != 0)
                {
                    fprintf(stderr, "Supervisor links restore unresolved path=%s error=%d\n", links[index].path, restore_ret);
                }
            }

            return ret;
        }
    }

    return 0;
}
