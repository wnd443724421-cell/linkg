/**
 * @file upgrade_install.c
 * @brief LinkG非稳定Release的App及Web完整安装
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "upgrade_install.h"

#include <ftw.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linkg_release_state.h"

/****************************** 模块常量 ******************************/

#define UPGRADE_INSTALL_STATE_PATH      linkg_release_path("LINKG_OTA_TEST_STATE", "/app/version_state.json") // Supervisor版本状态，只读
#define UPGRADE_INSTALL_APP_ROOT        linkg_release_path("LINKG_OTA_TEST_APP_ROOT", "/app")                 // App安装根目录
#define UPGRADE_INSTALL_WEB_ROOT        linkg_release_path("LINKG_OTA_TEST_WEB_ROOT", "/www")                 // Web安装根目录
#define UPGRADE_INSTALL_COPY_PROGRAM    "/bin/cp"                                                             // BusyBox cp可执行路径
#define UPGRADE_INSTALL_STATE_MAX_BYTES 4096U                                                                 // 状态JSON最大读取大小

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取Supervisor稳定状态，禁止在候选试运行期间覆盖其他Release。
 */
static int _upgrade_install_select_target(const char **stable, const char **target)
{
    linkg_release_state_t state;
    int                   ret;

    ret = linkg_release_load(UPGRADE_INSTALL_STATE_PATH, &state);
    if (ret == 0 && state.target != state.stable)
    {
        ret = -EBUSY;
    }
    if (ret == 0)
    {
        *stable = linkg_release_name(state.stable);
        *target = linkg_release_name(1U - state.stable);
    }
    return ret;
}

/**
 * @brief 验证current仍指向稳定Release，不允许清理正在运行的槽位。
 */
static int _upgrade_install_check_current(const char *root, const char *stable)
{
    char    current_path[PATH_MAX];
    char    expected[PATH_MAX];
    char    linked[PATH_MAX];
    ssize_t length;
    int     ret;

    ret = snprintf(current_path, sizeof(current_path), "%s/current", root);
    if (ret < 0 || (size_t)ret >= sizeof(current_path))
    {
        return -ENAMETOOLONG;
    }

    ret = snprintf(expected, sizeof(expected), "%s/releases/%s", root, stable);
    if (ret < 0 || (size_t)ret >= sizeof(expected))
    {
        return -ENAMETOOLONG;
    }

    length = readlink(current_path, linked, sizeof(linked) - 1U);
    if (length < 0)
    {
        return -errno;
    }
    if ((size_t)length >= sizeof(linked) - 1U)
    {
        return -ENAMETOOLONG;
    }

    linked[length] = '\0';
    return strcmp(linked, expected) == 0 ? 0 : -ESTALE;
}

/**
 * @brief 打开或创建一个真实的目标Release目录，禁止跟随符号链接。
 */
static int _upgrade_install_open_target(const char *root, const char *release)
{
    int root_fd;
    int releases_fd;
    int target_fd;
    int ret;

    root_fd = open(root, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0)
    {
        return -errno;
    }

    releases_fd = openat(root_fd, "releases", O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(root_fd);
    if (releases_fd < 0)
    {
        return -errno;
    }

    ret = mkdirat(releases_fd, release, 0755);
    if (ret != 0 && errno != EEXIST)
    {
        ret = -errno;
        close(releases_fd);
        return ret;
    }

    target_fd = openat(releases_fd, release, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    close(releases_fd);
    return target_fd < 0 ? -errno : target_fd;
}

/**
 * @brief 递归清理目标槽位旧文件和软链接，所有目录均原样保留。
 *
 * 使用FTW_PHYS禁止跟随链接，避免rootfs预置目录rmdir失败。
 */
static int _upgrade_install_remove_file(const char *path, const struct stat *information, int type, struct FTW *context)
{
    (void)information;
    (void)context;

    if (type == FTW_D || type == FTW_DP)
    {
        return 0;
    }
    if (type == FTW_F || type == FTW_SL || type == FTW_SLN)
    {
        return unlink(path);
    }

    errno = EPERM;
    return -1;
}

/**
 * @brief 确认旧槽位已经只剩下目录，不允许残留旧文件或软链接。
 */
static int _upgrade_install_check_empty(const char *path, const struct stat *information, int type, struct FTW *context)
{
    (void)path;
    (void)information;
    (void)context;

    if (type == FTW_D || type == FTW_DP)
    {
        return 0;
    }

    errno = EEXIST;
    return -1;
}

/**
 * @brief 调用系统cp -a完整复制目录内容，原样保留新包中的符号链接。
 */
static int _upgrade_install_copy(const char *source_dir, const char *target_dir)
{
    char  source[PATH_MAX];
    pid_t child;
    int   status;
    int   ret;

    ret = snprintf(source, sizeof(source), "%s/.", source_dir);
    if (ret < 0 || (size_t)ret >= sizeof(source))
    {
        return -ENAMETOOLONG;
    }

    child = fork();
    if (child < 0)
    {
        return -errno;
    }
    if (child == 0)
    {
        execl(UPGRADE_INSTALL_COPY_PROGRAM, "cp", "-a", source, target_dir, (char *)NULL);
        _exit(127);
    }

    do
    {
        ret = waitpid(child, &status, 0);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        return -errno;
    }

    return WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -EIO;
}

/**
 * @brief 确认解压源目录是普通目录，不能以软链接冒充安装源。
 */
static int _upgrade_install_check_source(const char *upgrade_dir, const char *name)
{
    char        path[PATH_MAX];
    struct stat information;
    int         ret;

    ret = snprintf(path, sizeof(path), "%s/%s", upgrade_dir, name);
    if (ret < 0 || (size_t)ret >= sizeof(path))
    {
        return -ENAMETOOLONG;
    }

    if (lstat(path, &information) != 0)
    {
        return -errno;
    }

    return S_ISDIR(information.st_mode) ? 0 : -ENOTDIR;
}

/****************************** 安装接口 ******************************/

/**
 * @brief 清理非稳定Release的旧文件，完整复制App及Web并同步到存储介质。
 *
 * 调用前必须已经通过upgrade_manifest_verify()校验。
 * current链接和Supervisor版本状态由Supervisor管理，本函数只读不写。
 */
int upgrade_install_release(const char *upgrade_dir, char *installed_release, size_t release_size)
{
    const char *stable = NULL;
    const char *target = NULL;
    const char *roots[2] = {UPGRADE_INSTALL_APP_ROOT, UPGRADE_INSTALL_WEB_ROOT};
    const char *sections[2] = {"app", "www"};
    char        source[PATH_MAX];
    char        destination[PATH_MAX];
    int         target_fd[2] = {-1, -1};
    int         ret;
    int         index;

    if (upgrade_dir == NULL || upgrade_dir[0] != '/' || installed_release == NULL || release_size < 3U)
    {
        return -EINVAL;
    }

    installed_release[0] = '\0';

    ret = _upgrade_install_check_source(upgrade_dir, "app");
    if (ret == 0)
    {
        ret = _upgrade_install_check_source(upgrade_dir, "www");
    }
    if (ret != 0)
    {
        return ret;
    }

    ret = _upgrade_install_select_target(&stable, &target);
    if (ret != 0)
    {
        return ret;
    }

    // 两个current都必须仍指向稳定槽位，才能进行任何清理。
    for (index = 0; index < 2; index++)
    {
        ret = _upgrade_install_check_current(roots[index], stable);
        if (ret != 0)
        {
            return ret;
        }
    }

    // 先确认两个目标目录均合法，再开始清理旧版本文件。
    for (index = 0; index < 2; index++)
    {
        target_fd[index] = _upgrade_install_open_target(roots[index], target);
        if (target_fd[index] < 0)
        {
            ret = target_fd[index];
            goto finish;
        }
    }

    for (index = 0; index < 2; index++)
    {
        ret = snprintf(destination, sizeof(destination), "%s/releases/%s", roots[index], target);
        if (ret < 0 || (size_t)ret >= sizeof(destination))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        // 不删除旧目录；即使遗留空目录也允许继续安装。
        ret = nftw(destination, _upgrade_install_remove_file, 16, FTW_PHYS | FTW_DEPTH);
        if (ret == 0)
        {
            ret = nftw(destination, _upgrade_install_check_empty, 16, FTW_PHYS | FTW_DEPTH);
        }
        if (ret != 0)
        {
            ret = -errno;
            goto finish;
        }
    }

    for (index = 0; index < 2; index++)
    {
        ret = snprintf(source, sizeof(source), "%s/%s", upgrade_dir, sections[index]);
        if (ret < 0 || (size_t)ret >= sizeof(source))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        ret = snprintf(destination, sizeof(destination), "%s/releases/%s", roots[index], target);
        if (ret < 0 || (size_t)ret >= sizeof(destination))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        ret = _upgrade_install_copy(source, destination);
        if (ret != 0)
        {
            goto finish;
        }
    }

    // syncfs会提交对应文件系统上的文件数据和目录元数据。
    for (index = 0; index < 2; index++)
    {
        if (syncfs(target_fd[index]) != 0)
        {
            ret = -errno;
            goto finish;
        }
    }

    (void)snprintf(installed_release, release_size, "%s", target);
    ret = 0;

finish:
    for (index = 0; index < 2; index++)
    {
        if (target_fd[index] >= 0)
        {
            close(target_fd[index]);
        }
    }

    return ret;
}
