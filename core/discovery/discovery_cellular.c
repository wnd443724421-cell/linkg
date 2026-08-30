/**
 * @file discovery_cellular.c
 * @brief LinkG设备发现Cellular Channel实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#define _GNU_SOURCE

#include "discovery_cellular.h"

#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_device_config.h"
#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "discovery_channel.h"
#include "discovery_types.h"
#include "discovery_wire.h"

/****************************** 运行参数 ******************************/

#define LINKG_DISCOVERY_CELLULAR_THREAD_NAME        "discovery-cell" // Cellular Discovery线程名称
#define LINKG_DISCOVERY_CELLULAR_REPORT_INTERVAL_US 1000000ULL       // 完整状态周期发送间隔，1s
#define LINKG_DISCOVERY_CELLULAR_IPV6_TCLASS        0xC0             // Discovery控制流量IPv6 Traffic Class
#define LINKG_DISCOVERY_CELLULAR_RX_CONTROL_SIZE    CMSG_SPACE(sizeof(struct in6_pktinfo)) // IPv6接收辅助控制区大小
#define LINKG_DISCOVERY_CELLULAR_TX_CONTROL_SIZE    CMSG_SPACE(sizeof(struct in6_pktinfo)) // IPv6发送辅助控制区大小

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t     lock;               // Cellular Discovery运行状态保护锁
    linkg_thread_t      thread;             // Cellular Discovery工作线程
    uint64_t            next_report_us;     // 下一次周期完整状态发送时间
    linkg_device_role_t role;               // 本机设备角色
    int                 socket_fd;          // Cellular Discovery IPv6 UDP套接字
    int                 run_error;          // 最近一次工作线程异常退出错误
    bool                channel_registered; // Core Cellular Channel是否已经注册
    bool                initialized;        // 模块是否已经初始化
    bool                running;            // 模块是否正在运行
} linkg_discovery_cellular_context_t;

/****************************** 全局上下文 ******************************/

static linkg_discovery_cellular_context_t g_discovery_cellular =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .role      = LINKG_DEVICE_ROLE_UNKNOWN,
    .socket_fd = -1
};

/****************************** 前置声明 ******************************/

static void _linkg_discovery_cellular_thread(linkg_thread_t *thread, void *user_data);

/****************************** 上下文辅助 ******************************/

/**
 * @brief 清空Cellular Discovery动态运行状态。
 *
 * 调用方必须持有Cellular Discovery状态锁。
 */
static void _linkg_discovery_cellular_reset_runtime_locked(void)
{
    g_discovery_cellular.next_report_us     = 0U;
    g_discovery_cellular.role               = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_cellular.socket_fd          = -1;
    g_discovery_cellular.run_error          = 0;
    g_discovery_cellular.channel_registered = false;
    g_discovery_cellular.running            = false;
}

/****************************** 地址辅助 ******************************/

/**
 * @brief 校验本机Discovery状态是否具有有效节点角色。
 */
static int _linkg_discovery_cellular_validate_local_report(const linkg_discovery_report_t *report)
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
 * @brief 校验Cellular数据面Endpoint是否合法。
 */
static bool _linkg_discovery_cellular_endpoint_valid(const linkg_path_endpoint_t *endpoint)
{
    const struct sockaddr_in6 *address;

    if (endpoint == NULL)
    {
        return false;
    }

    if (endpoint->length != sizeof(struct sockaddr_in6))
    {
        return false;
    }

    if (endpoint->address.ss_family != AF_INET6)
    {
        return false;
    }

    address = (const struct sockaddr_in6 *)&endpoint->address;

    if (!linkg_network_ipv6_address_is_global(&address->sin6_addr))
    {
        return false;
    }

    return address->sin6_port != 0U;
}

/**
 * @brief 获取完整Report中声明的Cellular源IPv6地址。
 */
static int _linkg_discovery_cellular_get_report_source(const linkg_discovery_report_t *report, struct in6_addr *source)
{
    const struct sockaddr_in6 *endpoint;

    if (report == NULL || source == NULL)
    {
        return -EINVAL;
    }

    if ((report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U)
    {
        return -ENETDOWN;
    }

    if (!_linkg_discovery_cellular_endpoint_valid(&report->cellular_endpoint))
    {
        return -EINVAL;
    }

    endpoint = (const struct sockaddr_in6 *)&report->cellular_endpoint.address;

    *source = endpoint->sin6_addr;

    return 0;
}

/**
 * @brief 将Cellular数据面Endpoint转换为Discovery目标地址。
 */
static int _linkg_discovery_cellular_build_destination(const linkg_path_endpoint_t *endpoint, struct sockaddr_in6 *destination)
{
    const struct sockaddr_in6 *address;

    if (endpoint == NULL || destination == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_discovery_cellular_endpoint_valid(endpoint))
    {
        return -EINVAL;
    }

    address = (const struct sockaddr_in6 *)&endpoint->address;

    *destination = *address;
    destination->sin6_port = htons(LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY);

    return 0;
}

/**
 * @brief 校验Cellular Discovery UDP实际来源。
 */
static bool _linkg_discovery_cellular_source_valid(const struct sockaddr_in6 *source)
{
    if (source == NULL)
    {
        return false;
    }

    if (source->sin6_family != AF_INET6)
    {
        return false;
    }

    if (!linkg_network_ipv6_address_is_global(&source->sin6_addr))
    {
        return false;
    }

    return source->sin6_port == htons(LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY);
}

/**
 * @brief 校验完整Report声明的Cellular地址与UDP实际来源是否一致。
 */
static bool _linkg_discovery_cellular_report_source_valid(const linkg_discovery_report_t *report, const struct sockaddr_in6 *source)
{
    const struct sockaddr_in6 *endpoint;

    if (report == NULL || source == NULL)
    {
        return false;
    }

    if ((report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U)
    {
        return false;
    }

    if (!_linkg_discovery_cellular_endpoint_valid(&report->cellular_endpoint))
    {
        return false;
    }

    endpoint = (const struct sockaddr_in6 *)&report->cellular_endpoint.address;

    return memcmp(&endpoint->sin6_addr, &source->sin6_addr, sizeof(endpoint->sin6_addr)) == 0;
}

/****************************** 接口辅助 ******************************/

/**
 * @brief 获取当前Cellular网络接口索引。
 */
static unsigned int _linkg_discovery_cellular_interface_index(void)
{
    return if_nametoindex(LINKG_RESOURCE_INTERFACE_CELLULAR);
}

/**
 * @brief 从IPv6辅助控制信息中提取实际接收接口索引。
 */
static unsigned int _linkg_discovery_cellular_rx_ifindex(const struct msghdr *header)
{
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;

    if (header == NULL)
    {
        return 0U;
    }

    for (cmsg = CMSG_FIRSTHDR((struct msghdr *)header);
         cmsg != NULL;
         cmsg = CMSG_NXTHDR((struct msghdr *)header, cmsg))
    {
        if (cmsg->cmsg_level != IPPROTO_IPV6 ||
            cmsg->cmsg_type != IPV6_PKTINFO ||
            cmsg->cmsg_len < CMSG_LEN(sizeof(*pktinfo)))
        {
            continue;
        }

        pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);

        return pktinfo->ipi6_ifindex;
    }

    return 0U;
}

/****************************** Socket辅助 ******************************/

/**
 * @brief 创建Cellular Discovery IPv6 UDP套接字。
 *
 * Socket监听所有本机IPv6地址，发送时通过IPV6_PKTINFO固定usb0出口；
 * 接收时同样使用IPV6_PKTINFO校验报文确实来自usb0。
 */
static int _linkg_discovery_cellular_open_socket(void)
{
    struct sockaddr_in6 local;
    int                 socket_fd;
    int                 enable;
    int                 tclass;
    int                 ret;

    socket_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
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

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    tclass = LINKG_DISCOVERY_CELLULAR_IPV6_TCLASS;

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    memset(&local, 0, sizeof(local));

    local.sin6_family = AF_INET6;
    local.sin6_addr   = in6addr_any;
    local.sin6_port   = htons(LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY);

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
 * @brief 通过当前Cellular Discovery Socket发送一个IPv6 UDP报文。
 *
 * source必须与当前发送Report中声明的Cellular IPv6保持一致。
 */
static int _linkg_discovery_cellular_send_to(const struct sockaddr_in6 *destination, const struct in6_addr *source, const uint8_t *buffer, uint32_t length)
{
    unsigned char      control[LINKG_DISCOVERY_CELLULAR_TX_CONTROL_SIZE];
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;
    struct msghdr       message;
    struct iovec        iovec;
    unsigned int        interface_index;
    ssize_t             sent;
    int                 socket_fd;

    if (destination == NULL || source == NULL || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    if (!linkg_network_ipv6_address_is_global(source))
    {
        return -EINVAL;
    }

    interface_index = _linkg_discovery_cellular_interface_index();
    if (interface_index == 0U)
    {
        return -ENODEV;
    }

    memset(control, 0, sizeof(control));
    memset(&message, 0, sizeof(message));
    memset(&iovec, 0, sizeof(iovec));

    iovec.iov_base = (void *)buffer;
    iovec.iov_len  = length;

    message.msg_name       = (void *)destination;
    message.msg_namelen    = sizeof(*destination);
    message.msg_iov        = &iovec;
    message.msg_iovlen     = 1U;
    message.msg_control    = control;
    message.msg_controllen = sizeof(control);

    cmsg = CMSG_FIRSTHDR(&message);
    if (cmsg == NULL)
    {
        return -EIO;
    }

    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type  = IPV6_PKTINFO;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(*pktinfo));

    pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);

    memset(pktinfo, 0, sizeof(*pktinfo));

    pktinfo->ipi6_addr    = *source;
    pktinfo->ipi6_ifindex = interface_index;

    pthread_mutex_lock(&g_discovery_cellular.lock);

    if (!g_discovery_cellular.running || g_discovery_cellular.socket_fd < 0)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -ENETDOWN;
    }

    socket_fd = g_discovery_cellular.socket_fd;

    do
    {
        sent = sendmsg(socket_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    while (sent < 0 && errno == EINTR);

    pthread_mutex_unlock(&g_discovery_cellular.lock);

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

/**
 * @brief 将一个完整Discovery报文发送给全部已知Cellular直接Peer。
 */
static int _linkg_discovery_cellular_send_to_targets(const struct in6_addr *source, const uint8_t *buffer, uint32_t length)
{
    linkg_path_endpoint_t targets[LINKG_NODE_PEER_MAX];
    struct sockaddr_in6   destination;
    uint32_t              target_count;
    uint32_t              index;
    int                   first_error;
    int                   ret;

    if (source == NULL || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    memset(targets, 0, sizeof(targets));

    target_count = 0U;

    ret = linkg_discovery_channel_get_cellular_targets(targets, LINKG_NODE_PEER_MAX, &target_count);
    if (ret != 0)
    {
        return ret;
    }

    first_error = 0;

    for (index = 0U; index < target_count; index++)
    {
        memset(&destination, 0, sizeof(destination));

        ret = _linkg_discovery_cellular_build_destination(&targets[index], &destination);
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        ret = _linkg_discovery_cellular_send_to(&destination, source, buffer, length);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** Wire发送 ******************************/

/**
 * @brief 通过Cellular向全部当前直接STA发送AP完整状态及拓扑。
 */
static int _linkg_discovery_cellular_send_ap_sync(void)
{
    uint8_t                   buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    linkg_discovery_ap_sync_t sync;
    struct in6_addr           source;
    uint32_t                  length;
    int                       ret;

    memset(buffer, 0, sizeof(buffer));
    memset(&sync, 0, sizeof(sync));
    memset(&source, 0, sizeof(source));

    ret = linkg_discovery_channel_build_ap_sync(&sync);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_cellular_get_report_source(&sync.ap, &source);
    if (ret == -ENETDOWN)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_wire_encode_ap_sync(&sync, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_discovery_cellular_send_to_targets(&source, buffer, length);
}

/**
 * @brief 通过Cellular向当前AP发送本机STA完整状态。
 */
static int _linkg_discovery_cellular_send_sta_report(void)
{
    uint8_t                  buffer[LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE];
    linkg_discovery_report_t report;
    struct in6_addr          source;
    uint32_t                 length;
    int                      ret;

    memset(buffer, 0, sizeof(buffer));
    memset(&report, 0, sizeof(report));
    memset(&source, 0, sizeof(source));

    ret = linkg_discovery_channel_get_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_cellular_get_report_source(&report, &source);
    if (ret == -ENETDOWN)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_wire_encode_sta_report(&report, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_discovery_cellular_send_to_targets(&source, buffer, length);
}

/**
 * @brief 通过Cellular发送当前Discovery Session主动离开状态。
 *
 * 本接口由Discovery Core同步调用，工作线程必须已经停止，
 * 但Socket和Core Channel注册状态仍然保持有效。
 */
static int _linkg_discovery_cellular_send_leave(const linkg_discovery_leave_t *leave, void *user_data)
{
    linkg_discovery_cellular_context_t *context;
    uint8_t                             buffer[LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE];
    struct in6_addr                     source;
    uint32_t                            length;
    int                                 ret;

    if (leave == NULL || user_data == NULL)
    {
        return -EINVAL;
    }

    context = user_data;

    pthread_mutex_lock(&context->lock);

    if (!context->running || context->socket_fd < 0)
    {
        pthread_mutex_unlock(&context->lock);
        return -ENETDOWN;
    }

    pthread_mutex_unlock(&context->lock);

    ret = linkg_network_interface_get_global_ipv6(LINKG_RESOURCE_INTERFACE_CELLULAR, &source);
    if (ret != 0)
    {
        return ret;
    }

    memset(buffer, 0, sizeof(buffer));

    ret = linkg_discovery_wire_encode_peer_leave(leave, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_discovery_cellular_send_to_targets(&source, buffer, length);
}

/****************************** Wire接收 ******************************/

/**
 * @brief 处理AP通过Cellular收到的STA完整状态。
 */
static void _linkg_discovery_cellular_handle_sta_report(const uint8_t *buffer, uint32_t length, const struct sockaddr_in6 *source, uint64_t now_us)
{
    linkg_discovery_report_t report;
    int                      ret;

    memset(&report, 0, sizeof(report));

    ret = linkg_discovery_wire_decode_sta_report(buffer, length, &report);
    if (ret != 0)
    {
        return;
    }

    if (!_linkg_discovery_cellular_report_source_valid(&report, source))
    {
        return;
    }

    (void)linkg_discovery_channel_handle_peer_report(LINKG_LINK_ACCESS_CELLULAR, &report, now_us);
}

/**
 * @brief 处理STA通过Cellular收到的AP完整状态及拓扑同步。
 */
static void _linkg_discovery_cellular_handle_ap_sync(const uint8_t *buffer, uint32_t length, const struct sockaddr_in6 *source, uint64_t now_us)
{
    linkg_discovery_ap_sync_t sync;
    int                       ret;

    memset(&sync, 0, sizeof(sync));

    ret = linkg_discovery_wire_decode_ap_sync(buffer, length, &sync);
    if (ret != 0)
    {
        return;
    }

    if (!_linkg_discovery_cellular_report_source_valid(&sync.ap, source))
    {
        return;
    }

    (void)linkg_discovery_channel_handle_ap_sync(LINKG_LINK_ACCESS_CELLULAR, &sync, now_us);
}

/**
 * @brief 处理通过Cellular收到的Peer主动离开状态。
 */
static void _linkg_discovery_cellular_handle_peer_leave(const uint8_t *buffer, uint32_t length, uint64_t now_us)
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
 * @brief 按本机节点角色分发一个Cellular Discovery报文。
 */
static void _linkg_discovery_cellular_handle_packet(const uint8_t *buffer, uint32_t length, const struct sockaddr_in6 *source, uint64_t now_us)
{
    linkg_discovery_wire_header_t header;
    linkg_device_role_t           role;
    int                           ret;

    if (buffer == NULL || source == NULL || length < LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return;
    }

    if (!_linkg_discovery_cellular_source_valid(source))
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

    pthread_mutex_lock(&g_discovery_cellular.lock);
    role = g_discovery_cellular.role;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (header.type == LINKG_DISCOVERY_MESSAGE_PEER_LEAVE)
    {
        _linkg_discovery_cellular_handle_peer_leave(buffer, length, now_us);
        return;
    }

    if (role == LINKG_DEVICE_ROLE_AP &&
        header.type == LINKG_DISCOVERY_MESSAGE_STA_REPORT)
    {
        _linkg_discovery_cellular_handle_sta_report(buffer, length, source, now_us);
        return;
    }

    if (role == LINKG_DEVICE_ROLE_STA &&
        header.type == LINKG_DISCOVERY_MESSAGE_AP_SYNC)
    {
        _linkg_discovery_cellular_handle_ap_sync(buffer, length, source, now_us);
    }
}

/**
 * @brief 接收并处理当前Socket中已经到达的全部Cellular Discovery报文。
 */
static int _linkg_discovery_cellular_receive(void)
{
    uint8_t             buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    unsigned char       control[LINKG_DISCOVERY_CELLULAR_RX_CONTROL_SIZE];
    struct sockaddr_in6 source;
    struct iovec        iovec;
    struct msghdr       message;
    unsigned int        expected_ifindex;
    unsigned int        received_ifindex;
    ssize_t             received;
    uint64_t            now_us;
    int                 socket_fd;

    pthread_mutex_lock(&g_discovery_cellular.lock);
    socket_fd = g_discovery_cellular.socket_fd;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    expected_ifindex = _linkg_discovery_cellular_interface_index();

    for (;;)
    {
        memset(buffer, 0, sizeof(buffer));
        memset(control, 0, sizeof(control));
        memset(&source, 0, sizeof(source));
        memset(&iovec, 0, sizeof(iovec));
        memset(&message, 0, sizeof(message));

        iovec.iov_base = buffer;
        iovec.iov_len  = sizeof(buffer);

        message.msg_name       = &source;
        message.msg_namelen    = sizeof(source);
        message.msg_iov        = &iovec;
        message.msg_iovlen     = 1U;
        message.msg_control    = control;
        message.msg_controllen = sizeof(control);

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

        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0)
        {
            continue;
        }

        if (received > (ssize_t)sizeof(buffer) ||
            received < (ssize_t)LINKG_DISCOVERY_WIRE_HEADER_SIZE)
        {
            continue;
        }

        if (message.msg_namelen != sizeof(source))
        {
            continue;
        }

        received_ifindex = _linkg_discovery_cellular_rx_ifindex(&message);

        if (expected_ifindex == 0U ||
            received_ifindex == 0U ||
            received_ifindex != expected_ifindex)
        {
            continue;
        }

        now_us = linkg_time_monotonic_us();

        _linkg_discovery_cellular_handle_packet(buffer, (uint32_t)received, &source, now_us);
    }
}

/****************************** 周期任务 ******************************/

/**
 * @brief 获取Cellular Discovery下一次周期任务等待时间。
 */
static int _linkg_discovery_cellular_get_poll_timeout(uint64_t now_us)
{
    uint64_t delay_us;
    uint64_t timeout_ms;

    if (g_discovery_cellular.next_report_us <= now_us)
    {
        return 0;
    }

    delay_us   = g_discovery_cellular.next_report_us - now_us;
    timeout_ms = (delay_us + 999ULL) / 1000ULL;

    if (timeout_ms > (uint64_t)INT_MAX)
    {
        return INT_MAX;
    }

    return (int)timeout_ms;
}

/**
 * @brief 执行一次Cellular Discovery周期完整状态发送及Peer老化。
 */
static void _linkg_discovery_cellular_process_periodic(uint64_t now_us)
{
    linkg_device_role_t role;

    if (now_us < g_discovery_cellular.next_report_us)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);
    role = g_discovery_cellular.role;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (role == LINKG_DEVICE_ROLE_AP)
    {
        (void)_linkg_discovery_cellular_send_ap_sync();
    }
    else if (role == LINKG_DEVICE_ROLE_STA)
    {
        (void)_linkg_discovery_cellular_send_sta_report();
    }

    (void)linkg_discovery_channel_age_peers(now_us);

    g_discovery_cellular.next_report_us = now_us + LINKG_DISCOVERY_CELLULAR_REPORT_INTERVAL_US;
}

/****************************** 工作线程 ******************************/

/**
 * @brief 运行Cellular Discovery收发循环。
 */
static int _linkg_discovery_cellular_run(linkg_thread_t *thread)
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

    pthread_mutex_lock(&g_discovery_cellular.lock);
    socket_fd = g_discovery_cellular.socket_fd;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

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
        timeout_ms = _linkg_discovery_cellular_get_poll_timeout(now_us);

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
            ret = _linkg_discovery_cellular_receive();
            if (ret != 0)
            {
                return ret;
            }
        }

        now_us = linkg_time_monotonic_us();

        _linkg_discovery_cellular_process_periodic(now_us);
    }

    return 0;
}

/**
 * @brief Cellular Discovery线程入口。
 */
static void _linkg_discovery_cellular_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_discovery_cellular_context_t *context;
    uint64_t                            now_us;
    int                                 unregister_ret;
    int                                 ret;
    bool                                unexpected_exit;

    context = user_data;

    if (thread == NULL || context == NULL)
    {
        return;
    }

    ret = _linkg_discovery_cellular_run(thread);

    unexpected_exit = linkg_thread_is_running(thread);

    if (!unexpected_exit)
    {
        return;
    }

    now_us = linkg_time_monotonic_us();

    unregister_ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_CELLULAR, now_us);

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
 * @brief 初始化Cellular Discovery Channel。
 *
 * 本接口只初始化进程内线程资源，不要求usb0已经获得Global IPv6。
 */
int linkg_discovery_cellular_init(void)
{
    int ret;

    pthread_mutex_lock(&g_discovery_cellular.lock);

    if (g_discovery_cellular.initialized)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -EALREADY;
    }

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = linkg_thread_init(&g_discovery_cellular.thread,
                            LINKG_DISCOVERY_CELLULAR_THREAD_NAME,
                            _linkg_discovery_cellular_thread,
                            &g_discovery_cellular);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);

    _linkg_discovery_cellular_reset_runtime_locked();
    g_discovery_cellular.initialized = true;

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    return 0;
}

/**
 * @brief 启动Cellular Discovery Channel。
 *
 * Discovery Core必须已经运行并存在Cellular Link；
 * Global IPv6允许在Channel启动后异步出现。
 */
int linkg_discovery_cellular_start(void)
{
    linkg_discovery_report_t report;
    uint64_t                 now_us;
    int                      socket_fd;
    int                      cleanup_ret;
    int                      ret;

    pthread_mutex_lock(&g_discovery_cellular.lock);

    if (!g_discovery_cellular.initialized)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -ENODEV;
    }

    if (g_discovery_cellular.running)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -EALREADY;
    }

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR) == LINKG_LINK_ID_INVALID)
    {
        return -ENODEV;
    }

    memset(&report, 0, sizeof(report));

    ret = linkg_discovery_channel_get_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_cellular_validate_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    socket_fd = _linkg_discovery_cellular_open_socket();
    if (socket_fd < 0)
    {
        return socket_fd;
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_discovery_cellular.lock);

    g_discovery_cellular.role           = report.node.role;
    g_discovery_cellular.socket_fd      = socket_fd;
    g_discovery_cellular.run_error      = 0;
    g_discovery_cellular.next_report_us = now_us;
    g_discovery_cellular.running        = true;

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = linkg_discovery_channel_register(LINKG_LINK_ACCESS_CELLULAR, _linkg_discovery_cellular_send_leave, &g_discovery_cellular);
    if (ret != 0)
    {
        goto fail_socket;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);
    g_discovery_cellular.channel_registered = true;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = linkg_thread_start(&g_discovery_cellular.thread);
    if (ret != 0)
    {
        goto fail_channel;
    }

    return 0;

fail_channel:
    cleanup_ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_CELLULAR, now_us);

    pthread_mutex_lock(&g_discovery_cellular.lock);
    g_discovery_cellular.channel_registered = false;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (cleanup_ret != 0 && ret == 0)
    {
        ret = cleanup_ret;
    }

fail_socket:
    pthread_mutex_lock(&g_discovery_cellular.lock);

    g_discovery_cellular.running = false;

    if (g_discovery_cellular.socket_fd >= 0)
    {
        close(g_discovery_cellular.socket_fd);
        g_discovery_cellular.socket_fd = -1;
    }

    g_discovery_cellular.next_report_us = 0U;
    g_discovery_cellular.role           = LINKG_DEVICE_ROLE_UNKNOWN;

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    return ret;
}

/**
 * @brief 停止Cellular Discovery工作线程但保留Socket和Core注册状态。
 *
 * 用于Discovery Core停止前冻结所有周期收发，避免LEAVE发送阶段
 * local_report和Peer状态继续被异步工作线程修改。
 */
int linkg_discovery_cellular_quiesce(void)
{
    pthread_mutex_lock(&g_discovery_cellular.lock);

    if (!g_discovery_cellular.initialized)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return 0;
    }

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    return linkg_thread_stop(&g_discovery_cellular.thread);
}

/**
 * @brief 停止Cellular Discovery Channel。
 *
 * 工作线程先进入静止状态，再注销Core Access并关闭Discovery Socket。
 * 整个Discovery Session的PEER_LEAVE必须已经由Core完成发送。
 */
int linkg_discovery_cellular_stop(void)
{
    uint64_t now_us;
    int      first_error;
    int      run_error;
    int      ret;
    bool     channel_registered;

    pthread_mutex_lock(&g_discovery_cellular.lock);

    if (!g_discovery_cellular.initialized)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return 0;
    }

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = linkg_discovery_cellular_quiesce();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);
    channel_registered = g_discovery_cellular.channel_registered;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    first_error = 0;
    now_us      = linkg_time_monotonic_us();

    if (channel_registered)
    {
        ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_CELLULAR, now_us);
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);

    run_error = g_discovery_cellular.run_error;

    if (g_discovery_cellular.socket_fd >= 0)
    {
        if (close(g_discovery_cellular.socket_fd) != 0 && first_error == 0)
        {
            first_error = -errno;
        }

        g_discovery_cellular.socket_fd = -1;
    }

    g_discovery_cellular.next_report_us     = 0U;
    g_discovery_cellular.role               = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_cellular.run_error          = 0;
    g_discovery_cellular.channel_registered = false;
    g_discovery_cellular.running            = false;

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (first_error == 0 && run_error != 0)
    {
        first_error = run_error;
    }

    return first_error;
}

/**
 * @brief 反初始化Cellular Discovery Channel。
 */
int linkg_discovery_cellular_deinit(void)
{
    int ret;

    ret = linkg_discovery_cellular_stop();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);

    if (!g_discovery_cellular.initialized)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return 0;
    }

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    linkg_thread_deinit(&g_discovery_cellular.thread);

    pthread_mutex_lock(&g_discovery_cellular.lock);

    _linkg_discovery_cellular_reset_runtime_locked();
    g_discovery_cellular.initialized = false;

    pthread_mutex_unlock(&g_discovery_cellular.lock);

    return 0;
}
