/**
 * @file web_server.c
 * @brief LinkG Web本机控制TCP服务实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-13
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "linkg_json.h"
#include "linkg_log.h"
#include "linkg_thread.h"

/****************************** 模块常量 ******************************/

#define LINKG_WEB_SERVER_THREAD_NAME      "web-server"          // Web服务线程名称
#define LINKG_WEB_SERVER_BACKLOG          8                     // TCP监听等待队列长度
#define LINKG_WEB_REQUEST_BUFFER_INIT     (4U * 1024U)          // 请求初始缓冲区大小
#define LINKG_WEB_REQUEST_MAX             (32U * 1024U)         // 最大请求长度
#define LINKG_WEB_RECV_CHUNK_SIZE         (4U * 1024U)          // 单次接收缓冲区大小
#define LINKG_WEB_IO_TIMEOUT_SEC          3                     // 单连接收发超时时间，单位秒
#define LINKG_WEB_LOG_TAG                 "WEB-SERVER"          // 日志标签
#define LINKG_WEB_DEBUG(fmt, ...)         LINKG_LOG_DEBUG("%s: " fmt, LINKG_WEB_LOG_TAG, ##__VA_ARGS__) // 调试日志
#define LINKG_WEB_INFO(fmt, ...)          LINKG_LOG_INFO("%s: " fmt, LINKG_WEB_LOG_TAG, ##__VA_ARGS__)   // 信息日志
#define LINKG_WEB_WARN(fmt, ...)          LINKG_LOG_WARN("%s: " fmt, LINKG_WEB_LOG_TAG, ##__VA_ARGS__)   // 警告日志
#define LINKG_WEB_ERROR(fmt, ...)         LINKG_LOG_ERROR("%s: " fmt, LINKG_WEB_LOG_TAG, ##__VA_ARGS__)  // 错误日志

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_thread_t thread;      // Web服务线程
    int            listen_fd;   // 本机TCP监听Socket
    bool           initialized; // Server是否已经初始化
} linkg_web_server_context_t;

/****************************** 全局上下文 ******************************/

static linkg_web_server_context_t g_web_server =
{
    .listen_fd   = -1,    // 监听Socket尚未创建
    .initialized = false  // Server尚未初始化
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 重置Web Server运行上下文。
 */
static void _linkg_web_server_reset_context(void)
{
    memset(&g_web_server, 0, sizeof(g_web_server));

    g_web_server.listen_fd = -1;
}

/**
 * @brief 关闭Web Server监听Socket。
 */
static void _linkg_web_server_close_listen_socket(void)
{
    if (g_web_server.listen_fd >= 0)
    {
        close(g_web_server.listen_fd);
        g_web_server.listen_fd = -1;
    }
}

/**
 * @brief 设置Web客户端Socket收发超时。
 */
static int _linkg_web_server_set_client_timeout(int client_fd)
{
    struct timeval timeout;

    if (client_fd < 0)
    {
        return -EINVAL;
    }

    memset(&timeout, 0, sizeof(timeout));

    timeout.tv_sec = LINKG_WEB_IO_TIMEOUT_SEC;

    if (setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        return -errno;
    }

    if (setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) < 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 创建Web Server本机监听Socket。
 */
static int _linkg_web_server_open_listen_socket(void)
{
    struct sockaddr_in address;
    int                 reuse;
    int                 fd;

    fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0)
    {
        return -errno;
    }

    reuse = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) < 0)
    {
        int error = -errno;

        close(fd);

        return error;
    }

    memset(&address, 0, sizeof(address));

    address.sin_family      = AF_INET;
    address.sin_port        = htons((uint16_t)LINKG_RESOURCE_TCP_PORT_WEB_CONTROL);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    if (bind(fd, (struct sockaddr *)&address, sizeof(address)) < 0)
    {
        int error = -errno;

        close(fd);

        return error;
    }

    if (listen(fd, LINKG_WEB_SERVER_BACKLOG) < 0)
    {
        int error = -errno;

        close(fd);

        return error;
    }

    g_web_server.listen_fd = fd;

    return 0;
}

/**
 * @brief 完整发送指定长度的数据。
 */
static int _linkg_web_server_send_all(int fd, const void *buffer, size_t length)
{
    const uint8_t *data;
    size_t         sent;
    int            flags;

    if (fd < 0 || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    data  = (const uint8_t *)buffer;
    sent  = 0U;
    flags = 0;

#ifdef MSG_NOSIGNAL
    flags = MSG_NOSIGNAL;
#endif

    while (sent < length)
    {
        ssize_t result;

        result = send(fd, data + sent, length - sent, flags);

        if (result > 0)
        {
            sent += (size_t)result;
            continue;
        }

        if (result == 0)
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
 * @brief 扩展Web请求接收缓冲区。
 */
static int _linkg_web_server_expand_request_buffer(char **buffer, size_t *capacity, size_t required)
{
    char   *new_buffer;
    size_t  new_capacity;

    if (buffer == NULL || *buffer == NULL || capacity == NULL)
    {
        return -EINVAL;
    }

    if (required > LINKG_WEB_REQUEST_MAX)
    {
        return -EFBIG;
    }

    if (required <= *capacity)
    {
        return 0;
    }

    new_capacity = *capacity;

    while (new_capacity < required)
    {
        if (new_capacity >= LINKG_WEB_REQUEST_MAX / 2U)
        {
            new_capacity = LINKG_WEB_REQUEST_MAX;
            break;
        }

        new_capacity *= 2U;
    }

    if (new_capacity < required || new_capacity > LINKG_WEB_REQUEST_MAX)
    {
        return -EFBIG;
    }

    new_buffer = (char *)realloc(*buffer, new_capacity + 1U);
    if (new_buffer == NULL)
    {
        return -ENOMEM;
    }

    *buffer   = new_buffer;
    *capacity = new_capacity;

    return 0;
}

/**
 * @brief 接收完整Web请求直到客户端关闭发送方向。
 */
static int _linkg_web_server_receive_request(int client_fd, char **request, size_t *request_length)
{
    char    chunk[LINKG_WEB_RECV_CHUNK_SIZE];
    char   *buffer;
    size_t  capacity;
    size_t  length;

    if (client_fd < 0 || request == NULL || request_length == NULL)
    {
        return -EINVAL;
    }

    *request        = NULL;
    *request_length = 0U;

    capacity = LINKG_WEB_REQUEST_BUFFER_INIT;
    length   = 0U;

    buffer = (char *)malloc(capacity + 1U);
    if (buffer == NULL)
    {
        return -ENOMEM;
    }

    while (1)
    {
        ssize_t result;

        result = recv(client_fd, chunk, sizeof(chunk), 0);

        if (result > 0)
        {
            size_t received;
            size_t required;
            int    ret;

            received = (size_t)result;

            if (received > LINKG_WEB_REQUEST_MAX - length)
            {
                free(buffer);
                return -EFBIG;
            }

            required = length + received;

            ret = _linkg_web_server_expand_request_buffer(&buffer, &capacity, required);
            if (ret != 0)
            {
                free(buffer);
                return ret;
            }

            memcpy(buffer + length, chunk, received);
            length += received;

            continue;
        }

        if (result == 0)
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

    *request        = buffer;
    *request_length = length;

    return 0;
}

/**
 * @brief 处理单个Web客户端连接。
 */
static int _linkg_web_server_handle_client(int client_fd)
{
    char   *request;
    char   *response;
    size_t  request_length;
    size_t  response_length;
    int     ret;

    if (client_fd < 0)
    {
        return -EINVAL;
    }

    request         = NULL;
    response        = NULL;
    request_length  = 0U;
    response_length = 0U;

    ret = _linkg_web_server_set_client_timeout(client_fd);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_web_server_receive_request(client_fd, &request, &request_length);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_web_dispatch_request(request, request_length, &response, &response_length);

    free(request);
    request = NULL;

    if (ret != 0)
    {
        return ret;
    }

    if (response == NULL || response_length == 0U)
    {
        linkg_json_string_free(response);
        return -EPROTO;
    }

    ret = _linkg_web_server_send_all(client_fd, response, response_length);

    linkg_json_string_free(response);

    return ret;
}

/**
 * @brief 等待监听Socket事件或服务线程停止事件。
 */
static int _linkg_web_server_poll(linkg_thread_t *thread, bool *listen_ready)
{
    struct pollfd descriptors[2];
    int           wakeup_fd;
    int           ret;

    if (thread == NULL || listen_ready == NULL || g_web_server.listen_fd < 0)
    {
        return -EINVAL;
    }

    *listen_ready = false;

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    memset(descriptors, 0, sizeof(descriptors));

    descriptors[0].fd     = wakeup_fd;
    descriptors[0].events = POLLIN;
    descriptors[1].fd     = g_web_server.listen_fd;
    descriptors[1].events = POLLIN;

    do
    {
        ret = poll(descriptors, 2U, -1);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (ret < 0)
    {
        return -errno;
    }

    if ((descriptors[0].revents & POLLIN) != 0)
    {
        ret = linkg_thread_clear_wakeup(thread);
        if (ret != 0)
        {
            return ret;
        }
    }

    if (!linkg_thread_is_running(thread))
    {
        return 0;
    }

    if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    *listen_ready = (descriptors[1].revents & POLLIN) != 0;

    return 0;
}

/**
 * @brief Web本机控制服务线程。
 */
static void _linkg_web_server_thread(linkg_thread_t *thread, void *user_data)
{
    bool listen_ready;
    int  client_fd;
    int  ret;

    (void)user_data;

    if (thread == NULL)
    {
        return;
    }

    LINKG_WEB_DEBUG("server thread entered");

    while (linkg_thread_is_running(thread))
    {
        listen_ready = false;

        ret = _linkg_web_server_poll(thread, &listen_ready);
        if (ret != 0)
        {
            if (linkg_thread_is_running(thread))
            {
                LINKG_WEB_ERROR("poll failed, error=%d", ret);
            }

            break;
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if (!listen_ready)
        {
            continue;
        }

        client_fd = accept(g_web_server.listen_fd, NULL, NULL);
        if (client_fd < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }

            if (linkg_thread_is_running(thread))
            {
                LINKG_WEB_WARN("accept failed, error=%d", -errno);
            }

            continue;
        }

        ret = _linkg_web_server_handle_client(client_fd);

        close(client_fd);

        if (ret != 0)
        {
            LINKG_WEB_WARN("client request failed, error=%d", ret);
        }
    }

    LINKG_WEB_DEBUG("server thread exited");
}

/****************************** 服务接口 ******************************/

/**
 * @brief 初始化Web本机控制服务器。
 */
int _linkg_web_server_init(void)
{
    int ret;

    if (g_web_server.initialized)
    {
        return -EALREADY;
    }

    _linkg_web_server_reset_context();

    ret = linkg_thread_init(&g_web_server.thread, LINKG_WEB_SERVER_THREAD_NAME, _linkg_web_server_thread, NULL);
    if (ret != 0)
    {
        _linkg_web_server_reset_context();
        return ret;
    }

    g_web_server.initialized = true;

    return 0;
}

/**
 * @brief 启动Web本机控制服务器。
 */
int _linkg_web_server_start(void)
{
    int ret;

    if (!g_web_server.initialized)
    {
        return -ENODEV;
    }

    if (linkg_thread_is_started(&g_web_server.thread))
    {
        return -EALREADY;
    }

    ret = _linkg_web_server_open_listen_socket();
    if (ret != 0)
    {
        LINKG_WEB_ERROR("open listen socket failed, port=%u, error=%d", LINKG_RESOURCE_TCP_PORT_WEB_CONTROL, ret);
        return ret;
    }

    ret = linkg_thread_start(&g_web_server.thread);
    if (ret != 0)
    {
        _linkg_web_server_close_listen_socket();
        return ret;
    }

    LINKG_WEB_INFO("server started, address=127.0.0.1, port=%u", LINKG_RESOURCE_TCP_PORT_WEB_CONTROL);

    return 0;
}

/**
 * @brief 停止Web本机控制服务器。
 */
int _linkg_web_server_stop(void)
{
    int ret;

    if (!g_web_server.initialized)
    {
        return 0;
    }

    if (linkg_thread_is_started(&g_web_server.thread))
    {
        ret = linkg_thread_stop(&g_web_server.thread);
        if (ret != 0 && linkg_thread_is_started(&g_web_server.thread))
        {
            return ret;
        }

        if (ret != 0)
        {
            LINKG_WEB_WARN("server thread stopped with cleanup error, error=%d", ret);
        }
    }

    _linkg_web_server_close_listen_socket();

    LINKG_WEB_INFO("server stopped");

    return 0;
}

/**
 * @brief 反初始化Web本机控制服务器。
 */
int _linkg_web_server_deinit(void)
{
    int ret;

    if (!g_web_server.initialized)
    {
        return 0;
    }

    ret = _linkg_web_server_stop();
    if (ret != 0)
    {
        return ret;
    }

    linkg_thread_deinit(&g_web_server.thread);

    _linkg_web_server_reset_context();

    return 0;
}
