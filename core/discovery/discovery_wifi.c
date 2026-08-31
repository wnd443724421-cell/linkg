/**
 * @file discovery_wifi.c
 * @brief LinkG设备发现Wi-Fi Channel实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "discovery_wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_device_config.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "discovery_channel.h"
#include "discovery_types.h"
#include "discovery_wire.h"

/****************************** 运行参数 ******************************/

#define LINKG_DISCOVERY_WIFI_THREAD_NAME        "discovery-wifi" // Wi-Fi Discovery线程名称
#define LINKG_DISCOVERY_WIFI_REPORT_INTERVAL_US 1000000ULL       // 完整状态周期发送间隔，1s
#define LINKG_DISCOVERY_WIFI_IP_TOS             0xC0             // Discovery控制流量，映射WMM AC_VO

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t    lock;              // Wi-Fi Discovery运行状态保护锁
    linkg_thread_t     thread;            // Wi-Fi Discovery工作线程
    struct sockaddr_in broadcast_address; // Wi-Fi Discovery广播目标
    struct sockaddr_in ap_address;        // STA当前学习到的AP Discovery地址
    uint64_t           next_report_us;    // 下一次周期完整状态发送时间
    linkg_device_role_t role;             // 本机设备角色
    int                socket_fd;         // Wi-Fi Discovery UDP套接字
    int                run_error;         // 最近一次工作线程异常退出错误
    bool               ap_address_valid;  // STA是否已经学习到AP Discovery地址
    bool               channel_registered;// Core Wi-Fi Channel是否已经注册
    bool               initialized;       // 模块是否已经初始化
    bool               running;           // 模块是否正在运行
} linkg_discovery_wifi_context_t;

/****************************** 全局上下文 ******************************/

static linkg_discovery_wifi_context_t g_discovery_wifi =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .role      = LINKG_DEVICE_ROLE_UNKNOWN,
    .socket_fd = -1
};

/****************************** 前置声明 ******************************/

static void _linkg_discovery_wifi_thread(linkg_thread_t *thread, void *user_data);

/****************************** 上下文辅助 ******************************/

/**
 * @brief 清空Wi-Fi Discovery动态运行状态。
 *
 * 调用方必须持有Wi-Fi Discovery状态锁。
 */
static void _linkg_discovery_wifi_reset_runtime_locked(void)
{
    memset(&g_discovery_wifi.broadcast_address, 0, sizeof(g_discovery_wifi.broadcast_address));
    memset(&g_discovery_wifi.ap_address, 0, sizeof(g_discovery_wifi.ap_address));

    g_discovery_wifi.next_report_us     = 0U;
    g_discovery_wifi.role               = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_wifi.socket_fd          = -1;
    g_discovery_wifi.run_error          = 0;
    g_discovery_wifi.ap_address_valid   = false;
    g_discovery_wifi.channel_registered = false;
    g_discovery_wifi.running            = false;
}

/****************************** 地址辅助 ******************************/

/**
 * @brief 构造Wi-Fi Discovery子网广播地址。
 */
static int _linkg_discovery_wifi_build_broadcast_address(struct sockaddr_in *address)
{
    struct in_addr network;
    struct in_addr netmask;
    uint32_t       broadcast;

    if (address == NULL)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));
    memset(&network, 0, sizeof(network));
    memset(&netmask, 0, sizeof(netmask));

    if (!linkg_network_ipv4_from_string(LINKG_RESOURCE_WIFI_IPV4_NETWORK, &network))
    {
        return -EINVAL;
    }

    if (!linkg_network_ipv4_netmask_from_prefix(LINKG_RESOURCE_WIFI_IPV4_PREFIX, &netmask))
    {
        return -EINVAL;
    }

    broadcast = (ntohl(network.s_addr) & ntohl(netmask.s_addr)) | ~ntohl(netmask.s_addr);

    address->sin_family      = AF_INET;
    address->sin_addr.s_addr = htonl(broadcast);
    address->sin_port        = htons(LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY);

    return 0;
}

/**
 * @brief 校验本机Discovery状态是否具有有效节点角色。
 *
 * Wi-Fi Endpoint允许在Channel启动后异步出现。
 */
static int _linkg_discovery_wifi_validate_local_report(const linkg_discovery_report_t *report)
{
    if (report == NULL)
    {
        return -EINVAL;
    }

    if (report->node.role != LINKG_DEVICE_ROLE_AP &&
        report->node.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验Wi-Fi Discovery UDP来源地址。
 */
static bool _linkg_discovery_wifi_source_valid(const struct sockaddr_in *source)
{
    if (source == NULL)
    {
        return false;
    }

    if (source->sin_family != AF_INET)
    {
        return false;
    }

    if (!linkg_network_ipv4_address_valid(&source->sin_addr))
    {
        return false;
    }

    return source->sin_port == htons(LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY);
}

/**
 * @brief 校验Wi-Fi Discovery控制报文来源与可选Wi-Fi数据Endpoint的一致性。
 *
 * Discovery控制面不依赖Wi-Fi数据Path。未声明Wi-Fi数据Endpoint时，
 * 只要求Endpoint为空；声明了Wi-Fi数据Path时继续校验其IPv4与实际UDP来源一致。
 */
static bool _linkg_discovery_wifi_report_source_valid(const linkg_discovery_report_t *report, const struct sockaddr_in *source)
{
    const struct sockaddr_in *endpoint;

    if (report == NULL || source == NULL)
    {
        return false;
    }

    if ((report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) == 0U)
    {
        return report->wifi_endpoint.length == 0U;
    }

    if (report->wifi_endpoint.length != sizeof(struct sockaddr_in) ||
        report->wifi_endpoint.address.ss_family != AF_INET)
    {
        return false;
    }

    endpoint = (const struct sockaddr_in *)&report->wifi_endpoint.address;

    return endpoint->sin_addr.s_addr == source->sin_addr.s_addr;
}

/****************************** Socket辅助 ******************************/

/**
 * @brief 创建Wi-Fi Discovery UDP套接字。
 *
 * 套接字绑定wlan0但监听INADDR_ANY，使其同时接收本机单播和子网广播。
 */
static int _linkg_discovery_wifi_open_socket(void)
{
    struct sockaddr_in local;
    int                socket_fd;
    int                enable;
	int                tos;
    int                ret;

    socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        return -errno;
    }

    enable = 1;

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    ret = setsockopt(socket_fd,
                     SOL_SOCKET,
                     SO_BINDTODEVICE,
                     LINKG_RESOURCE_INTERFACE_WIFI,
                     strlen(LINKG_RESOURCE_INTERFACE_WIFI));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

	tos = LINKG_DISCOVERY_WIFI_IP_TOS;

    ret = setsockopt(socket_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    memset(&local, 0, sizeof(local));

    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY);

    ret = bind(socket_fd, (const struct sockaddr *)&local, sizeof(local));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    return socket_fd;

fail:
    close(socket_fd);

    return ret;
}

/**
 * @brief 通过当前Wi-Fi Discovery套接字发送一个完整UDP报文。
 */
static int _linkg_discovery_wifi_send_to(const struct sockaddr_in *destination, const uint8_t *buffer, uint32_t length)
{
    ssize_t sent;
    int     socket_fd;

    if (destination == NULL || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.running || g_discovery_wifi.socket_fd < 0)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -ENETDOWN;
    }

    socket_fd = g_discovery_wifi.socket_fd;

    do
    {
        sent = sendto(socket_fd,
                      buffer,
                      length,
                      MSG_DONTWAIT,
                      (const struct sockaddr *)destination,
                      sizeof(*destination));
    }
    while (sent < 0 && errno == EINTR);

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (sent < 0)
    {
        return -errno;
    }

    if ((uint32_t)sent != length)
    {
        return -EIO;
    }

    return 0;
}

/****************************** Wire发送 ******************************/

/**
 * @brief 通过Wi-Fi广播当前AP完整状态及拓扑。
 */
static int _linkg_discovery_wifi_send_ap_sync(void)
{
    uint8_t                   buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    linkg_discovery_ap_sync_t sync;
    struct sockaddr_in        destination;
    uint32_t                  length;
    int                       ret;

    memset(&sync, 0, sizeof(sync));
    memset(buffer, 0, sizeof(buffer));

    ret = linkg_discovery_channel_build_ap_sync(&sync);
	if (ret != 0)
	{
		return ret;
	}

	ret = linkg_discovery_wire_encode_ap_sync(&sync, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    destination = g_discovery_wifi.broadcast_address;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return _linkg_discovery_wifi_send_to(&destination, buffer, length);
}

/**
 * @brief 通过Wi-Fi向当前AP发送本机STA完整状态。
 */
static int _linkg_discovery_wifi_send_sta_report(void)
{
    uint8_t                  buffer[LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE];
    linkg_discovery_report_t report;
    struct sockaddr_in       destination;
    uint32_t                 length;
    bool                     destination_valid;
    int                      ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);

    destination_valid = g_discovery_wifi.ap_address_valid;
    destination       = g_discovery_wifi.ap_address;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (!destination_valid)
    {
        return 0;
    }

    memset(&report, 0, sizeof(report));
    memset(buffer, 0, sizeof(buffer));

    ret = linkg_discovery_channel_get_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_wire_encode_sta_report(&report, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_discovery_wifi_send_to(&destination, buffer, length);
}

/**
 * @brief 通过Wi-Fi发送当前Discovery Session主动离开状态。
 *
 * AP使用子网广播；STA向最近学习到的AP Discovery地址单播。
 */
static int _linkg_discovery_wifi_send_leave(const linkg_discovery_leave_t *leave, void *user_data)
{
    linkg_discovery_wifi_context_t *context;
    uint8_t                         buffer[LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE];
    struct sockaddr_in              destination;
    uint32_t                        length;
    bool                            destination_valid;
    int                             ret;

    if (leave == NULL || user_data == NULL)
    {
        return -EINVAL;
    }

    context = user_data;

    memset(buffer, 0, sizeof(buffer));

    ret = linkg_discovery_wire_encode_peer_leave(leave, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&context->lock);

    if (!context->running || context->socket_fd < 0)
    {
        pthread_mutex_unlock(&context->lock);
        return -ENETDOWN;
    }

    destination_valid = true;

    if (context->role == LINKG_DEVICE_ROLE_AP)
    {
        destination = context->broadcast_address;
    }
    else if (context->role == LINKG_DEVICE_ROLE_STA)
    {
        if (!context->ap_address_valid)
        {
            destination_valid = false;
        }

        destination = context->ap_address;
    }
    else
    {
        destination_valid = false;
        memset(&destination, 0, sizeof(destination));
    }

    if (!destination_valid)
    {
        pthread_mutex_unlock(&context->lock);
        return -ENOTCONN;
    }

    do
    {
        ret = (int)sendto(context->socket_fd,
                          buffer,
                          length,
                          MSG_DONTWAIT,
                          (const struct sockaddr *)&destination,
                          sizeof(destination));
    }
    while (ret < 0 && errno == EINTR);

    pthread_mutex_unlock(&context->lock);

    if (ret < 0)
    {
        return -errno;
    }

    if ((uint32_t)ret != length)
    {
        return -EIO;
    }

    return 0;
}

/****************************** Wire接收 ******************************/

/**
 * @brief 处理AP收到的STA完整状态。
 */
static void _linkg_discovery_wifi_handle_sta_report(const uint8_t *buffer, uint32_t length, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_report_t report;
    int                      ret;

    memset(&report, 0, sizeof(report));

    ret = linkg_discovery_wire_decode_sta_report(buffer, length, &report);
    if (ret != 0)
    {
        return;
    }

    if (!_linkg_discovery_wifi_report_source_valid(&report, source))
    {
        return;
    }

    (void)linkg_discovery_channel_handle_peer_report(LINKG_LINK_ACCESS_WIFI, &report, now_us);
}

/**
 * @brief 处理STA收到的AP完整状态及拓扑同步。
 */
static void _linkg_discovery_wifi_handle_ap_sync(const uint8_t *buffer, uint32_t length, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_ap_sync_t sync;
    bool                      address_changed;
    int                       ret;

    memset(&sync, 0, sizeof(sync));

    ret = linkg_discovery_wire_decode_ap_sync(buffer, length, &sync);
    if (ret != 0)
    {
        return;
    }

    if (!_linkg_discovery_wifi_report_source_valid(&sync.ap, source))
    {
        return;
    }

    ret = linkg_discovery_channel_handle_ap_sync(LINKG_LINK_ACCESS_WIFI, &sync, now_us);
    if (ret != 0)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    address_changed = !g_discovery_wifi.ap_address_valid ||
                      g_discovery_wifi.ap_address.sin_addr.s_addr != source->sin_addr.s_addr ||
                      g_discovery_wifi.ap_address.sin_port != source->sin_port;

    g_discovery_wifi.ap_address       = *source;
    g_discovery_wifi.ap_address_valid = true;

    if (address_changed &&
        g_discovery_wifi.next_report_us > now_us)
    {
        g_discovery_wifi.next_report_us = now_us;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);
}

/**
 * @brief 处理通过Wi-Fi收到的Peer主动离开状态。
 */
static void _linkg_discovery_wifi_handle_peer_leave(const uint8_t *buffer, uint32_t length, uint64_t now_us)
{
    linkg_discovery_leave_t leave;
    int                     ret;

    memset(&leave, 0, sizeof(leave));

    ret = linkg_discovery_wire_decode_peer_leave(buffer, length, &leave);
    if (ret != 0)
    {
        return;
    }

    (void)linkg_discovery_channel_handle_peer_leave(&leave, now_us);
}

/**
 * @brief 按本机角色分发一个完整Wi-Fi Discovery报文。
 */
static void _linkg_discovery_wifi_handle_packet(const uint8_t *buffer, uint32_t length, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_wire_header_t header;
    linkg_device_role_t           role;
    int                           ret;

    if (buffer == NULL || source == NULL || length < LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return;
    }

    if (!_linkg_discovery_wifi_source_valid(source))
    {
        return;
    }

    memset(&header, 0, sizeof(header));

    ret = linkg_discovery_wire_decode_header(buffer, length, &header);
    if (ret != 0)
    {
        return;
    }

    if (header.magic != LINKG_DISCOVERY_WIRE_MAGIC ||
        header.version != LINKG_DISCOVERY_WIRE_VERSION)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    role = g_discovery_wifi.role;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (header.type == LINKG_DISCOVERY_MESSAGE_PEER_LEAVE)
    {
        _linkg_discovery_wifi_handle_peer_leave(buffer, length, now_us);
        return;
    }

    if (role == LINKG_DEVICE_ROLE_AP &&
        header.type == LINKG_DISCOVERY_MESSAGE_STA_REPORT)
    {
        _linkg_discovery_wifi_handle_sta_report(buffer, length, source, now_us);
        return;
    }

    if (role == LINKG_DEVICE_ROLE_STA &&
        header.type == LINKG_DISCOVERY_MESSAGE_AP_SYNC)
    {
        _linkg_discovery_wifi_handle_ap_sync(buffer, length, source, now_us);
    }
}

/**
 * @brief 接收并处理当前Socket中已经到达的全部Wi-Fi Discovery报文。
 */
static int _linkg_discovery_wifi_receive(void)
{
    uint8_t            buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    struct sockaddr_in source;
    struct iovec       iovec;
    struct msghdr      message;
    ssize_t            received;
    uint64_t           now_us;
    int                socket_fd;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    socket_fd = g_discovery_wifi.socket_fd;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    for (;;)
    {
        memset(buffer, 0, sizeof(buffer));
        memset(&source, 0, sizeof(source));
        memset(&iovec, 0, sizeof(iovec));
        memset(&message, 0, sizeof(message));

        iovec.iov_base = buffer;
        iovec.iov_len  = sizeof(buffer);

        message.msg_name       = &source;
        message.msg_namelen    = sizeof(source);
        message.msg_iov        = &iovec;
        message.msg_iovlen     = 1U;

        do
        {
            received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_TRUNC);
        }
        while (received < 0 && errno == EINTR);

        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK)
            {
                return 0;
            }

            return -errno;
        }

        if ((message.msg_flags & MSG_TRUNC) != 0 ||
            received > (ssize_t)sizeof(buffer))
        {
            continue;
        }

        if (received < (ssize_t)LINKG_DISCOVERY_WIRE_HEADER_SIZE)
        {
            continue;
        }

        if (message.msg_namelen != sizeof(source))
        {
            continue;
        }

        now_us = linkg_time_monotonic_us();

        _linkg_discovery_wifi_handle_packet(buffer, (uint32_t)received, &source, now_us);
    }
}

/****************************** 周期任务 ******************************/

/**
 * @brief 获取Wi-Fi Discovery下一次周期任务等待时间。
 */
static int _linkg_discovery_wifi_get_poll_timeout(uint64_t now_us)
{
    uint64_t delay_us;
    uint64_t timeout_ms;

    if (g_discovery_wifi.next_report_us <= now_us)
    {
        return 0;
    }

    delay_us   = g_discovery_wifi.next_report_us - now_us;
    timeout_ms = (delay_us + 999ULL) / 1000ULL;

    if (timeout_ms > (uint64_t)INT_MAX)
    {
        return INT_MAX;
    }

    return (int)timeout_ms;
}

/**
 * @brief 执行一次Wi-Fi Discovery周期状态发送和Peer老化。
 */
static void _linkg_discovery_wifi_process_periodic(uint64_t now_us)
{
    linkg_device_role_t role;

    if (now_us < g_discovery_wifi.next_report_us)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    role = g_discovery_wifi.role;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (role == LINKG_DEVICE_ROLE_AP)
    {
        (void)_linkg_discovery_wifi_send_ap_sync();
    }
    else if (role == LINKG_DEVICE_ROLE_STA)
    {
        (void)_linkg_discovery_wifi_send_sta_report();
    }

    (void)linkg_discovery_channel_age_peers(now_us);

    g_discovery_wifi.next_report_us = now_us + LINKG_DISCOVERY_WIFI_REPORT_INTERVAL_US;
}

/****************************** 工作线程 ******************************/

/**
 * @brief 运行Wi-Fi Discovery收发循环。
 */
static int _linkg_discovery_wifi_run(linkg_thread_t *thread)
{
    struct pollfd descriptors[2];
    uint64_t      now_us;
    int           socket_fd;
    int           timeout_ms;
    int           wakeup_fd;
    int           ret;

    if (thread == NULL)
    {
        return -EINVAL;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    socket_fd = g_discovery_wifi.socket_fd;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    while (linkg_thread_is_running(thread))
    {
        memset(descriptors, 0, sizeof(descriptors));

        descriptors[0].fd     = wakeup_fd;
        descriptors[0].events = POLLIN;
        descriptors[1].fd     = socket_fd;
        descriptors[1].events = POLLIN;

        now_us     = linkg_time_monotonic_us();
        timeout_ms = _linkg_discovery_wifi_get_poll_timeout(now_us);

        do
        {
            ret = poll(descriptors, 2U, timeout_ms);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (ret < 0)
        {
            return -errno;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                return ret;
            }

            if (!linkg_thread_is_running(thread))
            {
                break;
            }
        }

        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptors[1].revents & POLLIN) != 0)
        {
            ret = _linkg_discovery_wifi_receive();
            if (ret != 0)
            {
                return ret;
            }
        }

        now_us = linkg_time_monotonic_us();

        _linkg_discovery_wifi_process_periodic(now_us);
    }

    return 0;
}

/**
 * @brief Wi-Fi Discovery线程入口。
 */
static void _linkg_discovery_wifi_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_discovery_wifi_context_t *context;
    uint64_t                        now_us;
    int                             unregister_ret;
    int                             ret;
    bool                            unexpected_exit;

    context = user_data;

    if (thread == NULL || context == NULL)
    {
        return;
    }

    ret = _linkg_discovery_wifi_run(thread);

    unexpected_exit = linkg_thread_is_running(thread);

    if (!unexpected_exit)
    {
        return;
    }

    now_us = linkg_time_monotonic_us();

    unregister_ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_WIFI, now_us);

    pthread_mutex_lock(&context->lock);

	context->channel_registered = false;
	context->running            = false;
	context->run_error          = ret != 0 ? ret : unregister_ret;

	if (context->run_error == 0)
	{
		context->run_error = -EIO;
	}

	pthread_mutex_unlock(&context->lock);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi Discovery Channel。
 *
 * 本接口只初始化进程内线程资源，不创建UDP套接字。
 */
int linkg_discovery_wifi_init(void)
{
    int ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -EALREADY;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_thread_init(&g_discovery_wifi.thread,
                            LINKG_DISCOVERY_WIFI_THREAD_NAME,
                            _linkg_discovery_wifi_thread,
                            &g_discovery_wifi);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    _linkg_discovery_wifi_reset_runtime_locked();
    g_discovery_wifi.initialized = true;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return 0;
}

/**
 * @brief 启动Wi-Fi Discovery Channel。
 *
 * Discovery Core必须已经启动，并且Wi-Fi控制接口已经存在；
 * 不要求Wi-Fi数据Path或Wi-Fi数据Endpoint有效。
 */
int linkg_discovery_wifi_start(void)
{
    linkg_discovery_report_t report;
    struct sockaddr_in       broadcast_address;
    uint64_t                 now_us;
    int                      socket_fd;
    int                      cleanup_ret;
    int                      ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -ENODEV;
    }

    if (g_discovery_wifi.running)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -EALREADY;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    memset(&report, 0, sizeof(report));
    memset(&broadcast_address, 0, sizeof(broadcast_address));

    ret = linkg_discovery_channel_get_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_wifi_validate_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_wifi_build_broadcast_address(&broadcast_address);
    if (ret != 0)
    {
        return ret;
    }

    socket_fd = _linkg_discovery_wifi_open_socket();
    if (socket_fd < 0)
    {
        return socket_fd;
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_discovery_wifi.lock);

    g_discovery_wifi.broadcast_address = broadcast_address;
    g_discovery_wifi.role              = report.node.role;
    g_discovery_wifi.socket_fd         = socket_fd;
    g_discovery_wifi.run_error         = 0;
    g_discovery_wifi.ap_address_valid  = false;
    g_discovery_wifi.next_report_us    = now_us;
    g_discovery_wifi.running           = true;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_discovery_channel_register(LINKG_LINK_ACCESS_WIFI, _linkg_discovery_wifi_send_leave, &g_discovery_wifi);
    if (ret != 0)
    {
        goto fail_socket;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    g_discovery_wifi.channel_registered = true;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_thread_start(&g_discovery_wifi.thread);
    if (ret != 0)
    {
        goto fail_channel;
    }

    return 0;

fail_channel:
    cleanup_ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_WIFI, now_us);

    pthread_mutex_lock(&g_discovery_wifi.lock);
    g_discovery_wifi.channel_registered = false;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (cleanup_ret != 0 && ret == 0)
    {
        ret = cleanup_ret;
    }

fail_socket:
    pthread_mutex_lock(&g_discovery_wifi.lock);

    g_discovery_wifi.running = false;

    if (g_discovery_wifi.socket_fd >= 0)
    {
        close(g_discovery_wifi.socket_fd);
        g_discovery_wifi.socket_fd = -1;
    }

    memset(&g_discovery_wifi.broadcast_address, 0, sizeof(g_discovery_wifi.broadcast_address));
    memset(&g_discovery_wifi.ap_address, 0, sizeof(g_discovery_wifi.ap_address));

    g_discovery_wifi.role             = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_wifi.ap_address_valid = false;
    g_discovery_wifi.next_report_us   = 0U;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return ret;
}

/**
 * @brief 停止Wi-Fi Discovery工作线程但保留Socket和Core注册状态。
 *
 * 用于Discovery Core停止前冻结所有周期收发，避免LEAVE发送阶段
 * local_report和Peer状态继续被异步工作线程修改。
 */
int linkg_discovery_wifi_quiesce(void)
{
    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return 0;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return linkg_thread_stop(&g_discovery_wifi.thread);
}

/**
 * @brief 停止Wi-Fi Discovery Channel。
 *
 * 先停止工作线程，确保不再向Core提交Wi-Fi状态，再注销Access并关闭Socket。
 * 整个Discovery Session主动LEAVE由linkg_discovery_stop统一负责发送。
 */
int linkg_discovery_wifi_stop(void)
{
    uint64_t now_us;
    int      first_error;
    int      run_error;
    int      ret;
    bool     channel_registered;

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return 0;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_discovery_wifi_quiesce();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    channel_registered = g_discovery_wifi.channel_registered;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    first_error = 0;

    now_us = linkg_time_monotonic_us();

    if (channel_registered)
    {
        ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_WIFI, now_us);
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    run_error = g_discovery_wifi.run_error;

    if (g_discovery_wifi.socket_fd >= 0)
    {
        if (close(g_discovery_wifi.socket_fd) != 0 && first_error == 0)
        {
            first_error = -errno;
        }

        g_discovery_wifi.socket_fd = -1;
    }

    memset(&g_discovery_wifi.broadcast_address, 0, sizeof(g_discovery_wifi.broadcast_address));
    memset(&g_discovery_wifi.ap_address, 0, sizeof(g_discovery_wifi.ap_address));

    g_discovery_wifi.next_report_us     = 0U;
    g_discovery_wifi.role               = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_wifi.run_error          = 0;
    g_discovery_wifi.ap_address_valid   = false;
    g_discovery_wifi.channel_registered = false;
    g_discovery_wifi.running            = false;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (first_error == 0 && run_error != 0)
    {
        first_error = run_error;
    }

    return first_error;
}

/**
 * @brief 反初始化Wi-Fi Discovery Channel。
 */
int linkg_discovery_wifi_deinit(void)
{
    int ret;

    ret = linkg_discovery_wifi_stop();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return 0;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    linkg_thread_deinit(&g_discovery_wifi.thread);

    pthread_mutex_lock(&g_discovery_wifi.lock);

    _linkg_discovery_wifi_reset_runtime_locked();
    g_discovery_wifi.initialized = false;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return 0;
}
