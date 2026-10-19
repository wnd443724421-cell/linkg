/**
 * @file upgrade.c
 * @brief LinkG Web CGI固件包及配置文件上传工具
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-19
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

/****************************** 上传配置 ******************************/

#define LINKG_UPLOAD_UPGRADE_PATH       "/tmp/upgrade.tar.gz"     // 固件升级包暂存路径
#define LINKG_UPLOAD_CONFIG_PATH        "/tmp/config_import.json" // 配置导入文件暂存路径
#define LINKG_UPLOAD_TEMP_TEMPLATE      "/tmp/.linkg-upload.XXXXXX" // 同目录原子替换临时文件
#define LINKG_UPLOAD_LOCK_PATH          "/tmp/.linkg-upload.lock" // 并发上传互斥文件
#define LINKG_UPLOAD_UPGRADE_MAX_SIZE   (64U * 1024U * 1024U)    // 升级包大小上限
#define LINKG_UPLOAD_CONFIG_MAX_SIZE    (1U * 1024U * 1024U)     // 配置文件大小上限
#define LINKG_UPLOAD_CHUNK_SIZE         (8U * 1024U)             // 流式传输缓冲区

/****************************** 响应工具 ******************************/

/**
 * @brief 输出CGI JSON响应。
 */
static void _upload_print_response(int status, const char *reason, const char *code, const char *message, size_t size)
{
    printf("Status: %d %s\r\n", status, reason);
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n\r\n");
    printf("{\"code\":\"%s\",\"msg\":\"%s\",\"size\":%zu}", code, message, size);
    (void)fflush(stdout);
}

/****************************** 请求校验 ******************************/

/**
 * @brief 读取并验证请求体长度。
 */
static int _upload_parse_length(size_t max_size, size_t *length)
{
    const char         *value;
    const char         *cursor;
    char               *end;
    unsigned long long  parsed;

    if (length == NULL)
    {
        return -EINVAL;
    }

    value = getenv("CONTENT_LENGTH");
    if (value == NULL || value[0] == '\0')
    {
        return -EINVAL;
    }

    for (cursor = value; *cursor != '\0'; cursor++)
    {
        if (*cursor < '0' || *cursor > '9')
        {
            return -EINVAL;
        }
    }

    errno  = 0;
    end    = NULL;
    parsed = strtoull(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0' || parsed == 0ULL)
    {
        return -EINVAL;
    }

    if (parsed > max_size)
    {
        return -EFBIG;
    }

    *length = (size_t)parsed;
    return 0;
}

/****************************** 文件处理 ******************************/

/**
 * @brief 获取单实例上传锁，阻止两个CGI相互覆盖上传内容。
 */
static int _upload_acquire_lock(void)
{
    struct stat st;
    int         fd;
    int         error;

    fd = open(LINKG_UPLOAD_LOCK_PATH, O_CREAT | O_RDWR | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0)
    {
        return -errno;
    }

    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || st.st_uid != geteuid() || st.st_nlink != 1)
    {
        close(fd);
        return -EPERM;
    }

    if (flock(fd, LOCK_EX | LOCK_NB) != 0)
    {
        error = -errno;
        close(fd);
        return error;
    }

    return fd;
}

/**
 * @brief 将指定长度的CGI输入流完整写入文件。
 */
static int _upload_receive(int fd, size_t length)
{
    unsigned char buffer[LINKG_UPLOAD_CHUNK_SIZE];
    size_t        received;

    received = 0U;

    while (received < length)
    {
        size_t  wanted;
        ssize_t count;
        size_t  written;

        wanted = length - received;
        if (wanted > sizeof(buffer))
        {
            wanted = sizeof(buffer);
        }

        count = read(STDIN_FILENO, buffer, wanted);
        if (count < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -errno;
        }

        if (count == 0)
        {
            return -ENODATA;
        }

        written = 0U;
        while (written < (size_t)count)
        {
            ssize_t result;

            result = write(fd, buffer + written, (size_t)count - written);
            if (result < 0)
            {
                if (errno == EINTR)
                {
                    continue;
                }

                return -errno;
            }

            if (result == 0)
            {
                return -EIO;
            }

            written += (size_t)result;
        }

        received += (size_t)count;
    }

    return 0;
}

/****************************** 主程序 ******************************/

/**
 * @brief 接收并原子发布一个升级包或配置文件。
 */
int main(void)
{
    const char *method;
    const char *type;
    const char *target;
    char        temporary[] = LINKG_UPLOAD_TEMP_TEMPLATE;
    size_t      limit;
    size_t      length;
    int         lock_fd;
    int         file_fd;
    int         ret;

    method = getenv("REQUEST_METHOD");
    if (method == NULL || strcmp(method, "POST") != 0)
    {
        _upload_print_response(405, "Method Not Allowed", "error", "Only POST is supported", 0U);
        return EXIT_SUCCESS;
    }

    type = getenv("HTTP_X_UPLOAD_TYPE");
    if (type == NULL || strcmp(type, "upgrade") == 0)
    {
        target = LINKG_UPLOAD_UPGRADE_PATH;
        limit  = LINKG_UPLOAD_UPGRADE_MAX_SIZE;
    }
    else if (strcmp(type, "config") == 0)
    {
        target = LINKG_UPLOAD_CONFIG_PATH;
        limit  = LINKG_UPLOAD_CONFIG_MAX_SIZE;
    }
    else
    {
        _upload_print_response(400, "Bad Request", "error", "Invalid upload type", 0U);
        return EXIT_SUCCESS;
    }

    ret = _upload_parse_length(limit, &length);
    if (ret != 0)
    {
        if (ret == -EFBIG)
        {
            _upload_print_response(413, "Payload Too Large", "error", "Upload is too large", 0U);
        }
        else
        {
            _upload_print_response(400, "Bad Request", "error", "Invalid content length", 0U);
        }

        return EXIT_SUCCESS;
    }

    lock_fd = _upload_acquire_lock();
    if (lock_fd < 0)
    {
        if (lock_fd == -EWOULDBLOCK || lock_fd == -EAGAIN)
        {
            _upload_print_response(409, "Conflict", "error", "Another upload is in progress", 0U);
        }
        else
        {
            _upload_print_response(500, "Internal Server Error", "error", "Cannot acquire upload lock", 0U);
        }

        return EXIT_SUCCESS;
    }

    file_fd = mkstemp(temporary);
    if (file_fd < 0)
    {
        close(lock_fd);
        _upload_print_response(500, "Internal Server Error", "error", "Cannot create temporary file", 0U);
        return EXIT_SUCCESS;
    }

    ret = _upload_receive(file_fd, length);
    if (ret == 0 && fsync(file_fd) != 0)
    {
        ret = -errno;
    }

    if (close(file_fd) != 0 && ret == 0)
    {
        ret = -errno;
    }

    if (ret == 0 && rename(temporary, target) != 0)
    {
        ret = -errno;
    }

    if (ret != 0)
    {
        (void)unlink(temporary);
    }

    close(lock_fd);

    if (ret == -ENODATA)
    {
        _upload_print_response(400, "Bad Request", "error", "Incomplete upload", 0U);
    }
    else if (ret != 0)
    {
        _upload_print_response(500, "Internal Server Error", "error", "Upload failed", 0U);
    }
    else
    {
        _upload_print_response(200, "OK", "ok", "Success", length);
    }

    return EXIT_SUCCESS;
}
