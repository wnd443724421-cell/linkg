/**
 * @file upgrade_system.c
 * @brief LinkG升级包可选系统文件替换
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "upgrade_system.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "cJSON.h"

/****************************** 模块常量 ******************************/

#define UPGRADE_SYSTEM_JSON_NAME          "system_install.json"// 可选的系统文件替换声明
#define UPGRADE_SYSTEM_JSON_MAX_BYTES    (64U * 1024U) // JSON最大长度
#define UPGRADE_SYSTEM_MAX_FILES         64U           // 单次最多替换的系统文件数
#define UPGRADE_SYSTEM_COPY_BUFFER_BYTES 16384U        // 分块复制缓冲区大小

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查JSON对象恰好包含指定两个键，不允许未知键和重复键。
 */
static bool _upgrade_system_keys(const cJSON *object, const char *first, const char *second)
{
    const cJSON *item;
    unsigned int first_count;
    unsigned int second_count;

    if (!cJSON_IsObject(object))
    {
        return false;
    }

    first_count  = 0U;
    second_count = 0U;
    for (item = object->child; item != NULL; item = item->next)
    {
        if (item->string == NULL)
        {
            return false;
        }
        if (strcmp(item->string, first) == 0)
        {
            first_count++;
        }
        else if (strcmp(item->string, second) == 0)
        {
            second_count++;
        }
        else
        {
            return false;
        }
    }

    return first_count == 1U && second_count == 1U;
}

/**
 * @brief 校验相对或绝对路径的组成部分，不接受空段、点段和控制字符。
 */
static bool _upgrade_system_path_valid(const char *path, bool absolute)
{
    const char *segment;
    const char *cursor;
    size_t      length;

    if (path == NULL || (path[0] == '/' ? true : false) != absolute)
    {
        return false;
    }

    length = strnlen(path, PATH_MAX);
    if (length == 0U || length >= PATH_MAX)
    {
        return false;
    }

    segment = path + (absolute ? 1U : 0U);
    if (*segment == '\0')
    {
        return false;
    }

    for (cursor = segment; ; cursor++)
    {
        if ((unsigned char)*cursor < 32U || (unsigned char)*cursor == 127U)
        {
            return *cursor == '\0' && cursor > segment &&
                   (size_t)(cursor - segment) != 1U &&
                   !((size_t)(cursor - segment) == 2U && segment[0] == '.' && segment[1] == '.');
        }

        if (*cursor == '/')
        {
            if (cursor == segment || ((size_t)(cursor - segment) == 1U && segment[0] == '.'))
            {
                return false;
            }
            if ((size_t)(cursor - segment) == 2U && segment[0] == '.' && segment[1] == '.')
            {
                return false;
            }
            segment = cursor + 1;
        }
    }
}

/**
 * @brief 只允许固件资源和固定的Supervisor程序，禁止任意系统路径覆盖。
 */
static bool _upgrade_system_destination_allowed(const char *destination)
{
    return strcmp(destination, "/usr/sbin/linkg-supervisor") == 0 ||
           strncmp(destination, "/vendor/firmware/", sizeof("/vendor/firmware/") - 1U) == 0 ||
           strncmp(destination, "/lib/firmware/", sizeof("/lib/firmware/") - 1U) == 0;
}

/**
 * @brief source默认相对包内app目录，同时兼容原有app/前缀声明。
 */
static int _upgrade_system_source_path(const char *source, char *path, size_t capacity)
{
    int length;

    if (!_upgrade_system_path_valid(source, false))
    {
        return -EINVAL;
    }

    length = snprintf(path, capacity, "%s%s", strncmp(source, "app/", 4U) == 0 ? "" : "app/", source);
    return length < 0 || (size_t)length >= capacity ? -ENAMETOOLONG : 0;
}

/**
 * @brief 从已打开根目录逐级打开真实目录，可按需创建缺失目录。
 */
static int _upgrade_system_open_dirs(int root_fd, const char *relative, bool create)
{
    char  path[PATH_MAX];
    char *part;
    char *save;
    int   current;
    int   next;

    current = dup(root_fd);
    if (current < 0)
    {
        return -errno;
    }
    if (relative[0] == '\0')
    {
        return current;
    }

    (void)snprintf(path, sizeof(path), "%s", relative);
    for (part = strtok_r(path, "/", &save); part != NULL; part = strtok_r(NULL, "/", &save))
    {
        if (create && mkdirat(current, part, 0755) != 0 && errno != EEXIST)
        {
            next = -errno;
            close(current);
            return next;
        }

        next = openat(current, part, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        close(current);
        if (next < 0)
        {
            return -errno;
        }
        current = next;
    }

    return current;
}

/**
 * @brief 只打开升级包内的普通文件，拒绝路径中任何符号链接。
 */
static int _upgrade_system_open_source(int root_fd, const char *relative)
{
    char         path[PATH_MAX];
    char        *filename;
    struct stat  information;
    int          parent_fd;
    int          descriptor;
    int          ret;

    (void)snprintf(path, sizeof(path), "%s", relative);
    filename = strrchr(path, '/');
    if (filename == NULL)
    {
        parent_fd = dup(root_fd);
        filename  = path;
    }
    else
    {
        *filename++ = '\0';
        parent_fd   = _upgrade_system_open_dirs(root_fd, path, false);
    }
    if (parent_fd < 0)
    {
        return parent_fd;
    }

    descriptor = openat(parent_fd, filename, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    ret        = descriptor < 0 ? -errno : 0;
    close(parent_fd);
    if (ret != 0)
    {
        return ret;
    }

    if (fstat(descriptor, &information) != 0)
    {
        ret = -errno;
    }
    else if (!S_ISREG(information.st_mode))
    {
        ret = -EINVAL;
    }
    if (ret != 0)
    {
        close(descriptor);
        return ret;
    }

    return descriptor;
}

/**
 * @brief 将文件分块复制到同目录临时文件，再原子替换目标并持久化。
 *
 * 目标路径父目录不允许符号链接，原有目标只能是普通文件。
 */
static int _upgrade_system_copy(int source_fd, const char *destination)
{
    char         path[PATH_MAX];
    char         temporary[64];
    unsigned char buffer[UPGRADE_SYSTEM_COPY_BUFFER_BYTES];
    char        *filename;
    struct stat  information;
    struct stat  existing;
    ssize_t      length;
    ssize_t      written;
    size_t       offset;
    int          root_fd;
    int          parent_fd;
    int          temporary_fd;
    int          ret;
    unsigned int attempt;

    if (fstat(source_fd, &information) != 0 || !S_ISREG(information.st_mode))
    {
        return -EINVAL;
    }

    root_fd = open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (root_fd < 0)
    {
        return -errno;
    }

    (void)snprintf(path, sizeof(path), "%s", destination + 1);
    filename = strrchr(path, '/');
    if (filename == NULL)
    {
        parent_fd = dup(root_fd);
        filename  = path;
    }
    else
    {
        *filename++ = '\0';
        parent_fd   = _upgrade_system_open_dirs(root_fd, path, true);
    }
    close(root_fd);
    if (parent_fd < 0)
    {
        return parent_fd;
    }

    if (fstatat(parent_fd, filename, &existing, AT_SYMLINK_NOFOLLOW) == 0)
    {
        if (!S_ISREG(existing.st_mode))
        {
            close(parent_fd);
            return -EPERM;
        }
    }
    else if (errno != ENOENT)
    {
        ret = -errno;
        close(parent_fd);
        return ret;
    }

    temporary_fd = -1;
    for (attempt = 0U; attempt < 100U; attempt++)
    {
        (void)snprintf(temporary, sizeof(temporary), ".linkg-system-%ld-%u", (long)getpid(), attempt);
        temporary_fd = openat(parent_fd, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (temporary_fd >= 0)
        {
            break;
        }
        if (errno != EEXIST)
        {
            break;
        }
    }
    if (temporary_fd < 0)
    {
        ret = -errno;
        close(parent_fd);
        return ret;
    }

    ret = 0;
    for (;;)
    {
        length = read(source_fd, buffer, sizeof(buffer));
        if (length < 0 && errno == EINTR)
        {
            continue;
        }
        if (length < 0)
        {
            ret = -errno;
            break;
        }
        if (length == 0)
        {
            break;
        }
        for (offset = 0U; offset < (size_t)length; offset += (size_t)written)
        {
            written = write(temporary_fd, buffer + offset, (size_t)length - offset);
            if (written < 0 && errno == EINTR)
            {
                continue;
            }
            if (written <= 0)
            {
                ret = written < 0 ? -errno : -EIO;
                break;
            }
        }
        if (ret != 0)
        {
            break;
        }
    }

    if (ret == 0 && fchmod(temporary_fd, information.st_mode & 0777U) != 0)
    {
        ret = -errno;
    }
    if (ret == 0 && fsync(temporary_fd) != 0)
    {
        ret = -errno;
    }
    if (close(temporary_fd) != 0 && ret == 0)
    {
        ret = -errno;
    }
    if (ret == 0 && renameat(parent_fd, temporary, parent_fd, filename) != 0)
    {
        ret = -errno;
    }
    if (ret == 0 && fsync(parent_fd) != 0)
    {
        ret = -errno;
    }
    if (ret != 0)
    {
        (void)unlinkat(parent_fd, temporary, 0);
    }
    close(parent_fd);
    return ret;
}

/****************************** 安装接口 ******************************/

/**
 * @brief 可选安装system_install.json指定的系统文件，缺少声明时直接跳过。
 *
 * 调用前必须完成Manifest校验；成功返回只表示声明中的文件已替换，
 * 不代表整个OTA成功，也不包含已替换共享文件的回滚操作。
 */
int upgrade_system_install(const char *upgrade_dir)
{
    char         json[UPGRADE_SYSTEM_JSON_MAX_BYTES + 1U];
    char         source_path[PATH_MAX];
    struct stat  information;
    cJSON       *root;
    const cJSON *files;
    const cJSON *item;
    const cJSON *source;
    const cJSON *destination;
    const cJSON *other;
    const cJSON *version;
    size_t       total;
    ssize_t      count;
    int          root_fd;
    int          json_fd;
    int          source_fd;
    int          entries;
    int          index;
    int          previous;
    int          ret;

    if (upgrade_dir == NULL)
    {
        return -EINVAL;
    }

    root_fd = open(upgrade_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (root_fd < 0)
    {
        return -errno;
    }

    json_fd = openat(root_fd, UPGRADE_SYSTEM_JSON_NAME, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (json_fd < 0)
    {
        ret = errno == ENOENT ? 0 : -errno;
        close(root_fd);
        return ret;
    }

    ret = fstat(json_fd, &information);
    if (ret != 0 || !S_ISREG(information.st_mode) || information.st_size <= 0 ||
        information.st_size > (off_t)UPGRADE_SYSTEM_JSON_MAX_BYTES)
    {
        ret = ret != 0 ? -errno : -EINVAL;
        goto cleanup_json;
    }

    total = 0U;
    while (total < (size_t)information.st_size)
    {
        count = read(json_fd, json + total, (size_t)information.st_size - total);
        if (count < 0 && errno == EINTR)
        {
            continue;
        }
        if (count <= 0)
        {
            ret = count == 0 ? -EIO : -errno;
            goto cleanup_json;
        }
        total += (size_t)count;
    }
    if (memchr(json, '\0', total) != NULL)
    {
        ret = -EINVAL;
        goto cleanup_json;
    }
    json[total] = '\0';
    close(json_fd);
    json_fd = -1;

    // cJSON字符串接口不提供长度，禁止使用\u0000隐藏路径后缀。
    if (strstr(json, "\\u0000") != NULL)
    {
        ret = -EINVAL;
        goto cleanup_root;
    }

    root = cJSON_ParseWithOpts(json, NULL, 1);
    if (!_upgrade_system_keys(root, "schema_version", "files"))
    {
        ret = -EINVAL;
        goto cleanup_parsed;
    }

    version = cJSON_GetObjectItemCaseSensitive(root, "schema_version");
    files   = cJSON_GetObjectItemCaseSensitive(root, "files");
    entries = cJSON_IsArray(files) ? cJSON_GetArraySize(files) : 0;
    if (!cJSON_IsNumber(version) || version->valuedouble != 1.0 || entries <= 0 || entries > (int)UPGRADE_SYSTEM_MAX_FILES)
    {
        ret = -EINVAL;
        goto cleanup_parsed;
    }

    // 先验证全部声明和源文件，避免后续条目有错时提前修改系统。
    for (index = 0; index < entries; index++)
    {
        item        = cJSON_GetArrayItem(files, index);
        source      = cJSON_GetObjectItemCaseSensitive(item, "source");
        destination = cJSON_GetObjectItemCaseSensitive(item, "destination");
        if (!_upgrade_system_keys(item, "source", "destination") ||
            !cJSON_IsString(source) || !cJSON_IsString(destination) ||
            !_upgrade_system_path_valid(destination->valuestring, true) ||
            !_upgrade_system_destination_allowed(destination->valuestring) ||
            _upgrade_system_source_path(source->valuestring, source_path, sizeof(source_path)) != 0)
        {
            ret = -EINVAL;
            goto cleanup_parsed;
        }

        for (previous = 0; previous < index; previous++)
        {
            other = cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(files, previous), "destination");
            if (strcmp(destination->valuestring, other->valuestring) == 0)
            {
                ret = -EEXIST;
                goto cleanup_parsed;
            }
        }

        source_fd = _upgrade_system_open_source(root_fd, source_path);
        if (source_fd < 0)
        {
            ret = source_fd;
            goto cleanup_parsed;
        }
        ret = fstat(source_fd, &information) == 0 ? 0 : -errno;
        if (ret == 0 && strcmp(destination->valuestring, "/usr/sbin/linkg-supervisor") == 0 &&
            (information.st_mode & 0111U) == 0U)
        {
            ret = -EACCES;
        }
        close(source_fd);
        if (ret != 0)
        {
            goto cleanup_parsed;
        }
    }

    for (index = 0; index < entries; index++)
    {
        item        = cJSON_GetArrayItem(files, index);
        source      = cJSON_GetObjectItemCaseSensitive(item, "source");
        destination = cJSON_GetObjectItemCaseSensitive(item, "destination");
        ret         = _upgrade_system_source_path(source->valuestring, source_path, sizeof(source_path));
        if (ret != 0)
        {
            goto cleanup_parsed;
        }
        source_fd = _upgrade_system_open_source(root_fd, source_path);
        if (source_fd < 0)
        {
            ret = source_fd;
            goto cleanup_parsed;
        }
        ret = _upgrade_system_copy(source_fd, destination->valuestring);
        close(source_fd);
        if (ret != 0)
        {
            goto cleanup_parsed;
        }
    }

    ret = 0;

cleanup_parsed:
    cJSON_Delete(root);
cleanup_root:
    close(root_fd);
    return ret;

cleanup_json:
    close(json_fd);
    goto cleanup_root;
}
