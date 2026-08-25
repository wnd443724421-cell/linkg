/**
 * @file linkg_file.c
 * @brief LinkG文件操作实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-28
 */

#define _POSIX_C_SOURCE 200809L

#include "linkg_file.h"

#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

/****************************** 模块常量 ******************************/

#define LINKG_FILE_MODE_MASK 07777    // 文件权限有效位掩码

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查文件权限模式是否有效。
 */
static bool _mode_is_valid(mode_t mode)
{
    return (mode & ~((mode_t)LINKG_FILE_MODE_MASK)) == 0;
}

/**
 * @brief 复制字符串。
 */
static int _copy_string(char *destination, size_t destination_size, const char *source)
{
    size_t length;

    if (destination == NULL || destination_size == 0U || source == NULL)
    {
        return -EINVAL;
    }

    length = strnlen(source, destination_size);
    if (length >= destination_size)
    {
        destination[0] = '\0';
        return -ENAMETOOLONG;
    }

    memcpy(destination, source, length + 1U);
    return 0;
}

/**
 * @brief 移除路径末尾的斜杠。
 */
static void _trim_trailing_slashes(char *path)
{
    size_t length;

    if (path == NULL)
    {
        return;
    }

    length = strlen(path);
    while (length > 1U && path[length - 1U] == '/')
    {
        path[--length] = '\0';
    }
}

/**
 * @brief 为文件描述符设置执行时关闭标志。
 */
static int _descriptor_set_cloexec(int descriptor)
{
    int flags;

    if (descriptor < 0)
    {
        return -EBADF;
    }

    flags = fcntl(descriptor, F_GETFD);
    if (flags < 0)
    {
        return -errno;
    }

    if (fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) < 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 为文件流设置执行时关闭标志。
 */
static int _stream_set_cloexec(FILE *stream)
{
    int descriptor;

    if (stream == NULL)
    {
        return -EINVAL;
    }

    descriptor = fileno(stream);
    if (descriptor < 0)
    {
        return errno != 0 ? -errno : -EBADF;
    }

    return _descriptor_set_cloexec(descriptor);
}

/**
 * @brief 确保指定目录存在。
 */
static int _ensure_directory(const char *path, mode_t mode)
{
    struct stat status;

    if (stat(path, &status) == 0)
    {
        return S_ISDIR(status.st_mode) ? 0 : -ENOTDIR;
    }

    if (errno != ENOENT)
    {
        return -errno;
    }

    if (mkdir(path, mode) == 0)
    {
        return 0;
    }

    if (errno == EEXIST && stat(path, &status) == 0 && S_ISDIR(status.st_mode))
    {
        return 0;
    }

    return -errno;
}

/**
 * @brief 同步目标文件所在父目录。
 */
static int _sync_parent_directory(const char *path)
{
    char  parent[LINKG_FILE_PATH_MAX];
    char *separator;
    int   descriptor;
    int   flags;
    int   result;

    result = _copy_string(parent, sizeof(parent), path);
    if (result != 0)
    {
        return result;
    }

    separator = strrchr(parent, '/');
    if (separator == NULL)
    {
        parent[0] = '.';
        parent[1] = '\0';
    }
    else if (separator == parent)
    {
        parent[1] = '\0';
    }
    else
    {
        *separator = '\0';
    }

    flags = O_RDONLY;

#ifdef O_DIRECTORY
    flags |= O_DIRECTORY;
#endif

    descriptor = open(parent, flags);
    if (descriptor < 0)
    {
        return -errno;
    }

    result = _descriptor_set_cloexec(descriptor);
    if (result == 0 && fsync(descriptor) != 0)
    {
        result = -errno;
    }

    if (close(descriptor) != 0 && result == 0)
    {
        result = -errno;
    }

    return result;
}

/****************************** 文件流 ******************************/

/**
 * @brief 打开文件流。
 */
int  linkg_file_stream_open(const char *path, const char *mode, FILE **stream)
{
    FILE *opened_stream;
    int   result;

    if (path == NULL || mode == NULL || stream == NULL)
    {
        return -EINVAL;
    }

    *stream = NULL;

    opened_stream = fopen(path, mode);
    if (opened_stream == NULL)
    {
        return -errno;
    }

    result = _stream_set_cloexec(opened_stream);
    if (result != 0)
    {
        (void)fclose(opened_stream);
        return result;
    }

    *stream = opened_stream;
    return 0;
}

/**
 * @brief 关闭文件流。
 */
int  linkg_file_stream_close(FILE **stream)
{
    FILE *closing_stream;

    if (stream == NULL)
    {
        return -EINVAL;
    }

    closing_stream = *stream;
    *stream        = NULL;

    if (closing_stream == NULL)
    {
        return 0;
    }

    if (fclose(closing_stream) != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 设置文件流当前位置。
 */
int  linkg_file_stream_seek(FILE *stream, off_t offset, int whence)
{
    if (stream == NULL)
    {
        return -EINVAL;
    }

    if (fseeko(stream, offset, whence) != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 从文件流读取指定长度的数据。
 */
int  linkg_file_stream_read_exact(FILE *stream, void *buffer, size_t length)
{
    uint8_t *cursor;
    size_t   count;
    size_t   total;

    if (stream == NULL || (buffer == NULL && length != 0U))
    {
        return -EINVAL;
    }

    cursor = (uint8_t *)buffer;
    total  = 0U;

    while (total < length)
    {
        count = fread(cursor + total, 1U, length - total, stream);
        if (count == 0U)
        {
            if (ferror(stream))
            {
                return errno != 0 ? -errno : -EIO;
            }

            return -EIO;
        }

        total += count;
    }

    return 0;
}

/**
 * @brief 向文件流写入指定长度的数据。
 */
int  linkg_file_stream_write_exact(FILE *stream, const void *data, size_t length)
{
    const uint8_t *cursor;
    size_t         count;
    size_t         total;

    if (stream == NULL || (data == NULL && length != 0U))
    {
        return -EINVAL;
    }

    cursor = (const uint8_t *)data;
    total  = 0U;

    while (total < length)
    {
        count = fwrite(cursor + total, 1U, length - total, stream);
        if (count == 0U)
        {
            return ferror(stream) && errno != 0 ? -errno : -EIO;
        }

        total += count;
    }

    return 0;
}

/**
 * @brief 刷新文件流并按需同步到存储设备。
 */
int  linkg_file_stream_flush(FILE *stream, bool sync_to_disk)
{
    int descriptor;

    if (stream == NULL)
    {
        return -EINVAL;
    }

    if (fflush(stream) != 0)
    {
        return -errno;
    }

    if (!sync_to_disk)
    {
        return 0;
    }

    descriptor = fileno(stream);
    if (descriptor < 0)
    {
        return errno != 0 ? -errno : -EBADF;
    }

    if (fsync(descriptor) != 0)
    {
        return -errno;
    }

    return 0;
}

/****************************** 目录与空间 ******************************/

/**
 * @brief 递归创建指定目录。
 */
int  linkg_file_mkdirs(const char *directory_path, mode_t mode)
{
    char  path[LINKG_FILE_PATH_MAX];
    char *cursor;
    int   result;

    if (directory_path == NULL || directory_path[0] == '\0' || !_mode_is_valid(mode))
    {
        return -EINVAL;
    }

    result = _copy_string(path, sizeof(path), directory_path);
    if (result != 0)
    {
        return result;
    }

    _trim_trailing_slashes(path);

    for (cursor = path + 1; *cursor != '\0'; cursor++)
    {
        if (*cursor != '/')
        {
            continue;
        }

        *cursor = '\0';
        result  = _ensure_directory(path, mode);
        *cursor = '/';

        if (result != 0)
        {
            return result;
        }
    }

    return _ensure_directory(path, mode);
}

/**
 * @brief 查询指定路径所在文件系统的可用空间。
 */
int  linkg_file_path_free_bytes(const char *path, uint64_t *free_bytes)
{
    struct statvfs status;

    if (path == NULL || free_bytes == NULL)
    {
        return -EINVAL;
    }

    *free_bytes = 0U;

    if (statvfs(path, &status) != 0)
    {
        return errno != 0 ? -errno : -EIO;
    }

    if (status.f_frsize == 0U)
    {
        return -EIO;
    }

    if ((uint64_t)status.f_bavail > UINT64_MAX / (uint64_t)status.f_frsize)
    {
        return -EOVERFLOW;
    }

    *free_bytes = (uint64_t)status.f_bavail * (uint64_t)status.f_frsize;
    return 0;
}

/****************************** 文件读写 ******************************/

/**
 * @brief 读取文件的全部内容。
 */
int  linkg_file_read_all(const char *path, size_t max_length, char **data, size_t *length)
{
    struct stat status;
    FILE       *stream = NULL;
    char       *buffer = NULL;
    size_t      file_size;
    int         close_result;
    int         fd;
    int         result;

    if (path == NULL || data == NULL)
    {
        return -EINVAL;
    }

    *data = NULL;

    if (length != NULL)
    {
        *length = 0U;
    }

    result = linkg_file_stream_open(path, "rb", &stream);
    if (result != 0)
    {
        return result;
    }

    fd = fileno(stream);
    if (fd < 0)
    {
        result = errno != 0 ? -errno : -EBADF;
        goto cleanup;
    }

    if (fstat(fd, &status) != 0)
    {
        result = -errno;
        goto cleanup;
    }

    if (!S_ISREG(status.st_mode) || status.st_size < 0)
    {
        result = -EINVAL;
        goto cleanup;
    }

    if ((uintmax_t)status.st_size > (uintmax_t)SIZE_MAX - 1U)
    {
        result = -EOVERFLOW;
        goto cleanup;
    }

    file_size = (size_t)status.st_size;

    if (max_length > 0U && file_size > max_length)
    {
        result = -EFBIG;
        goto cleanup;
    }

    buffer = malloc(file_size + 1U);
    if (buffer == NULL)
    {
        result = -ENOMEM;
        goto cleanup;
    }

    if (file_size > 0U)
    {
        result = linkg_file_stream_read_exact(stream, buffer, file_size);
        if (result != 0)
        {
            goto cleanup;
        }
    }

    buffer[file_size] = '\0';

    result = linkg_file_stream_close(&stream);
    if (result != 0)
    {
        free(buffer);
        return result;
    }

    *data = buffer;

    if (length != NULL)
    {
        *length = file_size;
    }

    return 0;

cleanup:
    free(buffer);

    close_result = linkg_file_stream_close(&stream);
    if (result == 0)
    {
        result = close_result;
    }

    return result;
}

/**
 * @brief 将全部数据直接写入指定文件或虚拟节点。
 */
int  linkg_file_write_all(const char *path, const void *data, size_t length)
{
    const unsigned char *buffer;
    size_t               written_length;
    ssize_t              write_length;
    int                  saved_errno;
    int                  fd;

    if (path == NULL || data == NULL || length == 0U)
    {
        return -EINVAL;
    }

    fd = open(path, O_WRONLY | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }

    buffer         = data;
    written_length = 0U;

    while (written_length < length)
    {
        write_length = write(fd, buffer + written_length, length - written_length);
        if (write_length > 0)
        {
            written_length += (size_t)write_length;
            continue;
        }

        if (write_length < 0 && errno == EINTR)
        {
            continue;
        }

        saved_errno = (write_length < 0) ? errno : EIO;
        close(fd);

        return -saved_errno;
    }

    if (close(fd) != 0)
    {
        return -errno;
    }

    return 0;
}

/****************************** 原子文件操作 ******************************/

/**
 * @brief 原子写入完整文件内容。
 */
int  linkg_file_write_all_atomic(const char *path, const void *data, size_t length, mode_t mode)
{
    char  temporary_path[LINKG_FILE_PATH_MAX];
    FILE *stream;
    bool  renamed;
    int   descriptor;
    int   formatted_length;
    int   result;

    if (path == NULL || path[0] == '\0' ||
        (data == NULL && length != 0U) ||
        !_mode_is_valid(mode))
    {
        return -EINVAL;
    }

    formatted_length = snprintf(temporary_path, sizeof(temporary_path), "%s.tmp.XXXXXX", path);
    if (formatted_length < 0 || (size_t)formatted_length >= sizeof(temporary_path))
    {
        return -ENAMETOOLONG;
    }

    stream     = NULL;
    renamed    = false;
    descriptor = mkstemp(temporary_path);

    if (descriptor < 0)
    {
        return -errno;
    }

    result = _descriptor_set_cloexec(descriptor);
    if (result == 0 && fchmod(descriptor, mode) != 0)
    {
        result = -errno;
    }

    if (result == 0)
    {
        stream = fdopen(descriptor, "wb");
        if (stream == NULL)
        {
            result = -errno;
        }
        else
        {
            descriptor = -1;
        }
    }

    if (result == 0)
    {
        result = linkg_file_stream_write_exact(stream, data, length);
    }

    if (result == 0)
    {
        result = linkg_file_stream_flush(stream, true);
    }

    if (stream != NULL)
    {
        int close_result;

        close_result = linkg_file_stream_close(&stream);
        if (result == 0)
        {
            result = close_result;
        }
    }
    else if (descriptor >= 0)
    {
        if (close(descriptor) != 0 && result == 0)
        {
            result = -errno;
        }
    }

    if (result == 0)
    {
        if (rename(temporary_path, path) != 0)
        {
            result = -errno;
        }
        else
        {
            renamed = true;
        }
    }

    if (result == 0)
    {
        result = _sync_parent_directory(path);
    }

    if (!renamed)
    {
        (void)unlink(temporary_path);
    }

    return result;
}

/**
 * @brief 使用源文件内容原子替换目标文件。
 *
 * @note 源文件必须是非空普通文件，源文件本身不会被修改。
 */
int  linkg_file_replace_atomic(const char *source_path, const char *target_path, size_t max_length)
{
    struct stat status;
    char       *data;
    size_t      length;
    mode_t      mode;
    int         result;

    if (source_path == NULL || source_path[0] == '\0' ||
        target_path == NULL || target_path[0] == '\0')
    {
        return -EINVAL;
    }

    if (stat(source_path, &status) != 0)
    {
        return errno != 0 ? -errno : -EIO;
    }

    if (!S_ISREG(status.st_mode))
    {
        return -EINVAL;
    }

    mode   = status.st_mode & LINKG_FILE_MODE_MASK;
    data   = NULL;
    length = 0U;

    result = linkg_file_read_all(source_path, max_length, &data, &length);
    if (result != 0)
    {
        return result;
    }

    if (length == 0U)
    {
        free(data);
        return -ENODATA;
    }

    result = linkg_file_write_all_atomic(target_path, data, length, mode);

    free(data);
    return result;
}
