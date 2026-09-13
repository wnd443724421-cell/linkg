/**
 * @file cmain.c
 * @brief LinkG Web CGI与本机控制服务之间的JSON转发工具
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-13
 */

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "linkg/platform/linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_CMAIN_REQUEST_MAX        (32U * 1024U)  // 最大请求长度
#define LINKG_CMAIN_RESPONSE_INIT      (8U * 1024U)   // 响应初始缓冲区大小
#define LINKG_CMAIN_RESPONSE_MAX       (256U * 1024U) // 最大响应长度
#define LINKG_CMAIN_RECV_CHUNK         (4U * 1024U)   // 单次接收缓冲区大小
#define LINKG_CMAIN_IO_TIMEOUT_SEC     3              // 后端收发超时时间

/****************************** 内部辅助 ******************************/

/**
 * @brief 输出JSON格式HTTP响应。
 */
static void _cmain_print_json_response(int status, const char *reason, const char *body, size_t body_len)
{
    printf("Status: %d %s\r\n", status, reason);
    printf("Content-Type: application/json; charset=utf-8\r\n");
    printf("Cache-Control: no-store\r\n");
    printf("Content-Length: %zu\r\n", body_len);
    printf("\r\n");

    if (body != NULL && body_len > 0U)
    {
        (void)fwrite(body, 1U, body_len, stdout);
    }

    (void)fflush(stdout);
}

/**
 * @brief 输出CGI本地错误响应。
 */
static void _cmain_print_error(int status, const char *reason, const char *error, const char *message)
{
    char body[256];
    int length;

    length = snprintf(body, sizeof(body), "{\"code\":\"error\",\"error\":\"%s\",\"msg\":\"%s\"}", error, message);

    if (length < 0)
    {
        return;
    }

    if ((size_t)length >= sizeof(body))
    {
        length = (int)(sizeof(body) - 1U);
    }

    _cmain_print_json_response(status, reason, body, (size_t)length);
}

/**
 * @brief 解析CGI请求体长度。
 */
static int _cmain_parse_content_length(size_t *content_length)
{
    const char *value;
    char *end;
    unsigned long parsed;

    if (content_length == NULL)
    {
        return -EINVAL;
    }

    value = getenv("CONTENT_LENGTH");

    if (value == NULL || value[0] == '\0')
    {
        return -EINVAL;
    }

    errno = 0;
    end = NULL;
    parsed = strtoul(value, &end, 10);

    if (errno != 0 || end == value || end == NULL || *end != '\0' || parsed == 0UL)
    {
        return -EINVAL;
    }

    if (parsed > LINKG_CMAIN_REQUEST_MAX)
    {
        return -EFBIG;
    }

    *content_length = (size_t)parsed;

    return 0;
}

/**
 * @brief 从CGI标准输入完整读取请求体。
 */
static int _cmain_read_request(size_t content_length, char **request)
{
    char *buffer;
    size_t received = 0U;

    if (content_length == 0U || request == NULL)
    {
        return -EINVAL;
    }

    *request = NULL;

    buffer = (char *)malloc(content_length + 1U);

    if (buffer == NULL)
    {
        return -ENOMEM;
    }

    while (received < content_length)
    {
        ssize_t ret;

        ret = read(STDIN_FILENO, buffer + received, content_length - received);

        if (ret > 0)
        {
            received += (size_t)ret;
            continue;
        }

        if (ret == 0)
        {
            free(buffer);
            return -EIO;
        }

        if (errno == EINTR)
        {
            continue;
        }

        free(buffer);
        return -errno;
    }

    buffer[content_length] = '\0';

    *request = buffer;

    return 0;
}

/**
 * @brief 创建并连接LinkG本机Web控制服务。
 */
static int _cmain_connect_backend(void)
{
    struct sockaddr_in address;
    struct timeval timeout;
    int fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0)
    {
        return -1;
    }

    timeout.tv_sec = LINKG_CMAIN_IO_TIMEOUT_SEC;
    timeout.tv_usec = 0;

    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        close(fd);
        return -1;
    }

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        close(fd);
        return -1;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family = AF_INET;
    address.sin_port = htons(LINKG_RESOURCE_TCP_PORT_WEB_CONTROL);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (connect(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

/**
 * @brief 完整发送指定长度的数据。
 */
static int _cmain_send_all(int fd, const void *buffer, size_t length)
{
    const char *data = (const char *)buffer;
    size_t sent = 0U;
    int flags = 0;

    if (fd < 0 || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif

    while (sent < length)
    {
        ssize_t ret;

        ret = send(fd, data + sent, length - sent, flags);

        if (ret > 0)
        {
            sent += (size_t)ret;
            continue;
        }

        if (ret == 0)
        {
            return -EPIPE;
        }

        if (errno == EINTR)
        {
            continue;
        }

        return -errno;
    }

    return 0;
}

/**
 * @brief 接收LinkG响应直到对端关闭TCP连接。
 */
static int _cmain_receive_response(int fd, char **response, size_t *response_length)
{
    char chunk[LINKG_CMAIN_RECV_CHUNK];
    char *buffer;
    size_t capacity = LINKG_CMAIN_RESPONSE_INIT;
    size_t length = 0U;

    if (fd < 0 || response == NULL || response_length == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    *response_length = 0U;

    buffer = (char *)malloc(capacity + 1U);

    if (buffer == NULL)
    {
        return -ENOMEM;
    }

    while (1)
    {
        ssize_t ret;

        ret = recv(fd, chunk, sizeof(chunk), 0);

        if (ret > 0)
        {
            size_t received = (size_t)ret;
            size_t required;

            if (received > LINKG_CMAIN_RESPONSE_MAX - length)
            {
                free(buffer);
                return -EFBIG;
            }

            required = length + received;

            if (required > capacity)
            {
                size_t new_capacity = capacity;
                char *new_buffer;

                while (new_capacity < required)
                {
                    if (new_capacity >= LINKG_CMAIN_RESPONSE_MAX / 2U)
                    {
                        new_capacity = LINKG_CMAIN_RESPONSE_MAX;
                        break;
                    }

                    new_capacity *= 2U;
                }

                if (new_capacity < required || new_capacity > LINKG_CMAIN_RESPONSE_MAX)
                {
                    free(buffer);
                    return -EFBIG;
                }

                new_buffer = (char *)realloc(buffer, new_capacity + 1U);

                if (new_buffer == NULL)
                {
                    free(buffer);
                    return -ENOMEM;
                }

                buffer = new_buffer;
                capacity = new_capacity;
            }

            memcpy(buffer + length, chunk, received);
            length += received;

            continue;
        }

        if (ret == 0)
        {
            break;
        }

        if (errno == EINTR)
        {
            continue;
        }

        free(buffer);
        return -errno;
    }

    if (length == 0U)
    {
        free(buffer);
        return -ENODATA;
    }

    buffer[length] = '\0';

    *response = buffer;
    *response_length = length;

    return 0;
}

/****************************** 程序入口 ******************************/

/**
 * @brief 执行一次Web CGI请求转发。
 */
int main(void)
{
    const char *method;
    char *request = NULL;
    char *response = NULL;
    size_t request_length = 0U;
    size_t response_length = 0U;
    int backend_fd = -1;
    int ret;

    method = getenv("REQUEST_METHOD");

    if (method == NULL || strcmp(method, "POST") != 0)
    {
        _cmain_print_error(405, "Method Not Allowed", "method_not_allowed", "Only POST is supported");
        return EXIT_SUCCESS;
    }

    ret = _cmain_parse_content_length(&request_length);

    if (ret == -EFBIG)
    {
        _cmain_print_error(413, "Payload Too Large", "request_too_large", "Request body is too large");
        return EXIT_SUCCESS;
    }

    if (ret != 0)
    {
        _cmain_print_error(400, "Bad Request", "invalid_content_length", "Invalid request content length");
        return EXIT_SUCCESS;
    }

    ret = _cmain_read_request(request_length, &request);

    if (ret != 0)
    {
        _cmain_print_error(400, "Bad Request", "request_read_failed", "Failed to read complete request body");
        return EXIT_SUCCESS;
    }

    backend_fd = _cmain_connect_backend();

    if (backend_fd < 0)
    {
        free(request);

        _cmain_print_error(502, "Bad Gateway", "backend_unavailable", "LinkG service is unavailable");
        return EXIT_SUCCESS;
    }

    ret = _cmain_send_all(backend_fd, request, request_length);

    free(request);
    request = NULL;

    if (ret != 0)
    {
        close(backend_fd);

        _cmain_print_error(502, "Bad Gateway", "backend_send_failed", "Failed to send request to LinkG");
        return EXIT_SUCCESS;
    }

    if (shutdown(backend_fd, SHUT_WR) < 0)
    {
        close(backend_fd);

        _cmain_print_error(502, "Bad Gateway", "backend_shutdown_failed", "Failed to finish LinkG request");
        return EXIT_SUCCESS;
    }

    ret = _cmain_receive_response(backend_fd, &response, &response_length);

    close(backend_fd);
    backend_fd = -1;

    if (ret != 0)
    {
        if (ret == -EAGAIN || ret == -EWOULDBLOCK)
        {
            _cmain_print_error(504, "Gateway Timeout", "backend_timeout", "LinkG service response timeout");
        }
        else if (ret == -EFBIG)
        {
            _cmain_print_error(502, "Bad Gateway", "response_too_large", "LinkG service response is too large");
        }
        else
        {
            _cmain_print_error(502, "Bad Gateway", "backend_response_failed", "Failed to receive LinkG response");
        }

        return EXIT_SUCCESS;
    }

    _cmain_print_json_response(200, "OK", response, response_length);

    free(response);

    return EXIT_SUCCESS;
}
