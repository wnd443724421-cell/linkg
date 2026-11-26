/**
 * @file at_server.c
 * @brief LinkG AT UDP服务实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-26
 */

#define _GNU_SOURCE

#include "at_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"

/****************************** 模块常量 ******************************/

#define LINKG_AT_SERVER_THREAD_NAME "at-server" // AT服务线程名称

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_thread_t thread;      // AT服务线程
    int            socket_fd;   // UDP监听Socket，线程退出后释放
    bool           initialized; // Server是否已经初始化
} linkg_at_server_context_t;

typedef union
{
    struct cmsghdr alignment;                                  // 控制消息对齐
    unsigned char  bytes[CMSG_SPACE(sizeof(struct in_pktinfo))]; // IPv4控制消息空间
} linkg_at_control_buffer_t;

/****************************** 全局上下文 ******************************/

static linkg_at_server_context_t g_at_server =
{
    .socket_fd   = -1,    // UDP Socket尚未创建
    .initialized = false  // Server尚未初始化
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 重置已经释放资源的Server上下文。
 */
static void _linkg_at_server_reset_context(void)
{
    memset(&g_at_server, 0, sizeof(g_at_server));
    g_at_server.socket_fd = -1;
}

/**
 * @brief 关闭UDP Socket，调用前必须确认服务线程已经停止。
 */
static void _linkg_at_server_close_socket(void)
{
    if (g_at_server.socket_fd >= 0)
    {
        close(g_at_server.socket_fd);
        g_at_server.socket_fd = -1;
    }
}

/**
 * @brief 创建非阻塞IPv4 UDP Socket并获取入口接口信息。
 */
static int _linkg_at_server_open_socket(void)
{
    struct sockaddr_in address = {0};
    int                enable = 1;
    int                fd;
    int                ret;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -errno;
    }

    ret = setsockopt(fd, IPPROTO_IP, IP_PKTINFO, &enable, sizeof(enable));
    if (ret < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    // 不启用SO_REUSEADDR/SO_REUSEPORT，端口冲突必须显式失败。
    address.sin_family      = AF_INET;
    address.sin_port        = htons(LINKG_RESOURCE_UDP_PORT_AT);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    ret = bind(fd, (const struct sockaddr *)&address, sizeof(address));
    if (ret < 0)
    {
        ret = -errno;
        close(fd);
        return ret;
    }

    g_at_server.socket_fd = fd;

    return 0;
}

/**
 * @brief 判断是否为允许访问AT服务的入口接口。
 *
 * 不依赖本机动态IPv4地址，拒绝来自无线底层和蜂窝公网接口的请求。
 * 接口检查仅用于限制服务范围，不能替代敏感命令的授权验证。
 */
static bool _linkg_at_server_interface_allowed(unsigned int interface_index)
{
    char name[IF_NAMESIZE];

    if (interface_index == 0U || if_indextoname(interface_index, name) == NULL)
    {
        return false;
    }

    return strcmp(name, LINKG_RESOURCE_INTERFACE_ETHERNET) == 0 ||
           strcmp(name, LINKG_RESOURCE_INTERFACE_TUN) == 0 ||
           strcmp(name, LINKG_RESOURCE_INTERFACE_LOOPBACK) == 0;
}

/**
 * @brief 拒绝未指定、组播及有限广播地址。
 */
static bool _linkg_at_server_address_allowed(struct in_addr address)
{
    uint32_t value;

    value = ntohl(address.s_addr);

    return value != INADDR_ANY && value != INADDR_BROADCAST && !IN_MULTICAST(value);
}

/**
 * @brief 从控制消息读取本机单播目的地址和入口接口。
 */
static bool _linkg_at_server_get_packet_info(struct msghdr *message, linkg_at_request_context_t *context)
{
    struct cmsghdr   *header;
    struct in_pktinfo info;

    for (header = CMSG_FIRSTHDR(message); header != NULL; header = CMSG_NXTHDR(message, header))
    {
        if (header->cmsg_level != IPPROTO_IP || header->cmsg_type != IP_PKTINFO)
        {
            continue;
        }

        if (header->cmsg_len < CMSG_LEN(sizeof(info)))
        {
            return false;
        }

        memcpy(&info, CMSG_DATA(header), sizeof(info));

        // 广播或组播报文的目的地址与本地单播地址不同，不予应答。
        if (info.ipi_addr.s_addr != info.ipi_spec_dst.s_addr ||
            !_linkg_at_server_address_allowed(info.ipi_addr))
        {
            return false;
        }

        if (info.ipi_ifindex <= 0 || !_linkg_at_server_interface_allowed((unsigned int)info.ipi_ifindex))
        {
            return false;
        }

        context->interface_index = (unsigned int)info.ipi_ifindex;
        context->local_address   = info.ipi_addr;

        return true;
    }

    return false;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 使用请求命中的本机IPv4地址发送响应，出口由正常路由选择。
 */
static void _linkg_at_server_send_response(int socket_fd, const linkg_at_request_context_t *context, char *response, size_t response_length)
{
    struct sockaddr_in         peer = context->peer;
    struct iovec               iov = {0};
    struct msghdr              message = {0};
    linkg_at_control_buffer_t  control = {0};
    struct cmsghdr            *header;
    struct in_pktinfo          info = {0};

    iov.iov_base = response;
    iov.iov_len  = response_length;

    message.msg_name       = &peer;
    message.msg_namelen    = sizeof(peer);
    message.msg_iov        = &iov;
    message.msg_iovlen     = 1U;
    message.msg_control    = control.bytes;
    message.msg_controllen = sizeof(control.bytes);

    header             = CMSG_FIRSTHDR(&message);
    header->cmsg_level = IPPROTO_IP;
    header->cmsg_type  = IP_PKTINFO;
    header->cmsg_len   = CMSG_LEN(sizeof(info));

    info.ipi_spec_dst = context->local_address;
    memcpy(CMSG_DATA(header), &info, sizeof(info));

    // UDP发送失败不阻塞业务线程；客户端通过超时与重试处理丢包。
    (void)sendmsg(socket_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 接收并处理一条完整UDP请求，拒绝截断数据报。
 */
static void _linkg_at_server_receive(int socket_fd)
{
    linkg_at_request_context_t context = {0};
    linkg_at_control_buffer_t  control = {0};
    struct iovec               iov = {0};
    struct msghdr              message = {0};
    char                       request[LINKG_AT_REQUEST_BUFFER_SIZE];
    char                       response[LINKG_AT_RESPONSE_BUFFER_SIZE];
    size_t                     response_length;
    ssize_t                    length;
    int                        ret;

    iov.iov_base = request;
    iov.iov_len  = sizeof(request) - 1U;

    message.msg_name       = &context.peer;
    message.msg_namelen    = sizeof(context.peer);
    message.msg_iov        = &iov;
    message.msg_iovlen     = 1U;
    message.msg_control    = control.bytes;
    message.msg_controllen = sizeof(control.bytes);

    length = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_TRUNC);
    if (length <= 0)
    {
        return;
    }

    if ((message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) != 0 || (size_t)length >= sizeof(request))
    {
        return;
    }

    if (message.msg_namelen != sizeof(context.peer) || context.peer.sin_family != AF_INET ||
        context.peer.sin_port == 0U || !_linkg_at_server_address_allowed(context.peer.sin_addr))
    {
        return;
    }

    if (!_linkg_at_server_get_packet_info(&message, &context))
    {
        return;
    }

    ret = _linkg_at_dispatch_request(&context, request, (size_t)length, response, sizeof(response), &response_length);
    if (ret == 0 && response_length > 0U)
    {
        _linkg_at_server_send_response(socket_fd, &context, response, response_length);
    }
}

/****************************** 服务线程 ******************************/

/**
 * @brief 等待UDP请求或停止信号，优先处理线程唤醒。
 */
static void _linkg_at_server_thread(linkg_thread_t *thread, void *user_data)
{
    struct pollfd descriptors[2] = {0};
    socklen_t     error_length;
    int           socket_error;
    int           ret;

    (void)user_data;

    descriptors[0].fd     = linkg_thread_get_wakeup_fd(thread);
    descriptors[0].events = POLLIN;
    descriptors[1].fd     = g_at_server.socket_fd;
    descriptors[1].events = POLLIN;

    if (descriptors[0].fd < 0 || descriptors[1].fd < 0)
    {
        LINKG_LOG_ERROR("AT: invalid server descriptors");
        return;
    }

    while (linkg_thread_is_running(thread))
    {
        _linkg_at_ssh_tick();

        ret = poll(descriptors, 2U, _linkg_at_ssh_poll_timeout());
        if (ret < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            LINKG_LOG_ERROR("AT: server poll failed, error=%d", -errno);
            break;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_ERROR("AT: invalid wakeup descriptor events");
            break;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                LINKG_LOG_ERROR("AT: clear wakeup failed, error=%d", ret);
                break;
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        _linkg_at_ssh_tick();

        if ((descriptors[1].revents & (POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_ERROR("AT: invalid UDP socket events");
            break;
        }

        if ((descriptors[1].revents & POLLERR) != 0)
        {
            // 清除UDP异步错误，避免错误事件导致poll空转。
            socket_error = 0;
            error_length = sizeof(socket_error);
            ret = getsockopt(descriptors[1].fd, SOL_SOCKET, SO_ERROR, &socket_error, &error_length);
            if (ret != 0)
            {
                LINKG_LOG_ERROR("AT: read UDP socket error failed, error=%d", -errno);
                break;
            }
        }

        if ((descriptors[1].revents & POLLIN) != 0)
        {
            // 每次只处理一条数据报，持续收包时也能及时响应停止请求。
            _linkg_at_server_receive(descriptors[1].fd);
        }
    }

    // 线程异常退出时也尝试回收本次SSH维护服务。
    _linkg_at_auth_reset();
    (void)_linkg_at_ssh_shutdown();
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化AT Server线程对象，不绑定端口。
 */
int _linkg_at_server_init(void)
{
    int ret;

    if (g_at_server.initialized)
    {
        return -EALREADY;
    }

    _linkg_at_server_reset_context();

    ret = linkg_thread_init(&g_at_server.thread, LINKG_AT_SERVER_THREAD_NAME, _linkg_at_server_thread, NULL);
    if (ret != 0)
    {
        _linkg_at_server_reset_context();
        return ret;
    }

    g_at_server.initialized = true;

    return 0;
}

/**
 * @brief 绑定UDP端口并启动服务线程。
 */
int _linkg_at_server_start(void)
{
    int ret;

    if (!g_at_server.initialized)
    {
        return -ENODEV;
    }

    if (linkg_thread_is_started(&g_at_server.thread))
    {
        return -EALREADY;
    }

    ret = _linkg_at_ssh_shutdown();
    if (ret != 0)
    {
        return ret;
    }

    _linkg_at_auth_reset();

    ret = _linkg_at_server_open_socket();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("AT: bind UDP port failed, port=%u, error=%d", LINKG_RESOURCE_UDP_PORT_AT, ret);
        return ret;
    }

    _linkg_at_ssh_prepare();

    ret = linkg_thread_start(&g_at_server.thread);
    if (ret != 0)
    {
        _linkg_at_server_close_socket();
        return ret;
    }

    LINKG_LOG_INFO("AT: server listening, port=%u", LINKG_RESOURCE_UDP_PORT_AT);

    return 0;
}

/**
 * @brief 停止并回收线程后释放UDP Socket。
 */
int _linkg_at_server_stop(void)
{
    int ret;

    if (!g_at_server.initialized)
    {
        return 0;
    }

    if (linkg_thread_is_started(&g_at_server.thread))
    {
        ret = linkg_thread_stop(&g_at_server.thread);
        if (ret != 0)
        {
            return ret;
        }
    }

    _linkg_at_server_close_socket();
    _linkg_at_auth_reset();

    return _linkg_at_ssh_shutdown();
}

/**
 * @brief 停止AT Server并释放初始化资源。
 */
int _linkg_at_server_deinit(void)
{
    int ret;

    if (!g_at_server.initialized)
    {
        return 0;
    }

    ret = _linkg_at_server_stop();
    if (ret != 0)
    {
        return ret;
    }

    linkg_thread_deinit(&g_at_server.thread);
    _linkg_at_server_reset_context();

    return 0;
}
