/**
 * @file wifi_nb_report_netlink.c
 * @brief LinkG HI1105窄带状态Netlink通信实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#define _GNU_SOURCE

#include "wifi_nb_report_netlink.h"

#include <errno.h>
#include <linux/netlink.h>
#include <poll.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_wifi_config.h"

/****************************** 驱动协议 ******************************/

#define WIFI_NB_REPORT_NETLINK_PROTOCOL     26    // HI1105窄带上报Netlink协议号
#define WIFI_NB_REPORT_REQUEST_COMMAND      0U    // 获取窄带状态请求
#define WIFI_NB_REPORT_RESPONSE_COMMAND     1U    // 获取窄带状态响应
#define WIFI_NB_REPORT_REQUEST_DATA_SIZE    1U    // 请求数据区长度
#define WIFI_NB_REPORT_MAX_DATAGRAM_SIZE    2048U // Netlink最大数据报长度
#define WIFI_NB_REPORT_RESPONSE_TIMEOUT_MS  300   // 单次查询响应超时，单位毫秒

/****************************** 协议类型 ******************************/

typedef struct
{
    uint32_t command; // 窄带Netlink命令
    uint32_t length;  // 命令数据区长度
    uint8_t  data[];  // 命令数据区
} wifi_nb_message_header_t;

typedef struct
{
    uint8_t  connected;     // 驱动连接状态，本模块不使用
    uint8_t  rate_level;    // 当前窄带速率档位
    uint8_t  channel;       // 当前信道，本模块不使用
    uint8_t  bandwidth;     // 当前窄带带宽，本模块不使用
    int8_t   rssi;          // 当前RSSI，本模块不使用
    uint8_t  success_rate;  // 发帧成功率，本模块不使用
    uint16_t temperature_c; // HI1105芯片温度
} wifi_nb_report_wire_t;

/****************************** ABI约束 ******************************/

_Static_assert(sizeof(wifi_nb_message_header_t) == 8U,
               "wifi_nb_message_header_t ABI size mismatch");

_Static_assert(offsetof(wifi_nb_report_wire_t, temperature_c) == 6U,
               "wifi_nb_report_wire_t temperature offset mismatch");

_Static_assert(sizeof(wifi_nb_report_wire_t) == 8U,
               "wifi_nb_report_wire_t ABI size mismatch");

/****************************** 内部通信 ******************************/

/**
 * @brief 清除上一次查询可能残留的Netlink数据报。
 */
static int _wifi_nb_report_netlink_drain(int netlink_fd)
{
    uint8_t buffer[WIFI_NB_REPORT_MAX_DATAGRAM_SIZE];
    ssize_t received_length;

    for (;;)
    {
        received_length = recv(netlink_fd, buffer, sizeof(buffer), 0);
        if (received_length >= 0)
        {
            continue;
        }

        if (errno == EINTR)
        {
            continue;
        }

        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return 0;
        }

        return -errno;
    }
}

/**
 * @brief 发送一次窄带状态查询请求。
 */
static int _wifi_nb_report_netlink_send(int netlink_fd)
{
    _Alignas(struct nlmsghdr)
    uint8_t                   buffer[NLMSG_SPACE(sizeof(wifi_nb_message_header_t) +
                                                WIFI_NB_REPORT_REQUEST_DATA_SIZE)];
    struct nlmsghdr          *netlink_header;
    wifi_nb_message_header_t *message_header;
    struct sockaddr_nl        destination;
    struct iovec              vector;
    struct msghdr             message;
    size_t                    payload_size;
    ssize_t                   sent_length;

    memset(buffer, 0, sizeof(buffer));
    memset(&destination, 0, sizeof(destination));
    memset(&vector, 0, sizeof(vector));
    memset(&message, 0, sizeof(message));

    payload_size   = sizeof(wifi_nb_message_header_t) + WIFI_NB_REPORT_REQUEST_DATA_SIZE;
    netlink_header = (struct nlmsghdr *)buffer;

    netlink_header->nlmsg_len   = NLMSG_SPACE(payload_size);
    netlink_header->nlmsg_pid   = (uint32_t)getpid();
    netlink_header->nlmsg_flags = 0U;

    message_header = (wifi_nb_message_header_t *)NLMSG_DATA(netlink_header);

    message_header->command = WIFI_NB_REPORT_REQUEST_COMMAND;
    message_header->length  = WIFI_NB_REPORT_REQUEST_DATA_SIZE;
    message_header->data[0] = '\0';

    destination.nl_family = AF_NETLINK;
    destination.nl_pid    = 0U;
    destination.nl_groups = 0U;

    vector.iov_base = netlink_header;
    vector.iov_len  = netlink_header->nlmsg_len;

    message.msg_name    = &destination;
    message.msg_namelen = sizeof(destination);
    message.msg_iov     = &vector;
    message.msg_iovlen  = 1U;

    do
    {
        sent_length = sendmsg(netlink_fd, &message, 0);
    }
    while (sent_length < 0 && errno == EINTR);

    if (sent_length < 0)
    {
        return -errno;
    }

    if ((size_t)sent_length != vector.iov_len)
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 解析一个窄带状态响应数据报。
 */
static int _wifi_nb_report_netlink_parse(uint8_t *buffer, size_t buffer_size, wifi_nb_report_status_t *status)
{
    const wifi_nb_message_header_t *message_header;
    const wifi_nb_report_wire_t     *report;
    const struct nlmsgerr           *netlink_error;
    struct nlmsghdr                 *netlink_header;
    size_t                           payload_size;
    int                              message_length;

    if (buffer == NULL || status == NULL)
    {
        return -EINVAL;
    }

    if (buffer_size > INT32_MAX)
    {
        return -EMSGSIZE;
    }

    message_length = (int)buffer_size;

    for (netlink_header = (struct nlmsghdr *)buffer;
         NLMSG_OK(netlink_header, message_length);
         netlink_header = NLMSG_NEXT(netlink_header, message_length))
    {
        payload_size = NLMSG_PAYLOAD(netlink_header, 0);

        if (netlink_header->nlmsg_type == NLMSG_ERROR)
        {
            if (payload_size < sizeof(*netlink_error))
            {
                return -EPROTO;
            }

            netlink_error = (const struct nlmsgerr *)NLMSG_DATA(netlink_header);
            if (netlink_error->error != 0)
            {
                return netlink_error->error;
            }

            continue;
        }

        if (payload_size < sizeof(*message_header))
        {
            return -EPROTO;
        }

        message_header = (const wifi_nb_message_header_t *)NLMSG_DATA(netlink_header);
        if (message_header->command != WIFI_NB_REPORT_RESPONSE_COMMAND)
        {
            continue;
        }

        if (payload_size < sizeof(*message_header) + sizeof(*report))
        {
            return -EPROTO;
        }

        report = (const wifi_nb_report_wire_t *)message_header->data;
        if (report->rate_level > LINKG_WIFI_NARROW_RATE_MAX)
        {
            return -EPROTO;
        }

        status->rate_level         = report->rate_level;
        status->chip_temperature_c = report->temperature_c;

        return 0;
    }

    if (message_length != 0)
    {
        return -EPROTO;
    }

    return -EAGAIN;
}

/**
 * @brief 接收一次窄带状态响应。
 */
static int _wifi_nb_report_netlink_receive(int netlink_fd, wifi_nb_report_status_t *status)
{
    _Alignas(struct nlmsghdr)
    uint8_t buffer[NLMSG_SPACE(WIFI_NB_REPORT_MAX_DATAGRAM_SIZE)];
    ssize_t received_length;

    do
    {
        received_length = recv(netlink_fd, buffer, sizeof(buffer), MSG_TRUNC);
    }
    while (received_length < 0 && errno == EINTR);

    if (received_length < 0)
    {
        return -errno;
    }

    if (received_length == 0)
    {
        return -ENODATA;
    }

    if ((size_t)received_length > sizeof(buffer))
    {
        return -EMSGSIZE;
    }

    return _wifi_nb_report_netlink_parse(buffer, (size_t)received_length, status);
}

/**
 * @brief 等待一次窄带状态响应。
 */
static int _wifi_nb_report_netlink_wait(int netlink_fd)
{
    struct pollfd descriptor;
    int           ret;

    memset(&descriptor, 0, sizeof(descriptor));

    descriptor.fd     = netlink_fd;
    descriptor.events = POLLIN;

    do
    {
        ret = poll(&descriptor, 1U, WIFI_NB_REPORT_RESPONSE_TIMEOUT_MS);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 0)
    {
        return -ETIMEDOUT;
    }

    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    if ((descriptor.revents & POLLIN) == 0)
    {
        return -EIO;
    }

    return 0;
}

/****************************** Netlink接口 ******************************/

/**
 * @brief 创建并绑定HI1105窄带状态Netlink套接字。
 */
int wifi_nb_report_netlink_open(void)
{
    struct sockaddr_nl source;
    int                netlink_fd;
    int                saved_errno;

    netlink_fd = socket(AF_NETLINK,
                        SOCK_RAW | SOCK_CLOEXEC | SOCK_NONBLOCK,
                        WIFI_NB_REPORT_NETLINK_PROTOCOL);
    if (netlink_fd < 0)
    {
        return -errno;
    }

    memset(&source, 0, sizeof(source));

    source.nl_family = AF_NETLINK;
    source.nl_pid    = (uint32_t)getpid();
    source.nl_groups = 0U;

    if (bind(netlink_fd, (struct sockaddr *)&source, sizeof(source)) != 0)
    {
        saved_errno = errno;
        (void)close(netlink_fd);
        return -saved_errno;
    }

    return netlink_fd;
}

/**
 * @brief 通过已绑定套接字同步查询一次HI1105窄带状态。
 */
int wifi_nb_report_netlink_query(int netlink_fd, wifi_nb_report_status_t *status)
{
    int ret;

    if (netlink_fd < 0 || status == NULL)
    {
        return -EINVAL;
    }

    ret = _wifi_nb_report_netlink_drain(netlink_fd);
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_nb_report_netlink_send(netlink_fd);
    if (ret != 0)
    {
        return ret;
    }

    ret = _wifi_nb_report_netlink_wait(netlink_fd);
    if (ret != 0)
    {
        return ret;
    }

    return _wifi_nb_report_netlink_receive(netlink_fd, status);
}
