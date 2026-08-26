/**
 * @file wpa_ctrl_unix.c
 * @brief LinkG wpa_supplicant UNIX控制接口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#define _POSIX_C_SOURCE             200809L     // POSIX.1-2008功能特性开关

#include "wpa_ctrl.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/****************************** 控制参数 ******************************/

#define WPA_CTRL_REQUEST_TIMEOUT_MS 1000U       // 单次请求总截止时间
#define WPA_CTRL_NSEC_PER_SEC       1000000000L // 每秒纳秒数
#define WPA_CTRL_NSEC_PER_MSEC      1000000L    // 每毫秒纳秒数
#define WPA_CTRL_LOCAL_DIR          "/tmp"      // 本地控制套接字目录

/****************************** 内部类型 ******************************/

struct wpa_ctrl
{
    int                socket_fd;        // UNIX数据报套接字
    bool               request_poisoned; // 请求响应序列已经失步
    struct sockaddr_un local_addr;       // 本地套接字地址
    struct sockaddr_un remote_addr;      // 服务端套接字地址
};

/****************************** 全局状态 ******************************/

static atomic_uint g_wpa_ctrl_sequence = ATOMIC_VAR_INIT(0U); // 本地套接字序列号

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查UNIX套接字路径是否有效。
 */
static bool _wpa_ctrl_path_valid(const char *path)
{
    if (path == NULL)
    {
        return false;
    }

    if (path[0] == '\0')
    {
        return false;
    }

    return strlen(path) < sizeof(((struct sockaddr_un *)0)->sun_path);
}

/**
 * @brief 计算一次控制请求的单调时钟绝对截止时间。
 */
static int _wpa_ctrl_make_deadline(struct timespec *deadline)
{
    if (deadline == NULL)
    {
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, deadline) != 0)
    {
        return -1;
    }

    deadline->tv_sec += (time_t)(WPA_CTRL_REQUEST_TIMEOUT_MS / 1000U);
    deadline->tv_nsec += (long)(WPA_CTRL_REQUEST_TIMEOUT_MS % 1000U) * WPA_CTRL_NSEC_PER_MSEC;

    if (deadline->tv_nsec >= WPA_CTRL_NSEC_PER_SEC)
    {
        deadline->tv_sec++;
        deadline->tv_nsec -= WPA_CTRL_NSEC_PER_SEC;
    }

    return 0;
}

/**
 * @brief 将绝对截止时间换算成select本轮剩余时间。
 *
 * @note 返回1表示仍有剩余时间，0表示已经超时，-1表示时钟读取失败。
 */
static int _wpa_ctrl_remaining_timeout(const struct timespec *deadline, struct timeval *timeout)
{
    struct timespec now;
    time_t          seconds;
    long            nanoseconds;

    if (deadline == NULL || timeout == NULL)
    {
        return -1;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return -1;
    }

    seconds     = deadline->tv_sec - now.tv_sec;
    nanoseconds = deadline->tv_nsec - now.tv_nsec;

    if (nanoseconds < 0L)
    {
        seconds--;
        nanoseconds += WPA_CTRL_NSEC_PER_SEC;
    }

    if (seconds < 0 || (seconds == 0 && nanoseconds <= 0L))
    {
        timeout->tv_sec  = 0L;
        timeout->tv_usec = 0L;
        return 0;
    }

    timeout->tv_sec  = seconds;
    timeout->tv_usec = nanoseconds / 1000L;
    if (timeout->tv_usec == 0L && nanoseconds > 0L)
    {
        timeout->tv_usec = 1L;
    }

    return 1;
}

/**
 * @brief 在同一个绝对截止时间内发送控制命令。
 *
 * @note 返回0表示发送成功，-1表示发送失败，-2表示截止时间已到。
 */
static int _wpa_ctrl_send_request(struct wpa_ctrl *ctrl, const char *command, size_t command_length, const struct timespec *deadline)
{
    fd_set         write_set;
    struct timeval timeout;
    ssize_t        sent;
    int            ret;
    int            timeout_state;

    for (;;)
    {
        timeout_state = _wpa_ctrl_remaining_timeout(deadline, &timeout);
        if (timeout_state < 0)
        {
            return -1;
        }

        if (timeout_state == 0)
        {
            return -2;
        }

        sent = send(ctrl->socket_fd, command, command_length, MSG_DONTWAIT);
        if (sent == (ssize_t)command_length)
        {
            return 0;
        }

        if (sent >= 0)
        {
            return -1;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != ENOBUFS)
        {
            return -1;
        }

        FD_ZERO(&write_set);
        FD_SET(ctrl->socket_fd, &write_set);

        ret = select(ctrl->socket_fd + 1, NULL, &write_set, NULL, &timeout);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return -1;
        }
    }
}

/**
 * @brief 标记本连接的请求响应序列已不再可信。
 *
 * @note 控制协议没有请求ID。命令发送后如果失败或超时，迟到的响应可能被下一条
 *       命令误收，因此调用方必须关闭并重新打开控制连接。
 */
static int _wpa_ctrl_fail_request(struct wpa_ctrl *ctrl, int error)
{
    if (ctrl != NULL)
    {
        ctrl->request_poisoned = true;
    }

    return error;
}

/**
 * @brief 创建wpa_supplicant UNIX控制连接。
 */
static struct wpa_ctrl *_wpa_ctrl_open_unix(const char *ctrl_path, const char *client_dir)
{
    struct wpa_ctrl *ctrl;
    unsigned int     sequence;
    int              length;

    if (!_wpa_ctrl_path_valid(ctrl_path))
    {
        return NULL;
    }

    if (!_wpa_ctrl_path_valid(client_dir))
    {
        return NULL;
    }

    ctrl = calloc(1U, sizeof(*ctrl));
    if (ctrl == NULL)
    {
        return NULL;
    }

    ctrl->socket_fd = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ctrl->socket_fd < 0)
    {
        free(ctrl);
        return NULL;
    }

    if (ctrl->socket_fd >= FD_SETSIZE)
    {
        close(ctrl->socket_fd);
        free(ctrl);
        return NULL;
    }

    ctrl->local_addr.sun_family = AF_UNIX;
    sequence = atomic_fetch_add_explicit(&g_wpa_ctrl_sequence, 1U, memory_order_relaxed);
    length = snprintf(ctrl->local_addr.sun_path,
                      sizeof(ctrl->local_addr.sun_path),
                      "%s/linkg_wpa_ctrl_%ld_%u",
                      client_dir,
                      (long)getpid(),
                      sequence);
    if (length < 0 || (size_t)length >= sizeof(ctrl->local_addr.sun_path))
    {
        wpa_ctrl_close(ctrl);
        return NULL;
    }

    if (bind(ctrl->socket_fd, (const struct sockaddr *)&ctrl->local_addr, sizeof(ctrl->local_addr)) != 0)
    {
        wpa_ctrl_close(ctrl);
        return NULL;
    }

    ctrl->remote_addr.sun_family = AF_UNIX;
    memcpy(ctrl->remote_addr.sun_path, ctrl_path, strlen(ctrl_path) + 1U);

    if (connect(ctrl->socket_fd, (const struct sockaddr *)&ctrl->remote_addr, sizeof(ctrl->remote_addr)) != 0)
    {
        wpa_ctrl_close(ctrl);
        return NULL;
    }

    return ctrl;
}

/**
 * @brief 发送无需异步消息回调的控制命令。
 */
static int _wpa_ctrl_command(struct wpa_ctrl *ctrl, const char *command)
{
    char   reply[16];
    size_t reply_length;
    int    ret;

    reply_length = sizeof(reply);
    ret = wpa_ctrl_request(ctrl, command, strlen(command), reply, &reply_length, NULL);
    if (ret != 0)
    {
        return ret;
    }

    if (reply_length < 2U)
    {
        return -1;
    }

    return memcmp(reply, "OK", 2U) == 0 ? 0 : -1;
}

/****************************** 控制接口 ******************************/

/**
 * @brief 打开wpa_supplicant或hostapd控制接口。
 */
struct wpa_ctrl *wpa_ctrl_open(const char *ctrl_path)
{
    return _wpa_ctrl_open_unix(ctrl_path, WPA_CTRL_LOCAL_DIR);
}

/**
 * @brief 使用指定客户端目录打开控制接口。
 */
struct wpa_ctrl *wpa_ctrl_open2(const char *ctrl_path, const char *cli_path)
{
    return _wpa_ctrl_open_unix(ctrl_path, cli_path);
}

/**
 * @brief 关闭控制接口并删除本地套接字。
 */
void wpa_ctrl_close(struct wpa_ctrl *ctrl)
{
    if (ctrl == NULL)
    {
        return;
    }

    if (ctrl->socket_fd >= 0)
    {
        close(ctrl->socket_fd);
    }

    if (ctrl->local_addr.sun_path[0] != '\0')
    {
        unlink(ctrl->local_addr.sun_path);
    }

    free(ctrl);
}

/**
 * @brief 向wpa_supplicant或hostapd发送控制命令。
 *
 * @note 请求发送失败或等待响应超时后连接会被标记为不可继续使用，
 *       调用方应关闭并重新打开控制连接。
 */
int wpa_ctrl_request(struct wpa_ctrl *ctrl, const char *cmd, size_t cmd_len, char *reply, size_t *reply_len, void (*msg_cb)(char *msg, size_t len))
{
    fd_set        read_set;
    struct timeval timeout;
    struct timespec deadline;
    ssize_t        received;
    int            ret;
    int            timeout_state;

    if (ctrl == NULL || ctrl->request_poisoned)
    {
        return -1;
    }

    if (cmd == NULL)
    {
        return -1;
    }

    if (reply == NULL)
    {
        return -1;
    }

    if (reply_len == NULL)
    {
        return -1;
    }

    if (*reply_len == 0U)
    {
        return -1;
    }

    if (_wpa_ctrl_make_deadline(&deadline) != 0)
    {
        return -1;
    }

    ret = _wpa_ctrl_send_request(ctrl, cmd, cmd_len, &deadline);
    if (ret != 0)
    {
        return _wpa_ctrl_fail_request(ctrl, ret);
    }

    for (;;)
    {
        FD_ZERO(&read_set);
        FD_SET(ctrl->socket_fd, &read_set);

        timeout_state = _wpa_ctrl_remaining_timeout(&deadline, &timeout);
        if (timeout_state < 0)
        {
            return _wpa_ctrl_fail_request(ctrl, -1);
        }

        if (timeout_state == 0)
        {
            return _wpa_ctrl_fail_request(ctrl, -2);
        }

        ret = select(ctrl->socket_fd + 1, &read_set, NULL, NULL, &timeout);
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return _wpa_ctrl_fail_request(ctrl, -1);
        }

        if (ret == 0)
        {
            continue;
        }

        received = recv(ctrl->socket_fd, reply, *reply_len, MSG_DONTWAIT);
        if (received < 0)
        {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
            {
                continue;
            }

            return _wpa_ctrl_fail_request(ctrl, -1);
        }

        if (received > 0 && reply[0] == '<')
        {
            if (msg_cb != NULL)
            {
                msg_cb(reply, (size_t)received);
            }
            continue;
        }

        *reply_len = (size_t)received;
        return 0;
    }
}

/**
 * @brief 将控制接口注册为事件监听器。
 */
int wpa_ctrl_attach(struct wpa_ctrl *ctrl)
{
    return _wpa_ctrl_command(ctrl, "ATTACH");
}

/**
 * @brief 取消控制接口事件监听。
 */
int wpa_ctrl_detach(struct wpa_ctrl *ctrl)
{
    return _wpa_ctrl_command(ctrl, "DETACH");
}

/**
 * @brief 接收待处理的控制接口事件。
 */
int wpa_ctrl_recv(struct wpa_ctrl *ctrl, char *reply, size_t *reply_len)
{
    ssize_t received;

    if (ctrl == NULL)
    {
        return -1;
    }

    if (ctrl->request_poisoned || reply == NULL)
    {
        return -1;
    }

    if (reply_len == NULL)
    {
        return -1;
    }

    if (*reply_len == 0U)
    {
        return -1;
    }

    received = recv(ctrl->socket_fd, reply, *reply_len, MSG_DONTWAIT);
    if (received < 0)
    {
        return -1;
    }

    *reply_len = (size_t)received;
    return 0;
}

/**
 * @brief 检查控制接口是否存在待接收事件。
 */
int wpa_ctrl_pending(struct wpa_ctrl *ctrl)
{
    fd_set        read_set;
    struct timeval timeout;

    if (ctrl == NULL || ctrl->request_poisoned)
    {
        return -1;
    }

    FD_ZERO(&read_set);
    FD_SET(ctrl->socket_fd, &read_set);
    timeout.tv_sec  = 0L;
    timeout.tv_usec = 0L;

    return select(ctrl->socket_fd + 1, &read_set, NULL, NULL, &timeout);
}

/**
 * @brief 获取控制接口使用的文件描述符。
 */
int wpa_ctrl_get_fd(struct wpa_ctrl *ctrl)
{
    if (ctrl == NULL || ctrl->request_poisoned)
    {
        return -1;
    }

    return ctrl->socket_fd;
}
