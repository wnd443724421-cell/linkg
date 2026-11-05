/**
 * @file upgrade_install.c
 * @brief LinkG非稳定Release的App及Web选择性安装
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "upgrade_install.h"

#include <dirent.h>
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
#include "linkg_log.h"
#include "upgrade_manifest.h"

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
 * @brief 递归清理一个目录项中的普通文件和软链接，保留目录结构。
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
 * @brief 递归清理目录组，保留根目录，允许保留无法删除的子目录。
 */
static int _upgrade_install_remove_tree(const char *path, const struct stat *information, int type, struct FTW *context)
{
    int saved_errno;

    (void)information;

    if (type == FTW_D)
    {
        return 0;
    }

    if (type == FTW_DP)
    {
        // bin/modules根目录需要保留，后续直接复制新文件。
        if (context->level == 0)
        {
            return 0;
        }

        if (rmdir(path) == 0)
        {
            return 0;
        }

        saved_errno = errno;
        if (saved_errno == EIO)
        {
            LINKG_LOG_WARN("UPGRADE: keep undeletable directory, path=%s errno=%d(%s)",
                           path, saved_errno, strerror(saved_errno));
            return 0;
        }

        errno = saved_errno;
        return -1;
    }

    if (type == FTW_F || type == FTW_SL || type == FTW_SLN)
    {
        return unlink(path);
    }

    errno = EPERM;
    return -1;
}

/**
 * @brief 清理Release根目录下的旧内容，可保留App的bin和modules目录。
 */
static int _upgrade_install_clear_release(const char *release_dir, bool preserve_app_groups)
{
    DIR           *directory;
    struct dirent *entry;
    char            path[PATH_MAX];
    int             length;
    int             ret;

    directory = opendir(release_dir);
    if (directory == NULL)
    {
        return -errno;
    }

    ret = 0;
    while (true)
    {
        errno  = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                ret = -errno;
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }
        if (preserve_app_groups &&
            (strcmp(entry->d_name, "bin") == 0 || strcmp(entry->d_name, "modules") == 0))
        {
            continue;
        }

        length = snprintf(path, sizeof(path), "%s/%s", release_dir, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path))
        {
            ret = -ENAMETOOLONG;
            break;
        }

        errno = 0;
        length = nftw(path, _upgrade_install_remove_file, 16, FTW_PHYS | FTW_DEPTH);
        if (length != 0)
        {
            ret = errno != 0 ? -errno : -EIO;
            break;
        }
    }

    if (closedir(directory) != 0 && ret == 0)
    {
        ret = -errno;
    }

    return ret;
}

/**
 * @brief 删除目标目录组的全部旧内容，允许目标目录原本不存在。
 */
static int _upgrade_install_remove_group(const char *target_dir)
{
    struct stat information;
    int         ret;

    if (lstat(target_dir, &information) != 0)
    {
        return errno == ENOENT ? 0 : -errno;
    }

    if (S_ISDIR(information.st_mode))
    {
        errno = 0;
        ret = nftw(target_dir, _upgrade_install_remove_tree, 16, FTW_PHYS | FTW_DEPTH);
        return ret == 0 ? 0 : (errno != 0 ? -errno : -EIO);
    }

    if (S_ISREG(information.st_mode) || S_ISLNK(information.st_mode))
    {
        return unlink(target_dir) == 0 ? 0 : -errno;
    }

    return -EPERM;
}

/**
 * @brief 先判断目录存在性和权限，再按已验证清单比较文件内容。
 *
 * 升级包未包含某目录组时允许源目录不存在，对应目标目录也必须不存在才能跳过。
 * 目录权限以升级包实际权限为准，不额外规定固定值。
 */
static int _upgrade_install_group_equal(const char *upgrade_dir, const char *manifest_prefix, const char *source_dir, const char *target_dir, bool *same, bool *source_exists)
{
    struct stat source_info;
    struct stat target_info;

    if (same == NULL || source_exists == NULL)
    {
        return -EINVAL;
    }

    *same          = false;
    *source_exists = false;

    if (lstat(source_dir, &source_info) != 0)
    {
        if (errno != ENOENT)
        {
            return -errno;
        }

        // 源目录不存在时，目标也不存在则无需任何处理。
        if (lstat(target_dir, &target_info) != 0)
        {
            if (errno == ENOENT || errno == ENOTDIR)
            {
                *same = true;
                return 0;
            }

            return -errno;
        }

        return 0;
    }

    if (!S_ISDIR(source_info.st_mode))
    {
        return -ENOTDIR;
    }

    *source_exists = true;
    if (lstat(target_dir, &target_info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    }

    if (!S_ISDIR(target_info.st_mode) ||
        (source_info.st_mode & 07777U) != (target_info.st_mode & 07777U))
    {
        return 0;
    }

    return upgrade_manifest_group_equal(upgrade_dir, manifest_prefix, target_dir, same);
}

/**
 * @brief 调用系统cp -a复制单个文件、软链接或目录到目标目录。
 */
static int _upgrade_install_copy_path(const char *source_path, const char *target_dir)
{
    pid_t child;
    int   status;
    int   ret;

    child = fork();
    if (child < 0)
    {
        return -errno;
    }
    if (child == 0)
    {
        execl(UPGRADE_INSTALL_COPY_PROGRAM, "cp", "-a", source_path, target_dir, (char *)NULL);
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
 * @brief 调用系统cp -a完整复制目录内容，原样保留新包中的符号链接。
 */
static int _upgrade_install_copy(const char *source_dir, const char *target_dir)
{
    char source[PATH_MAX];
    int  ret;

    ret = snprintf(source, sizeof(source), "%s/.", source_dir);
    if (ret < 0 || (size_t)ret >= sizeof(source))
    {
        return -ENAMETOOLONG;
    }

    return _upgrade_install_copy_path(source, target_dir);
}

/**
 * @brief 复制App根目录内容，排除由目录组安装流程单独处理的bin和modules。
 */
static int _upgrade_install_copy_app(const char *source_dir, const char *target_dir)
{
    DIR           *directory;
    struct dirent *entry;
    char            source[PATH_MAX];
    int             length;
    int             ret;

    directory = opendir(source_dir);
    if (directory == NULL)
    {
        return -errno;
    }

    ret = 0;
    while (true)
    {
        errno  = 0;
        entry = readdir(directory);
        if (entry == NULL)
        {
            if (errno != 0)
            {
                ret = -errno;
            }
            break;
        }

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            strcmp(entry->d_name, "bin") == 0 || strcmp(entry->d_name, "modules") == 0)
        {
            continue;
        }

        length = snprintf(source, sizeof(source), "%s/%s", source_dir, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(source))
        {
            ret = -ENAMETOOLONG;
            break;
        }

        ret = _upgrade_install_copy_path(source, target_dir);
        if (ret != 0)
        {
            break;
        }
    }

    if (closedir(directory) != 0 && ret == 0)
    {
        ret = -errno;
    }

    return ret;
}

/**
 * @brief 替换不一致的目录组，保留已有目录并复制新内容。
 */
static int _upgrade_install_replace_group(const char *source_dir, const char *target_release_dir, const char *target_dir, bool source_exists)
{
    struct stat information;
    int         ret;

    ret = _upgrade_install_remove_group(target_dir);
    if (ret != 0)
    {
        return ret;
    }

    if (!source_exists)
    {
        return 0;
    }

    // 目标目录仍存在时，直接复制源目录中的内容。
    if (lstat(target_dir, &information) == 0)
    {
        if (!S_ISDIR(information.st_mode))
        {
            return -ENOTDIR;
        }

        return _upgrade_install_copy(source_dir, target_dir);
    }

    if (errno != ENOENT)
    {
        return -errno;
    }

    // 目标目录原本不存在时，复制整个目录组。
    return _upgrade_install_copy_path(source_dir, target_release_dir);
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
 * @brief 安装非稳定Release的App及Web，仅跳过内容和权限完全一致的bin与modules目录组。
 *
 * 调用前必须已经通过upgrade_manifest_verify()校验。
 * current链接和Supervisor版本状态由Supervisor管理，本函数只读不写。
 */
int upgrade_install_release(const char *upgrade_dir, char *installed_release, size_t release_size)
{
    const char *stable = NULL;
    const char *target = NULL;
    const char *roots[2] = {UPGRADE_INSTALL_APP_ROOT, UPGRADE_INSTALL_WEB_ROOT};
    char        app_source[PATH_MAX];
    char        app_destination[PATH_MAX];
    char        web_source[PATH_MAX];
    char        web_destination[PATH_MAX];
    char        group_source[PATH_MAX];
    char        group_destination[PATH_MAX];
    int         target_fd[2] = {-1, -1};
    bool        bin_same              = false;
    bool        modules_same          = false;
    bool        bin_source_exists     = false;
    bool        modules_source_exists = false;
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

    // 先确认两个目标Release目录均合法，再开始目录比较和清理。
    for (index = 0; index < 2; index++)
    {
        target_fd[index] = _upgrade_install_open_target(roots[index], target);
        if (target_fd[index] < 0)
        {
            ret = target_fd[index];
            goto finish;
        }
    }

    ret = snprintf(app_source, sizeof(app_source), "%s/app", upgrade_dir);
    if (ret < 0 || (size_t)ret >= sizeof(app_source))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = snprintf(app_destination, sizeof(app_destination), "%s/releases/%s", UPGRADE_INSTALL_APP_ROOT, target);
    if (ret < 0 || (size_t)ret >= sizeof(app_destination))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = snprintf(web_source, sizeof(web_source), "%s/www", upgrade_dir);
    if (ret < 0 || (size_t)ret >= sizeof(web_source))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = snprintf(web_destination, sizeof(web_destination), "%s/releases/%s", UPGRADE_INSTALL_WEB_ROOT, target);
    if (ret < 0 || (size_t)ret >= sizeof(web_destination))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = snprintf(group_source, sizeof(group_source), "%s/bin", app_source);
    if (ret < 0 || (size_t)ret >= sizeof(group_source))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = snprintf(group_destination, sizeof(group_destination), "%s/bin", app_destination);
    if (ret < 0 || (size_t)ret >= sizeof(group_destination))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    // 两个目录组必须在任何清理操作前完成比较。
    ret = _upgrade_install_group_equal(upgrade_dir, "app/bin/", group_source, group_destination, &bin_same, &bin_source_exists);
    if (ret != 0)
    {
        goto finish;
    }

    ret = snprintf(group_source, sizeof(group_source), "%s/modules", app_source);
    if (ret < 0 || (size_t)ret >= sizeof(group_source))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = snprintf(group_destination, sizeof(group_destination), "%s/modules", app_destination);
    if (ret < 0 || (size_t)ret >= sizeof(group_destination))
    {
        ret = -ENAMETOOLONG;
        goto finish;
    }

    ret = _upgrade_install_group_equal(upgrade_dir, "app/modules/", group_source, group_destination, &modules_same, &modules_source_exists);
    if (ret != 0)
    {
        goto finish;
    }

    // App清理时保留bin和modules，由后续目录组逻辑分别决定是否替换。
    ret = _upgrade_install_clear_release(app_destination, true);
    if (ret != 0)
    {
        goto finish;
    }

    // Web保持原有完整清理行为。
    ret = _upgrade_install_clear_release(web_destination, false);
    if (ret != 0)
    {
        goto finish;
    }

    // 复制App中除bin和modules以外的全部顶层内容。
    ret = _upgrade_install_copy_app(app_source, app_destination);
    if (ret != 0)
    {
        goto finish;
    }

    // 仅替换不一致的目录组，完全一致时不删除也不复制。
    if (!bin_same)
    {
        ret = snprintf(group_source, sizeof(group_source), "%s/bin", app_source);
        if (ret < 0 || (size_t)ret >= sizeof(group_source))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        ret = snprintf(group_destination, sizeof(group_destination), "%s/bin", app_destination);
        if (ret < 0 || (size_t)ret >= sizeof(group_destination))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        ret = _upgrade_install_replace_group(group_source, app_destination, group_destination, bin_source_exists);
        if (ret != 0)
        {
            goto finish;
        }
    }

    if (!modules_same)
    {
        ret = snprintf(group_source, sizeof(group_source), "%s/modules", app_source);
        if (ret < 0 || (size_t)ret >= sizeof(group_source))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        ret = snprintf(group_destination, sizeof(group_destination), "%s/modules", app_destination);
        if (ret < 0 || (size_t)ret >= sizeof(group_destination))
        {
            ret = -ENAMETOOLONG;
            goto finish;
        }

        ret = _upgrade_install_replace_group(group_source, app_destination, group_destination, modules_source_exists);
        if (ret != 0)
        {
            goto finish;
        }
    }

    // Web继续完整复制，不参与目录组跳过优化。
    ret = _upgrade_install_copy(web_source, web_destination);
    if (ret != 0)
    {
        goto finish;
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
