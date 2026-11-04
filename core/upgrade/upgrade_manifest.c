/**
 * @file upgrade_manifest.c
 * @brief LinkG升级包FILES.txt文件完整性校验
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#define _GNU_SOURCE

#include "upgrade_manifest.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

/****************************** 模块常量 ******************************/

#define UPGRADE_MANIFEST_BUSYBOX_PATH "/bin/busybox"         // 使用系统BusyBox的sha256sum
#define UPGRADE_MANIFEST_FILENAME     "FILES.txt"            // 清单文件名称
#define UPGRADE_MANIFEST_MAX_ENTRIES  2048U                  // 最大文件记录数量
#define UPGRADE_MANIFEST_MAX_SIZE     (512U * 1024U)         // 清单最大字节数
#define UPGRADE_MANIFEST_MAX_DATA     (128U * 1024U * 1024U) // 普通文件累计大小
#define UPGRADE_MANIFEST_MAX_DEPTH    32U                    // 目录遍历深度上限

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验归档内的普通相对路径，且仅允许预期安装区域。
 */
static bool _upgrade_manifest_valid_path(const char *path)
{
    const char *part;
    const char *p;
    size_t      length;

    if (path == NULL || path[0] == '/' || path[0] == '\0' || strlen(path) >= PATH_MAX)
    {
        return false;
    }

    part = path;
    for (p = path; ; ++p)
    {
        if ((*p != '\0' && ((unsigned char)*p < 32U || (unsigned char)*p == 127U)) || *p == '\\')
        {
            return false;
        }

        if (*p != '/' && *p != '\0')
        {
            continue;
        }

        length = (size_t)(p - part);
        if (length == 0U || (length == 1U && part[0] == '.') ||
            (length == 2U && part[0] == '.' && part[1] == '.'))
        {
            return false;
        }

        if (*p == '\0')
        {
            break;
        }

        part = p + 1;
    }

    return strncmp(path, "app/", 4U) == 0 || strncmp(path, "www/", 4U) == 0 ||
           strcmp(path, "kernel/boot.img") == 0 || strcmp(path, "system_install.json") == 0;
}

/**
 * @brief 解析文件大小及64位小写SHA-256字段。
 */
static int _upgrade_manifest_parse_fields(const char *size_text, const char *sha, uint64_t *size)
{
    char *end;
    unsigned int i;

    if (size_text[0] < '0' || size_text[0] > '9' || strlen(sha) != 64U)
    {
        return -EBADMSG;
    }

    for (i = 0U; i < 64U; ++i)
    {
        if (!((sha[i] >= '0' && sha[i] <= '9') || (sha[i] >= 'a' && sha[i] <= 'f')))
        {
            return -EBADMSG;
        }
    }

    errno = 0;
    *size = strtoull(size_text, &end, 10);
    return errno == 0 && *end == '\0' ? 0 : -EBADMSG;
}

/**
 * @brief 解析清单普通文件的四位八进制权限字段。
 */
static int _upgrade_manifest_parse_mode(const char *mode_text, mode_t *mode)
{
    mode_t       parsed;
    unsigned int index;

    if (mode_text == NULL || mode == NULL || strlen(mode_text) != 4U)
    {
        return -EBADMSG;
    }

    parsed = 0;
    for (index = 0U; index < 4U; ++index)
    {
        if (mode_text[index] < '0' || mode_text[index] > '7')
        {
            return -EBADMSG;
        }

        parsed = (mode_t)((parsed << 3) | (mode_t)(mode_text[index] - '0'));
    }

    *mode = parsed;
    return 0;
}

/**
 * @brief 调用BusyBox sha256sum，对文件或内存字节流计算摘要。
 */
static int _upgrade_manifest_sha256(const char *file, const char *bytes, size_t size, const char *expected)
{
    char    output[65];
    char    buffer[512];
    int     result_pipe[2];
    int     input_pipe[2];
    pid_t   pid;
    size_t  sent;
    size_t  received;
    ssize_t length;
    int     status;
    int     wait_ret;
    int     ret;

    input_pipe[0] = -1;
    input_pipe[1] = -1;
    if (pipe(result_pipe) != 0)
    {
        return -errno;
    }

    if (file == NULL && socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, input_pipe) != 0)
    {
        ret = -errno;
        close(result_pipe[0]);
        close(result_pipe[1]);
        return ret;
    }

    pid = fork();
    if (pid < 0)
    {
        ret = -errno;
        goto close_pipes;
    }

    if (pid == 0)
    {
        close(result_pipe[0]);
        if (dup2(result_pipe[1], STDOUT_FILENO) < 0)
        {
            _exit(127);
        }

        close(result_pipe[1]);
        if (file == NULL)
        {
            close(input_pipe[1]);
            if (dup2(input_pipe[0], STDIN_FILENO) < 0)
            {
                _exit(127);
            }

            close(input_pipe[0]);
            execl(UPGRADE_MANIFEST_BUSYBOX_PATH, "busybox", "sha256sum", (char *)NULL);
        }
        else
        {
            execl(UPGRADE_MANIFEST_BUSYBOX_PATH, "busybox", "sha256sum", file, (char *)NULL);
        }

        _exit(127);
    }

    close(result_pipe[1]);
    result_pipe[1] = -1;
    ret            = 0;
    if (file == NULL)
    {
        close(input_pipe[0]);
        input_pipe[0] = -1;
        sent = 0U;
        while (sent < size)
        {
            length = send(input_pipe[1], bytes + sent, size - sent, MSG_NOSIGNAL);
            if (length < 0 && errno == EINTR)
            {
                continue;
            }

            if (length <= 0)
            {
                ret = length < 0 ? -errno : -EIO;
                break;
            }

            sent += (size_t)length;
        }

        close(input_pipe[1]);
        input_pipe[1] = -1;
    }

    received = 0U;
    while (true)
    {
        length = read(result_pipe[0], buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR)
        {
            continue;
        }

        if (length <= 0)
        {
            if (length < 0 && ret == 0)
            {
                ret = -errno;
            }
            break;
        }

        if (received < sizeof(output))
        {
            size_t copy = sizeof(output) - received;
            if (copy > (size_t)length)
            {
                copy = (size_t)length;
            }

            memcpy(output + received, buffer, copy);
        }

        received += (size_t)length;
        if (received > PATH_MAX + 80U)
        {
            ret = -EBADMSG;
        }
    }

    close(result_pipe[0]);
    result_pipe[0] = -1;
    do
    {
        wait_ret = waitpid(pid, &status, 0);
    }
    while (wait_ret < 0 && errno == EINTR);

    if (wait_ret < 0)
    {
        ret = -errno;
    }
    else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        ret = WIFEXITED(status) && WEXITSTATUS(status) == 127 ? -ENOSYS : -EIO;
    }
    else if (ret == 0)
    {
        ret = received >= 66U && output[64] == ' ' && memcmp(output, expected, 64U) == 0 ? 0 : -EBADMSG;
    }

close_pipes:
    if (result_pipe[0] >= 0) close(result_pipe[0]);
    if (result_pipe[1] >= 0) close(result_pipe[1]);
    if (input_pipe[0] >= 0) close(input_pipe[0]);
    if (input_pipe[1] >= 0) close(input_pipe[1]);
    return ret;
}

/**
 * @brief 检查清单记录对应的文件类型、大小、目标及SHA-256。
 */
static int _upgrade_manifest_check_entry(const char *root, char *line, char **name, uint64_t *total_size)
{
    char       *fields[6];
    char       *cursor;
    const char *entry_path;
    char        full_path[PATH_MAX];
    char        resolved[PATH_MAX];
    char        area[PATH_MAX];
    char        target[PATH_MAX];
    struct stat info;
    uint64_t    size;
    mode_t      expected_mode;
    bool        regular_file;
    unsigned int count;
    ssize_t     target_size;
    int         length;
    int         ret;

    count  = 0U;
    cursor = line;
    while (cursor != NULL && count < 6U)
    {
        fields[count++] = strsep(&cursor, "\t");
    }

    if (cursor != NULL || count != 5U ||
        !(strcmp(fields[0], "F") == 0 || strcmp(fields[0], "L") == 0) ||
        _upgrade_manifest_parse_fields(fields[1], fields[2], &size) != 0)
    {
        return -EBADMSG;
    }

    regular_file = strcmp(fields[0], "F") == 0;
    entry_path   = fields[regular_file ? 4U : 3U];
    if (!_upgrade_manifest_valid_path(entry_path))
    {
        return -EBADMSG;
    }

    length = snprintf(full_path, sizeof(full_path), "%s/%s", root, entry_path);
    if (length < 0 || (size_t)length >= sizeof(full_path))
    {
        return -ENAMETOOLONG;
    }

    if (lstat(full_path, &info) != 0)
    {
        return -errno;
    }

    if (!regular_file && (strcmp(entry_path, "app/bin/linkg") == 0 ||
                          strcmp(entry_path, "www/index.html") == 0 ||
                          strcmp(entry_path, "www/cgi-bin/cmain") == 0 ||
                          strcmp(entry_path, "www/cgi-bin/upgrade") == 0 ||
                          strcmp(entry_path, "www/version.json") == 0))
    {
        return -EBADMSG;
    }

    if (regular_file)
    {
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
            (uint64_t)info.st_size != size || size > UPGRADE_MANIFEST_MAX_DATA - *total_size ||
            _upgrade_manifest_parse_mode(fields[3], &expected_mode) != 0 ||
            (info.st_mode & 07777U) != expected_mode)
        {
            return -EBADMSG;
        }

        if (realpath(full_path, resolved) == NULL || strcmp(resolved, full_path) != 0)
        {
            return -EBADMSG;
        }

        ret = _upgrade_manifest_sha256(full_path, NULL, 0U, fields[2]);
        if (ret == 0)
        {
            *total_size += size;
        }
    }
    else
    {
        if (!S_ISLNK(info.st_mode) || size >= sizeof(target) || fields[4][0] == '/' ||
            strpbrk(fields[4], "\r\n\t") != NULL || strlen(fields[4]) != size)
        {
            return -EBADMSG;
        }

        target_size = readlink(full_path, target, sizeof(target));
        if (target_size < 0)
        {
            return -errno;
        }

        if ((uint64_t)target_size != size || memcmp(target, fields[4], (size_t)target_size) != 0)
        {
            return -EBADMSG;
        }

        if (!(strncmp(fields[3], "app/", 4U) == 0 || strncmp(fields[3], "www/", 4U) == 0))
        {
            return -EBADMSG;
        }

        length = snprintf(area, sizeof(area), "%s/%s", root,
                          strncmp(fields[3], "app/", 4U) == 0 ? "app/" : "www/");
        if (length < 0 || (size_t)length >= sizeof(area) ||
            realpath(full_path, resolved) == NULL || strncmp(resolved, area, (size_t)length) != 0 ||
            stat(resolved, &info) != 0 || !S_ISREG(info.st_mode))
        {
            return -EBADMSG;
        }

        ret = _upgrade_manifest_sha256(NULL, fields[4], (size_t)size, fields[2]);
    }

    if (ret != 0)
    {
        return ret;
    }

    *name = strdup(entry_path);
    return *name == NULL ? -ENOMEM : 0;
}

/**
 * @brief 排序和检索清单中的相对文件名。
 */
static int _upgrade_manifest_compare(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

/**
 * @brief 递归检查解压目录不存在任何清单之外的文件或特殊类型。
 */
static int _upgrade_manifest_scan(const char *root, const char *relative, char **names, size_t count, size_t *seen, unsigned int depth)
{
    DIR           *directory;
    struct dirent *entry;
    struct stat    info;
    char           folder[PATH_MAX];
    char           name[PATH_MAX];
    char           absolute[PATH_MAX];
    const char    *lookup;
    int            ret;
    int            length;

    if (depth > UPGRADE_MANIFEST_MAX_DEPTH)
    {
        return -ELOOP;
    }

    length = snprintf(folder, sizeof(folder), "%s%s%s", root, relative[0] != '\0' ? "/" : "", relative);
    if (length < 0 || (size_t)length >= sizeof(folder))
    {
        return -ENAMETOOLONG;
    }

    directory = opendir(folder);
    if (directory == NULL)
    {
        return -errno;
    }

    ret = 0;
    errno = 0;
    while ((entry = readdir(directory)) != NULL)
    {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
        {
            continue;
        }

        length = snprintf(name, sizeof(name), "%s%s%s", relative,
                          relative[0] != '\0' ? "/" : "", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(name))
        {
            ret = -ENAMETOOLONG;
            break;
        }

        length = snprintf(absolute, sizeof(absolute), "%s/%s", root, name);
        if (length < 0 || (size_t)length >= sizeof(absolute) || lstat(absolute, &info) != 0)
        {
            ret = -EBADMSG;
            break;
        }

        if (S_ISDIR(info.st_mode))
        {
            if (!(strcmp(name, "app") == 0 || strcmp(name, "www") == 0 ||
                  strcmp(name, "kernel") == 0 || _upgrade_manifest_valid_path(name)))
            {
                ret = -EBADMSG;
                break;
            }

            ret = _upgrade_manifest_scan(root, name, names, count, seen, depth + 1U);
            if (ret != 0)
            {
                break;
            }
        }
        else if (strcmp(name, UPGRADE_MANIFEST_FILENAME) == 0 && S_ISREG(info.st_mode))
        {
            continue;
        }
        else if ((S_ISREG(info.st_mode) || S_ISLNK(info.st_mode)) && _upgrade_manifest_valid_path(name))
        {
            lookup = name;
            if (bsearch(&lookup, names, count, sizeof(*names), _upgrade_manifest_compare) == NULL)
            {
                ret = -EBADMSG;
                break;
            }

            ++*seen;
        }
        else
        {
            ret = -EBADMSG;
            break;
        }

        errno = 0;
    }

    if (ret == 0 && errno != 0)
    {
        ret = -errno;
    }

    closedir(directory);
    return ret;
}

/****************************** 清单校验 ******************************/

/**
 * @brief 校验FILES.txt及解压文件全集，不执行实际安装。
 */
int upgrade_manifest_verify(const char *upgrade_dir)
{
    struct stat info;
    char        manifest_path[PATH_MAX];
    char       *root;
    char       *line;
    char      **names;
    FILE       *stream;
    uint64_t    total_size;
    size_t      capacity;
    size_t      count;
    size_t      seen;
    size_t      i;
    ssize_t     length;
    int         ret;

    if (upgrade_dir == NULL || lstat(upgrade_dir, &info) != 0 || !S_ISDIR(info.st_mode))
    {
        return -EINVAL;
    }

    root = realpath(upgrade_dir, NULL);
    if (root == NULL)
    {
        return -errno;
    }

    if (snprintf(manifest_path, sizeof(manifest_path), "%s/%s", root, UPGRADE_MANIFEST_FILENAME) >= (int)sizeof(manifest_path) ||
        lstat(manifest_path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        info.st_size <= 0 || info.st_size > (off_t)UPGRADE_MANIFEST_MAX_SIZE)
    {
        free(root);
        return -EBADMSG;
    }

    stream = fopen(manifest_path, "r");
    if (stream == NULL)
    {
        ret = -errno;
        free(root);
        return ret;
    }

    names = calloc(UPGRADE_MANIFEST_MAX_ENTRIES, sizeof(*names));
    if (names == NULL)
    {
        fclose(stream);
        free(root);
        return -ENOMEM;
    }

    line       = NULL;
    capacity   = 0U;
    count      = 0U;
    total_size = 0U;
    ret        = 0;
    while ((length = getline(&line, &capacity, stream)) >= 0)
    {
        if (length < 2 || length > (ssize_t)(PATH_MAX * 2U + 160U) ||
            line[length - 1] != '\n' || count >= UPGRADE_MANIFEST_MAX_ENTRIES ||
            memchr(line, '\0', (size_t)length) != NULL || memchr(line, '\r', (size_t)length) != NULL)
        {
            ret = -EBADMSG;
            break;
        }

        line[length - 1] = '\0';
        ret = _upgrade_manifest_check_entry(root, line, &names[count], &total_size);
        if (ret != 0)
        {
            break;
        }

        ++count;
    }

    if (ret == 0 && ferror(stream))
    {
        ret = -EIO;
    }

    free(line);
    fclose(stream);

    if (ret == 0 && count == 0U)
    {
        ret = -EBADMSG;
    }

    if (ret == 0)
    {
        qsort(names, count, sizeof(*names), _upgrade_manifest_compare);
        for (i = 1U; i < count; ++i)
        {
            if (strcmp(names[i - 1U], names[i]) == 0)
            {
                ret = -EBADMSG;
                break;
            }
        }
    }

    if (ret == 0)
    {
        seen = 0U;
        ret  = _upgrade_manifest_scan(root, "", names, count, &seen, 0U);
        if (ret == 0 && seen != count)
        {
            ret = -EBADMSG;
        }
    }

    for (i = 0U; i < count; ++i)
    {
        free(names[i]);
    }

    free(names);
    free(root);
    return ret;
}
/****************************** 目录比较 ******************************/

typedef struct
{
    char     *path;       // 相对清单目录前缀的文件路径
    char      type;       // F普通文件或L软链接
    uint64_t  size;       // 文件大小或软链接目标长度
    mode_t    mode;       // 普通文件权限
    char      sha256[65]; // 文件或软链接目标SHA-256
    char     *target;     // 软链接目标内容
} upgrade_manifest_group_entry_t;

/**
 * @brief 按相对路径排序目录清单记录。
 */
static int _upgrade_manifest_group_compare(const void *left, const void *right)
{
    const upgrade_manifest_group_entry_t *left_entry  = left;
    const upgrade_manifest_group_entry_t *right_entry = right;

    return strcmp(left_entry->path, right_entry->path);
}

/**
 * @brief 根据相对路径查找目录清单记录。
 */
static int _upgrade_manifest_group_lookup_compare(const void *key, const void *element)
{
    const char                            *path  = *(const char *const *)key;
    const upgrade_manifest_group_entry_t *entry = element;

    return strcmp(path, entry->path);
}

/**
 * @brief 判断目录是否为清单中某个文件的父目录。
 */
static bool _upgrade_manifest_group_has_child(const upgrade_manifest_group_entry_t *entries, size_t count, const char *directory)
{
    size_t index;
    size_t length;

    length = strlen(directory);
    for (index = 0U; index < count; ++index)
    {
        if (strncmp(entries[index].path, directory, length) == 0 && entries[index].path[length] == '/')
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 检查目标文件或软链接的元数据，需要时再校验SHA-256。
 */
static int _upgrade_manifest_group_check_entry(const char *target_dir, const upgrade_manifest_group_entry_t *entry, bool verify_sha256, bool *matches)
{
    char        relative[PATH_MAX];
    char        full_path[PATH_MAX];
    char        link_target[PATH_MAX];
    const char *separator;
    struct stat info;
    ssize_t     target_size;
    size_t      parent_length;
    int         length;
    int         ret;

    *matches = false;
    separator = strchr(entry->path, '/');
    while (separator != NULL)
    {
        parent_length = (size_t)(separator - entry->path);
        if (parent_length == 0U || parent_length >= sizeof(relative))
        {
            return -EBADMSG;
        }

        memcpy(relative, entry->path, parent_length);
        relative[parent_length] = '\0';
        length = snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, relative);
        if (length < 0 || (size_t)length >= sizeof(full_path))
        {
            return -ENAMETOOLONG;
        }

        if (lstat(full_path, &info) != 0)
        {
            if (errno == ENOENT || errno == ENOTDIR)
            {
                return 0;
            }

            return -errno;
        }

        if (!S_ISDIR(info.st_mode))
        {
            return 0;
        }

        separator = strchr(separator + 1, '/');
    }

    length = snprintf(full_path, sizeof(full_path), "%s/%s", target_dir, entry->path);
    if (length < 0 || (size_t)length >= sizeof(full_path))
    {
        return -ENAMETOOLONG;
    }

    if (lstat(full_path, &info) != 0)
    {
        if (errno == ENOENT || errno == ENOTDIR)
        {
            return 0;
        }

        return -errno;
    }

    if (entry->type == 'F')
    {
        if (!S_ISREG(info.st_mode) || info.st_nlink != 1 || info.st_size < 0 ||
            (uint64_t)info.st_size != entry->size || (info.st_mode & 07777U) != entry->mode)
        {
            return 0;
        }

        if (verify_sha256)
        {
            ret = _upgrade_manifest_sha256(full_path, NULL, 0U, entry->sha256);
            if (ret == -EBADMSG)
            {
                return 0;
            }
            if (ret != 0)
            {
                return ret;
            }
        }
    }
    else if (entry->type == 'L')
    {
        if (!S_ISLNK(info.st_mode) || entry->size >= sizeof(link_target))
        {
            return 0;
        }

        target_size = readlink(full_path, link_target, sizeof(link_target));
        if (target_size < 0)
        {
            if (errno == ENOENT || errno == ENOTDIR)
            {
                return 0;
            }

            return -errno;
        }

        if ((uint64_t)target_size != entry->size ||
            memcmp(link_target, entry->target, (size_t)target_size) != 0)
        {
            return 0;
        }

        if (verify_sha256)
        {
            ret = _upgrade_manifest_sha256(NULL, entry->target, (size_t)entry->size, entry->sha256);
            if (ret == -EBADMSG)
            {
                return 0;
            }
            if (ret != 0)
            {
                return ret;
            }
        }
    }
    else
    {
        return -EBADMSG;
    }

    *matches = true;
    return 0;
}

/**
 * @brief 递归统计目标文件数量，或检查文件名称与目录结构。
 */
static int _upgrade_manifest_group_scan_target(const char *target_dir, const char *relative,
                                               const upgrade_manifest_group_entry_t *entries, size_t count,
                                               size_t *seen, bool *matches, bool check_names, unsigned int depth)
{
    DIR           *directory;
    struct dirent *entry;
    struct stat    info;
    char           folder[PATH_MAX];
    char           name[PATH_MAX];
    char           absolute[PATH_MAX];
    const char    *lookup;
    int            length;
    int            ret;

    if (depth > UPGRADE_MANIFEST_MAX_DEPTH)
    {
        *matches = false;
        return 0;
    }

    length = snprintf(folder, sizeof(folder), "%s%s%s", target_dir, relative[0] != '\0' ? "/" : "", relative);
    if (length < 0 || (size_t)length >= sizeof(folder))
    {
        return -ENAMETOOLONG;
    }

    directory = opendir(folder);
    if (directory == NULL)
    {
        if (errno == ENOENT || errno == ENOTDIR)
        {
            *matches = false;
            return 0;
        }

        return -errno;
    }

    ret = 0;
    while (true)
    {
        errno = 0;
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

        length = snprintf(name, sizeof(name), "%s%s%s", relative,
                          relative[0] != '\0' ? "/" : "", entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(name))
        {
            ret = -ENAMETOOLONG;
            break;
        }

        length = snprintf(absolute, sizeof(absolute), "%s/%s", target_dir, name);
        if (length < 0 || (size_t)length >= sizeof(absolute))
        {
            ret = -ENAMETOOLONG;
            break;
        }

        if (lstat(absolute, &info) != 0)
        {
            if (errno == ENOENT || errno == ENOTDIR)
            {
                *matches = false;
                break;
            }

            ret = -errno;
            break;
        }

        if (S_ISDIR(info.st_mode))
        {
            if (check_names && !_upgrade_manifest_group_has_child(entries, count, name))
            {
                *matches = false;
                break;
            }

            ret = _upgrade_manifest_group_scan_target(target_dir, name, entries, count, seen, matches, check_names, depth + 1U);
            if (ret != 0 || !*matches)
            {
                break;
            }
        }
        else if (S_ISREG(info.st_mode) || S_ISLNK(info.st_mode))
        {
            if (check_names)
            {
                lookup = name;
                if (bsearch(&lookup, entries, count, sizeof(*entries), _upgrade_manifest_group_lookup_compare) == NULL)
                {
                    *matches = false;
                    break;
                }
            }

            ++*seen;
        }
        else
        {
            *matches = false;
            break;
        }
    }

    closedir(directory);
    return ret;
}

/**
 * @brief 比较升级清单指定目录组与目标Release目录是否完全一致。
 *
 * 仅支持app/bin/和app/modules/。调用前必须已经通过upgrade_manifest_verify()校验。
 * 目标目录不存在、文件不一致或存在额外内容时返回成功且same为false；真实I/O错误返回负错误码。
 */
int upgrade_manifest_group_equal(const char *upgrade_dir, const char *manifest_prefix, const char *target_dir, bool *same)
{
    struct stat                     info;
    upgrade_manifest_group_entry_t *entries;
    char                            manifest_path[PATH_MAX];
    char                           *root;
    char                           *line;
    char                           *cursor;
    char                           *fields[6];
    const char                     *entry_path;
    const char                     *relative_path;
    FILE                           *stream;
    uint64_t                        size;
    size_t                          capacity;
    size_t                          count;
    size_t                          index;
    size_t                          seen;
    ssize_t                         line_length;
    unsigned int                    field_count;
    mode_t                          mode;
    bool                            matches;
    int                             length;
    int                             ret;

    if (same == NULL)
    {
        return -EINVAL;
    }

    *same = false;
    if (upgrade_dir == NULL || upgrade_dir[0] == '\0' || manifest_prefix == NULL || target_dir == NULL || target_dir[0] == '\0' ||
        (strcmp(manifest_prefix, "app/bin/") != 0 && strcmp(manifest_prefix, "app/modules/") != 0))
    {
        return -EINVAL;
    }

    if (lstat(target_dir, &info) != 0)
    {
        return errno == ENOENT || errno == ENOTDIR ? 0 : -errno;
    }

    if (!S_ISDIR(info.st_mode))
    {
        return 0;
    }

    root = realpath(upgrade_dir, NULL);
    if (root == NULL)
    {
        return -errno;
    }

    length = snprintf(manifest_path, sizeof(manifest_path), "%s/%s", root, UPGRADE_MANIFEST_FILENAME);
    if (length < 0 || (size_t)length >= sizeof(manifest_path))
    {
        free(root);
        return -ENAMETOOLONG;
    }

    if (lstat(manifest_path, &info) != 0)
    {
        ret = -errno;
        free(root);
        return ret;
    }

    if (!S_ISREG(info.st_mode) || info.st_nlink != 1 ||
        info.st_size <= 0 || info.st_size > (off_t)UPGRADE_MANIFEST_MAX_SIZE)
    {
        free(root);
        return -EBADMSG;
    }

    stream = fopen(manifest_path, "r");
    if (stream == NULL)
    {
        ret = -errno;
        free(root);
        return ret;
    }

    entries = calloc(UPGRADE_MANIFEST_MAX_ENTRIES, sizeof(*entries));
    if (entries == NULL)
    {
        fclose(stream);
        free(root);
        return -ENOMEM;
    }

    line     = NULL;
    capacity = 0U;
    count    = 0U;
    ret      = 0;
    while ((line_length = getline(&line, &capacity, stream)) >= 0)
    {
        if (line_length < 2 || line_length > (ssize_t)(PATH_MAX * 2U + 160U) ||
            line[line_length - 1] != '\n' || memchr(line, '\0', (size_t)line_length) != NULL ||
            memchr(line, '\r', (size_t)line_length) != NULL)
        {
            ret = -EBADMSG;
            break;
        }

        line[line_length - 1] = '\0';
        field_count = 0U;
        cursor      = line;
        while (cursor != NULL && field_count < 6U)
        {
            fields[field_count++] = strsep(&cursor, "\t");
        }

        if (cursor != NULL || field_count != 5U ||
            !(strcmp(fields[0], "F") == 0 || strcmp(fields[0], "L") == 0))
        {
            ret = -EBADMSG;
            break;
        }

        entry_path = strcmp(fields[0], "F") == 0 ? fields[4] : fields[3];
        if (!_upgrade_manifest_valid_path(entry_path))
        {
            ret = -EBADMSG;
            break;
        }

        if (strncmp(entry_path, manifest_prefix, strlen(manifest_prefix)) != 0)
        {
            continue;
        }

        if (count >= UPGRADE_MANIFEST_MAX_ENTRIES || _upgrade_manifest_parse_fields(fields[1], fields[2], &size) != 0)
        {
            ret = -EBADMSG;
            break;
        }

        relative_path = entry_path + strlen(manifest_prefix);
        if (relative_path[0] == '\0')
        {
            ret = -EBADMSG;
            break;
        }

        entries[count].path = strdup(relative_path);
        if (entries[count].path == NULL)
        {
            ret = -ENOMEM;
            break;
        }

        entries[count].type = fields[0][0];
        entries[count].size = size;
        memcpy(entries[count].sha256, fields[2], sizeof(entries[count].sha256));

        if (entries[count].type == 'F')
        {
            ret = _upgrade_manifest_parse_mode(fields[3], &mode);
            if (ret != 0)
            {
                ret = -EBADMSG;
                break;
            }

            entries[count].mode = mode;
        }
        else
        {
            if (size >= PATH_MAX || fields[4][0] == '/' || strpbrk(fields[4], "\r\n\t") != NULL ||
                strlen(fields[4]) != (size_t)size)
            {
                ret = -EBADMSG;
                break;
            }

            entries[count].target = strdup(fields[4]);
            if (entries[count].target == NULL)
            {
                ret = -ENOMEM;
                break;
            }
        }

        ++count;
    }

    if (ret == 0 && ferror(stream))
    {
        ret = -EIO;
    }

    free(line);
    fclose(stream);

    if (ret != 0)
    {
        goto finish;
    }

    // 目录组允许为空，是否存在文件完全以已验证的FILES.txt为准。
    qsort(entries, count, sizeof(*entries), _upgrade_manifest_group_compare);
    for (index = 1U; index < count; ++index)
    {
        if (strcmp(entries[index - 1U].path, entries[index].path) == 0)
        {
            ret = -EBADMSG;
            goto finish;
        }
    }

    // 第一阶段：先统计目标目录内文件数量，不计算文件哈希。
    matches = true;
    seen    = 0U;
    ret     = _upgrade_manifest_group_scan_target(target_dir, "", entries, count, &seen, &matches, false, 0U);
    if (ret != 0 || !matches || seen != count)
    {
        goto finish;
    }

    // 第二阶段：校验名称集合及目录结构，发现额外空目录也必须替换。
    seen = 0U;
    ret  = _upgrade_manifest_group_scan_target(target_dir, "", entries, count, &seen, &matches, true, 0U);
    if (ret != 0 || !matches || seen != count)
    {
        goto finish;
    }

    // 第三阶段：先检查全部文件类型、大小、权限与软链接目标。
    for (index = 0U; index < count; ++index)
    {
        ret = _upgrade_manifest_group_check_entry(target_dir, &entries[index], false, &matches);
        if (ret != 0 || !matches)
        {
            goto finish;
        }
    }

    // 第四阶段：全部元数据一致后才计算SHA-256。
    for (index = 0U; index < count; ++index)
    {
        ret = _upgrade_manifest_group_check_entry(target_dir, &entries[index], true, &matches);
        if (ret != 0 || !matches)
        {
            goto finish;
        }
    }

    *same = true;

finish:
    for (index = 0U; index < UPGRADE_MANIFEST_MAX_ENTRIES; ++index)
    {
        free(entries[index].path);
        free(entries[index].target);
    }

    free(entries);
    free(root);
    return ret;
}
