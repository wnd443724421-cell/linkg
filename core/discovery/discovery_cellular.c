/**
 * @file discovery_cellular.c
 * @brief LinkG设备发现Cellular Channel实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-23
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
#include "linkg_log.h"
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
#define LINKG_DISCOVERY_CELLULAR_MONITOR_INTERVAL_MS 500             // 网络状态检查及等待兜底间隔
#define LINKG_DISCOVERY_CELLULAR_RX_BUDGET          64U              // 单次最多处理的接收报文数量
#define LINKG_DISCOVERY_CELLULAR_IPV6_TCLASS        0xC0             // Discovery控制流量IPv6 Traffic Class
#define LINKG_DISCOVERY_CELLULAR_RX_CONTROL_SIZE    CMSG_SPACE(sizeof(struct in6_pktinfo)) // IPv6接收控制区大小
#define LINKG_DISCOVERY_CELLULAR_TX_CONTROL_SIZE    CMSG_SPACE(sizeof(struct in6_pktinfo)) // IPv6发送控制区大小

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t     lock;               // Cellular Discovery运行状态保护锁
    linkg_thread_t      thread;             // Cellular Discovery工作线程
    struct in6_addr     bound_ipv6;         // 建立当前Channel时的Cellular全局IPv6
    uint64_t            next_report_us;     // 下一次周期完整状态发送时间
    linkg_device_role_t role;               // 本机设备角色
    unsigned int        bound_ifindex;      // 当前Cellular接口索引
    int                 socket_fd;          // Cellular Discovery IPv6 UDP套接字
    int                 run_error;          // 最近一次Worker异常错误
    bool                channel_registered; // Core Cellular Channel是否注册
    bool                initialized;        // 模块是否初始化
    bool                running;            // Start生命周期是否激活
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

/** @brief 清空动态运行状态。调用方持锁，且Socket已关闭。 */
static void _linkg_discovery_cellular_reset_runtime_locked(void)
{
    memset(&g_discovery_cellular.bound_ipv6, 0, sizeof(g_discovery_cellular.bound_ipv6));
    g_discovery_cellular.next_report_us     = 0U;
    g_discovery_cellular.role               = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_cellular.bound_ifindex      = 0U;
    g_discovery_cellular.socket_fd          = -1;
    g_discovery_cellular.run_error          = 0;
    g_discovery_cellular.channel_registered = false;
    g_discovery_cellular.running            = false;
}

/****************************** 地址辅助 ******************************/

/** @brief 校验本机Discovery角色；数据Endpoint允许异步出现。 */
static int _linkg_discovery_cellular_validate_local_report(const linkg_discovery_report_t *report)
{
    if (report == NULL)
    {
        return -EINVAL;
    }

    if (report->node.role != LINKG_DEVICE_ROLE_AP && report->node.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    return 0;
}

/** @brief 校验Cellular数据Endpoint。 */
static bool _linkg_discovery_cellular_endpoint_valid(const linkg_path_endpoint_t *endpoint)
{
    const struct sockaddr_in6 *address;

    if (endpoint == NULL || endpoint->length != sizeof(struct sockaddr_in6) || endpoint->address.ss_family != AF_INET6)
    {
        return false;
    }

    address = (const struct sockaddr_in6 *)&endpoint->address;
    return linkg_network_ipv6_address_is_global(&address->sin6_addr) && address->sin6_port != 0U;
}

/** @brief 获取完整Report声明的Cellular源IPv6；数据Path未就绪是正常等待。 */
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

/** @brief 将Peer的Cellular数据Endpoint转换为Discovery UDP目标。 */
static int _linkg_discovery_cellular_build_destination(const linkg_path_endpoint_t *endpoint, struct sockaddr_in6 *destination)
{
    if (endpoint == NULL || destination == NULL || !_linkg_discovery_cellular_endpoint_valid(endpoint))
    {
        return -EINVAL;
    }

    *destination = *(const struct sockaddr_in6 *)&endpoint->address;
    destination->sin6_port = htons(LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY);
    return 0;
}

/** @brief 校验Cellular Discovery UDP来源地址及端口。 */
static bool _linkg_discovery_cellular_source_valid(const struct sockaddr_in6 *source)
{
    return source != NULL && source->sin6_family == AF_INET6 &&
           linkg_network_ipv6_address_is_global(&source->sin6_addr) &&
           source->sin6_port == htons(LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY);
}

/** @brief 校验完整Report中声明的Cellular IPv6与实际UDP来源一致。 */
static bool _linkg_discovery_cellular_report_source_valid(const linkg_discovery_report_t *report, const struct sockaddr_in6 *source)
{
    const struct sockaddr_in6 *endpoint;

    if (report == NULL || source == NULL ||
        (report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U ||
        !_linkg_discovery_cellular_endpoint_valid(&report->cellular_endpoint))
    {
        return false;
    }

    endpoint = (const struct sockaddr_in6 *)&report->cellular_endpoint.address;
    return memcmp(&endpoint->sin6_addr, &source->sin6_addr, sizeof(endpoint->sin6_addr)) == 0;
}

/****************************** 接口状态 ******************************/

/** @brief 获取usb0当前索引；接口尚未出现时返回0。 */
static unsigned int _linkg_discovery_cellular_interface_index(void)
{
    return if_nametoindex(LINKG_RESOURCE_INTERFACE_CELLULAR);
}

/**
 * @brief 检查Cellular控制网络是否就绪。
 * @return 1表示usb0已UP且有全局IPv6，0表示暂不可用，负值表示查询失败。
 */
static int _linkg_discovery_cellular_read_interface(struct in6_addr *address, unsigned int *ifindex)
{
    unsigned int current_ifindex;
    bool interface_up;
    int ret;

    if (address == NULL || ifindex == NULL)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));
    *ifindex = 0U;
    current_ifindex = _linkg_discovery_cellular_interface_index();
    if (current_ifindex == 0U)
    {
        return 0;
    }

    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_CELLULAR, &interface_up);
    if (ret == -ENODEV || ret == -ENXIO)
    {
        return 0;
    }
    if (ret != 0)
    {
        return ret;
    }
    if (!interface_up)
    {
        return 0;
    }

    ret = linkg_network_interface_get_global_ipv6(LINKG_RESOURCE_INTERFACE_CELLULAR, address);
    if (ret == -ENODEV || ret == -ENXIO || ret == -EADDRNOTAVAIL)
    {
        return 0;
    }
    if (ret != 0)
    {
        return ret;
    }
    if (!linkg_network_ipv6_address_is_global(address))
    {
        return 0;
    }

    *ifindex = current_ifindex;
    return 1;
}

/** @brief 从IPv6辅助信息获取实际接收接口索引。 */
static unsigned int _linkg_discovery_cellular_rx_ifindex(const struct msghdr *header)
{
    const struct in6_pktinfo *pktinfo;
    struct cmsghdr *cmsg;

    if (header == NULL)
    {
        return 0U;
    }

    for (cmsg = CMSG_FIRSTHDR((struct msghdr *)header); cmsg != NULL; cmsg = CMSG_NXTHDR((struct msghdr *)header, cmsg))
    {
        if (cmsg->cmsg_level != IPPROTO_IPV6 || cmsg->cmsg_type != IPV6_PKTINFO || cmsg->cmsg_len < CMSG_LEN(sizeof(struct in6_pktinfo)))
        {
            continue;
        }

        pktinfo = (const struct in6_pktinfo *)CMSG_DATA(cmsg);
        return pktinfo->ipi6_ifindex;
    }

    return 0U;
}

/****************************** Socket辅助 ******************************/

/** @brief 创建IPv6 UDP Socket；收包使用IPV6_PKTINFO校验usb0，发包使用IPV6_PKTINFO选择源及出口。 */
static int _linkg_discovery_cellular_open_socket(void)
{
    struct sockaddr_in6 local;
    int socket_fd;
    int enable;
    int tclass;
    int ret;

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
    (void)close(socket_fd);
    return ret;
}

/** @brief 指定Global IPv6与usb0接口发送UDP；由context.lock保护FD生命周期。 */
static int _linkg_discovery_cellular_send_to(const struct sockaddr_in6 *destination, const struct in6_addr *source, const uint8_t *buffer, uint32_t length)
{
    unsigned char control[LINKG_DISCOVERY_CELLULAR_TX_CONTROL_SIZE];
    struct in6_pktinfo *pktinfo;
    struct cmsghdr *cmsg;
    struct msghdr message;
    struct iovec iovec;
    unsigned int interface_index;
    ssize_t sent;
    int saved_errno;

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

    do
    {
        sent = sendmsg(g_discovery_cellular.socket_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    while (sent < 0 && errno == EINTR);

    saved_errno = errno;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (sent < 0)
    {
        return -saved_errno;
    }
    return (uint32_t)sent == length ? 0 : -EIO;
}

/** @brief 将完整Discovery报文发送给已知Cellular直接Peer。 */
static int _linkg_discovery_cellular_send_to_targets(const struct in6_addr *source, const uint8_t *buffer, uint32_t length)
{
    linkg_path_endpoint_t targets[LINKG_NODE_PEER_MAX];
    struct sockaddr_in6 destination;
    uint32_t target_count;
    uint32_t index;
    int first_error;
    int ret;

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
        ret = _linkg_discovery_cellular_build_destination(&targets[index], &destination);
        if (ret == 0)
        {
            ret = _linkg_discovery_cellular_send_to(&destination, source, buffer, length);
        }
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** Wire发送 ******************************/

/** @brief AP通过Cellular向已知STA单播当前完整状态及拓扑。 */
static int _linkg_discovery_cellular_send_ap_sync(void)
{
    uint8_t buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    linkg_discovery_ap_sync_t sync;
    struct in6_addr source;
    uint32_t length;
    int ret;

    memset(&sync, 0, sizeof(sync));
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

/** @brief STA通过Cellular向已知AP单播当前完整状态。 */
static int _linkg_discovery_cellular_send_sta_report(void)
{
    uint8_t buffer[LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE];
    linkg_discovery_report_t report;
    struct in6_addr source;
    uint32_t length;
    int ret;

    memset(&report, 0, sizeof(report));
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

/** @brief Core在quiesce之后发送本机主动LEAVE；保留Socket至stop。 */
static int _linkg_discovery_cellular_send_leave(const linkg_discovery_leave_t *leave, void *user_data)
{
    linkg_discovery_cellular_context_t *context;
    uint8_t buffer[LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE];
    struct in6_addr source;
    uint32_t length;
    int ret;

    if (leave == NULL || user_data == NULL)
    {
        return -EINVAL;
    }

    context = user_data;
    pthread_mutex_lock(&context->lock);
    if (!context->running || !context->channel_registered || context->socket_fd < 0)
    {
        pthread_mutex_unlock(&context->lock);
        return -ENETDOWN;
    }
    source = context->bound_ipv6;
    pthread_mutex_unlock(&context->lock);

    /* LEAVE尽力发送；源IPv6若已消失，sendmsg会返回对应错误。 */
    ret = linkg_discovery_wire_encode_peer_leave(leave, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_discovery_cellular_send_to_targets(&source, buffer, length);
}

/****************************** Wire接收 ******************************/

/** @brief AP处理STA完整状态；只接受实际源与声明端点一致的报文。 */
static void _linkg_discovery_cellular_handle_sta_report(const uint8_t *buffer, uint32_t length, const struct sockaddr_in6 *source, uint64_t now_us)
{
    linkg_discovery_report_t report;
    int ret;

    memset(&report, 0, sizeof(report));
    ret = linkg_discovery_wire_decode_sta_report(buffer, length, &report);
    if (ret != 0 || !_linkg_discovery_cellular_report_source_valid(&report, source))
    {
        return;
    }

    ret = linkg_discovery_channel_handle_peer_report(LINKG_LINK_ACCESS_CELLULAR, &report, now_us);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-CELLULAR: STA report rejected, node=%u error=%d", (unsigned int)report.node.node_id, ret);
    }
}

/** @brief STA处理AP完整状态及拓扑。 */
static void _linkg_discovery_cellular_handle_ap_sync(const uint8_t *buffer, uint32_t length, const struct sockaddr_in6 *source, uint64_t now_us)
{
    linkg_discovery_ap_sync_t sync;
    int ret;

    memset(&sync, 0, sizeof(sync));
    ret = linkg_discovery_wire_decode_ap_sync(buffer, length, &sync);
    if (ret != 0 || !_linkg_discovery_cellular_report_source_valid(&sync.ap, source))
    {
        return;
    }

    ret = linkg_discovery_channel_handle_ap_sync(LINKG_LINK_ACCESS_CELLULAR, &sync, now_us);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-CELLULAR: AP sync rejected, node=%u error=%d", (unsigned int)sync.ap.node.node_id, ret);
    }
}

/** @brief 处理Peer主动LEAVE。 */
static void _linkg_discovery_cellular_handle_peer_leave(const uint8_t *buffer, uint32_t length, uint64_t now_us)
{
    linkg_discovery_leave_t leave;
    int ret;

    memset(&leave, 0, sizeof(leave));
    ret = linkg_discovery_wire_decode_peer_leave(buffer, length, &leave);
    if (ret == 0)
    {
        (void)linkg_discovery_channel_handle_peer_leave(&leave, now_us);
    }
}

/** @brief 按本机角色分发合法的Cellular Discovery报文。 */
static void _linkg_discovery_cellular_handle_packet(const uint8_t *buffer, uint32_t length, const struct sockaddr_in6 *source, uint64_t now_us)
{
    linkg_discovery_wire_header_t header;
    linkg_device_role_t role;
    int ret;

    if (buffer == NULL || source == NULL || length < LINKG_DISCOVERY_WIRE_HEADER_SIZE || !_linkg_discovery_cellular_source_valid(source))
    {
        return;
    }

    memset(&header, 0, sizeof(header));
    ret = linkg_discovery_wire_decode_header(buffer, length, &header);
    if (ret != 0 || header.magic != LINKG_DISCOVERY_WIRE_MAGIC || header.version != LINKG_DISCOVERY_WIRE_VERSION)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);
    role = g_discovery_cellular.role;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (header.type == LINKG_DISCOVERY_MESSAGE_PEER_LEAVE)
    {
        _linkg_discovery_cellular_handle_peer_leave(buffer, length, now_us);
    }
    else if (role == LINKG_DEVICE_ROLE_AP && header.type == LINKG_DISCOVERY_MESSAGE_STA_REPORT)
    {
        _linkg_discovery_cellular_handle_sta_report(buffer, length, source, now_us);
    }
    else if (role == LINKG_DEVICE_ROLE_STA && header.type == LINKG_DISCOVERY_MESSAGE_AP_SYNC)
    {
        _linkg_discovery_cellular_handle_ap_sync(buffer, length, source, now_us);
    }
}

/** @brief 单轮最多消费RX_BUDGET个UDP包，避免接收高负载饿死周期任务。 */
static int _linkg_discovery_cellular_receive(linkg_thread_t *thread)
{
    uint8_t buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    unsigned char control[LINKG_DISCOVERY_CELLULAR_RX_CONTROL_SIZE];
    struct sockaddr_in6 source;
    struct iovec iovec;
    struct msghdr message;
    unsigned int expected_ifindex;
    unsigned int received_ifindex;
    uint64_t now_us;
    ssize_t received;
    uint32_t index;
    int socket_fd;

    pthread_mutex_lock(&g_discovery_cellular.lock);
    socket_fd = g_discovery_cellular.socket_fd;
    expected_ifindex = g_discovery_cellular.bound_ifindex;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (socket_fd < 0 || expected_ifindex == 0U)
    {
        return -ENODEV;
    }

    for (index = 0U; index < LINKG_DISCOVERY_CELLULAR_RX_BUDGET && linkg_thread_is_running(thread); index++)
    {
        memset(&source, 0, sizeof(source));
        memset(&iovec, 0, sizeof(iovec));
        memset(&message, 0, sizeof(message));
        memset(control, 0, sizeof(control));

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
        while (received < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (!linkg_thread_is_running(thread))
        {
            return 0;
        }
        if (received < 0)
        {
            return errno == EAGAIN || errno == EWOULDBLOCK ? 0 : -errno;
        }
        if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 ||
            received > (ssize_t)sizeof(buffer) ||
            received < (ssize_t)LINKG_DISCOVERY_WIRE_HEADER_SIZE ||
            message.msg_namelen != sizeof(source))
        {
            continue;
        }

        received_ifindex = _linkg_discovery_cellular_rx_ifindex(&message);
        if (received_ifindex == 0U || received_ifindex != expected_ifindex)
        {
            continue;
        }

        now_us = linkg_time_monotonic_us();
        _linkg_discovery_cellular_handle_packet(buffer, (uint32_t)received, &source, now_us);
    }

    return 0;
}

/****************************** 周期任务 ******************************/

/** @brief 获取下次状态发送等待时间，最多500ms以便监测网络变化。 */
static int _linkg_discovery_cellular_poll_timeout(uint64_t now_us)
{
    uint64_t delay_us;
    uint64_t timeout_ms;

    if (g_discovery_cellular.next_report_us <= now_us)
    {
        return 0;
    }

    delay_us   = g_discovery_cellular.next_report_us - now_us;
    timeout_ms = (delay_us + 999ULL) / 1000ULL;
    if (timeout_ms > LINKG_DISCOVERY_CELLULAR_MONITOR_INTERVAL_MS)
    {
        return LINKG_DISCOVERY_CELLULAR_MONITOR_INTERVAL_MS;
    }
    return (int)timeout_ms;
}

/** @brief 执行一次Cellular周期状态发送及Peer老化。 */
static void _linkg_discovery_cellular_process_periodic(uint64_t now_us)
{
    linkg_device_role_t role;
    int ret;

    if (now_us < g_discovery_cellular.next_report_us)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);
    role = g_discovery_cellular.role;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = 0;
    if (role == LINKG_DEVICE_ROLE_AP)
    {
        ret = _linkg_discovery_cellular_send_ap_sync();
    }
    else if (role == LINKG_DEVICE_ROLE_STA)
    {
        ret = _linkg_discovery_cellular_send_sta_report();
    }
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-CELLULAR: periodic report failed, error=%d", ret);
    }

    ret = linkg_discovery_channel_age_peers(now_us);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-CELLULAR: peer aging failed, error=%d", ret);
    }

    g_discovery_cellular.next_report_us = now_us + LINKG_DISCOVERY_CELLULAR_REPORT_INTERVAL_US;
}

/****************************** Channel资源 ******************************/

/** @brief 由Worker建立当前Cellular Socket并向Core注册Channel。 */
static int _linkg_discovery_cellular_activate(const struct in6_addr *ipv6, unsigned int ifindex)
{
    linkg_discovery_report_t report;
    uint64_t now_us;
    int socket_fd;
    int ret;

    if (ipv6 == NULL || ifindex == 0U)
    {
        return -EINVAL;
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
        return -EPROTO;
    }

    socket_fd = _linkg_discovery_cellular_open_socket();
    if (socket_fd < 0)
    {
        return socket_fd;
    }

    now_us = linkg_time_monotonic_us();
    pthread_mutex_lock(&g_discovery_cellular.lock);
    g_discovery_cellular.socket_fd      = socket_fd;
    g_discovery_cellular.bound_ipv6     = *ipv6;
    g_discovery_cellular.bound_ifindex  = ifindex;
    g_discovery_cellular.role           = report.node.role;
    g_discovery_cellular.next_report_us = now_us;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = linkg_discovery_channel_register(LINKG_LINK_ACCESS_CELLULAR, _linkg_discovery_cellular_send_leave, &g_discovery_cellular);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_discovery_cellular.lock);
        g_discovery_cellular.socket_fd = -1;
        g_discovery_cellular.bound_ifindex = 0U;
        memset(&g_discovery_cellular.bound_ipv6, 0, sizeof(g_discovery_cellular.bound_ipv6));
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        (void)close(socket_fd);
        return ret;
    }

    pthread_mutex_lock(&g_discovery_cellular.lock);
    g_discovery_cellular.channel_registered = true;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    LINKG_LOG_INFO("DISCOVERY-CELLULAR: channel ready, interface=%s ifindex=%u", LINKG_RESOURCE_INTERFACE_CELLULAR, ifindex);
    return 0;
}

/** @brief 网络资源实际失效时撤销Core Channel并关闭Socket；正常quiesce不得调用。 */
static int _linkg_discovery_cellular_deactivate(void)
{
    uint64_t now_us;
    int first_error;
    int ret;
    bool registered;

    pthread_mutex_lock(&g_discovery_cellular.lock);
    registered = g_discovery_cellular.channel_registered;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    now_us = linkg_time_monotonic_us();
    if (registered)
    {
        ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_CELLULAR, now_us);
        if (ret != 0)
        {
            /* 注册关系未撤销成功则不要释放其Socket，也不能重复注册。 */
            return ret;
        }
    }

    first_error = 0;
    pthread_mutex_lock(&g_discovery_cellular.lock);
    g_discovery_cellular.channel_registered = false;
    if (g_discovery_cellular.socket_fd >= 0 && close(g_discovery_cellular.socket_fd) != 0)
    {
        first_error = -errno;
    }
    g_discovery_cellular.socket_fd = -1;
    g_discovery_cellular.bound_ifindex = 0U;
    g_discovery_cellular.next_report_us = 0U;
    g_discovery_cellular.role = LINKG_DEVICE_ROLE_UNKNOWN;
    memset(&g_discovery_cellular.bound_ipv6, 0, sizeof(g_discovery_cellular.bound_ipv6));
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    return first_error;
}

/****************************** 工作线程 ******************************/

/** @brief 可中断等待：Network状态变更和停止请求共用Worker wakeup FD。 */
static int _linkg_discovery_cellular_wait(linkg_thread_t *thread, int socket_fd, int timeout_ms, bool *socket_ready)
{
    struct pollfd descriptors[2];
    nfds_t count;
    int ret;

    if (thread == NULL || socket_ready == NULL)
    {
        return -EINVAL;
    }

    *socket_ready = false;
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].fd     = linkg_thread_get_wakeup_fd(thread);
    descriptors[0].events = POLLIN;
    if (descriptors[0].fd < 0)
    {
        return -ENODEV;
    }

    count = 1U;
    if (socket_fd >= 0)
    {
        descriptors[1].fd = socket_fd;
        descriptors[1].events = POLLIN;
        count = 2U;
    }

    do
    {
        ret = poll(descriptors, count, timeout_ms);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (!linkg_thread_is_running(thread))
    {
        return 0;
    }
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
    }

    if (count == 2U)
    {
        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -ENETDOWN;
        }
        *socket_ready = (descriptors[1].revents & POLLIN) != 0;
    }

    return 0;
}

/**
 * @brief Cellular运行主循环。
 *
 * Worker可先于usb0/Global IPv6启动；Channel只在网络可用时建立。
 * Cellular不知道Peer时保持空闲，不反向依赖WiFi启动结果。
 */
static int _linkg_discovery_cellular_run(linkg_thread_t *thread)
{
    struct in6_addr ipv6;
    unsigned int ifindex;
    bool socket_ready;
    bool active;
    int last_retry_error;
    int socket_fd;
    int timeout_ms;
    int ready;
    int ret;

    if (thread == NULL)
    {
        return -EINVAL;
    }

    active = false;
    last_retry_error = 0;

    while (linkg_thread_is_running(thread))
    {
        ready = _linkg_discovery_cellular_read_interface(&ipv6, &ifindex);

        if (ready < 0)
        {
            if (ready != last_retry_error)
            {
                LINKG_LOG_WARN("DISCOVERY-CELLULAR: read interface failed, error=%d", ready);
                last_retry_error = ready;
            }
        }

        if (active && (ready == 0 || (ready > 0 && (ifindex != g_discovery_cellular.bound_ifindex || memcmp(&ipv6, &g_discovery_cellular.bound_ipv6, sizeof(ipv6)) != 0))))
        {
            if (!linkg_thread_is_running(thread))
            {
                break;
            }

            LINKG_LOG_WARN("DISCOVERY-CELLULAR: IPv6/interface changed, rebuilding channel");

            ret = _linkg_discovery_cellular_deactivate();
            if (ret != 0)
            {
                return ret;
            }

            active = false;
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if (!active && ready > 0)
        {
            ret = _linkg_discovery_cellular_activate(&ipv6, ifindex);
            if (ret == 0)
            {
                active = true;
                last_retry_error = 0;
            }
            else
            {
                if (ret == -EPROTO)
                {
                    return ret;
                }
                if (ret != last_retry_error)
                {
                    LINKG_LOG_WARN("DISCOVERY-CELLULAR: activation pending, error=%d", ret);
                    last_retry_error = ret;
                }
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if (active)
        {
            _linkg_discovery_cellular_process_periodic(linkg_time_monotonic_us());
        }

        timeout_ms = active ? _linkg_discovery_cellular_poll_timeout(linkg_time_monotonic_us()) : LINKG_DISCOVERY_CELLULAR_MONITOR_INTERVAL_MS;
        socket_fd = active ? g_discovery_cellular.socket_fd : -1;
        ret = _linkg_discovery_cellular_wait(thread, socket_fd, timeout_ms, &socket_ready);
        if (!linkg_thread_is_running(thread))
        {
            break;
        }
        if (ret != 0)
        {
            if (active && ret == -ENETDOWN)
            {
                ret = _linkg_discovery_cellular_deactivate();
                if (ret != 0)
                {
                    return ret;
                }
                active = false;
                continue;
            }
            return ret;
        }

        if (active && socket_ready)
        {
            ret = _linkg_discovery_cellular_receive(thread);
            if (ret != 0)
            {
                LINKG_LOG_WARN("DISCOVERY-CELLULAR: receive failed, rebuilding channel, error=%d", ret);
                ret = _linkg_discovery_cellular_deactivate();
                if (ret != 0)
                {
                    return ret;
                }
                active = false;
            }
        }
    }

    /* 正常quiesce只停止Worker，Socket与Channel留给Core发送LEAVE。 */
    return 0;
}

/** @brief Cellular Worker入口：异常退出才清理Channel并记录错误。 */
static void _linkg_discovery_cellular_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_discovery_cellular_context_t *context;
    int cleanup_ret;
    int ret;

    context = user_data;
    if (thread == NULL || context == NULL)
    {
        return;
    }

    ret = _linkg_discovery_cellular_run(thread);
    if (!linkg_thread_is_running(thread))
    {
        return;
    }

    LINKG_LOG_ERROR("DISCOVERY-CELLULAR: worker exited unexpectedly, error=%d", ret);
    cleanup_ret = _linkg_discovery_cellular_deactivate();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("DISCOVERY-CELLULAR: unexpected-exit cleanup failed, error=%d", cleanup_ret);
    }

    pthread_mutex_lock(&context->lock);
    context->run_error = ret != 0 ? ret : (cleanup_ret != 0 ? cleanup_ret : -EIO);
    pthread_mutex_unlock(&context->lock);
}

/****************************** 生命周期 ******************************/

/** @brief 仅初始化进程内线程资源，不查询usb0或Global IPv6。 */
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

    ret = linkg_thread_init(&g_discovery_cellular.thread, LINKG_DISCOVERY_CELLULAR_THREAD_NAME, _linkg_discovery_cellular_thread, &g_discovery_cellular);
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

/** @brief 仅启动Worker；Core已经运行，usb0和IPv6允许之后异步出现。 */
int linkg_discovery_cellular_start(void)
{
    int ret;

    pthread_mutex_lock(&g_discovery_cellular.lock);
    if (!g_discovery_cellular.initialized)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -ENODEV;
    }
    if (g_discovery_cellular.running || linkg_thread_is_started(&g_discovery_cellular.thread))
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -EALREADY;
    }
    if (g_discovery_cellular.socket_fd >= 0 || g_discovery_cellular.channel_registered)
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return -EBUSY;
    }

    _linkg_discovery_cellular_reset_runtime_locked();
    g_discovery_cellular.running = true;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    ret = linkg_thread_start(&g_discovery_cellular.thread);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_discovery_cellular.lock);
        g_discovery_cellular.running = false;
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return ret;
    }

    return 0;
}

/** @brief 网络配置、接口或IPv6发生变化；不向外暴露wakeup FD。 */
int linkg_discovery_cellular_notify_network_changed(void)
{
    pthread_mutex_lock(&g_discovery_cellular.lock);
    if (!g_discovery_cellular.initialized || !g_discovery_cellular.running ||
        !linkg_thread_is_started(&g_discovery_cellular.thread) || !linkg_thread_is_running(&g_discovery_cellular.thread))
    {
        pthread_mutex_unlock(&g_discovery_cellular.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    return linkg_thread_wakeup(&g_discovery_cellular.thread);
}

/** @brief 冻结Worker，保留Socket与Core Channel供统一发送PEER_LEAVE。 */
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

/** @brief 完成Core LEAVE后注销Channel、关闭Socket、重置运行状态。 */
int linkg_discovery_cellular_stop(void)
{
    uint64_t now_us;
    int first_error;
    int run_error;
    int ret;
    bool registered;

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
    registered = g_discovery_cellular.channel_registered;
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (registered)
    {
        now_us = linkg_time_monotonic_us();
        ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_CELLULAR, now_us);
        if (ret != 0)
        {
            return ret;
        }
    }

    first_error = 0;
    pthread_mutex_lock(&g_discovery_cellular.lock);
    run_error = g_discovery_cellular.run_error;
    if (g_discovery_cellular.socket_fd >= 0 && close(g_discovery_cellular.socket_fd) != 0)
    {
        first_error = -errno;
    }
    _linkg_discovery_cellular_reset_runtime_locked();
    pthread_mutex_unlock(&g_discovery_cellular.lock);

    if (run_error != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-CELLULAR: previous Worker failure, error=%d", run_error);
    }
    return first_error;
}

/** @brief 停止并释放Cellular线程对象。 */
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
