/**
 * @file cellular_link_heartbeat.c
 * @brief LinkG蜂窝业务链路心跳实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-31
 */

#define _GNU_SOURCE

#include "cellular_link_heartbeat.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_LINK_HEARTBEAT_THREAD_NAME      "cell-link-hb" // 心跳工作线程名称
#define LINKG_CELLULAR_LINK_HEARTBEAT_INTERVAL_MS      6000           // 业务链路心跳周期，单位毫秒
#define LINKG_CELLULAR_LINK_HEARTBEAT_MAGIC_SIZE       4U             // 心跳Magic长度
#define LINKG_CELLULAR_LINK_HEARTBEAT_VERSION          1U             // 心跳协议版本
#define LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_0       0xA5U          // 心跳保留校验字节0
#define LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_1       0x5AU          // 心跳保留校验字节1
#define LINKG_CELLULAR_LINK_HEARTBEAT_TX_CONTROL_SIZE  CMSG_SPACE(sizeof(struct in6_pktinfo)) // IPv6发送辅助控制区大小

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t magic[LINKG_CELLULAR_LINK_HEARTBEAT_MAGIC_SIZE]; // 心跳Magic
    uint8_t version;                                         // 心跳协议版本
    uint8_t tx_class;                                        // 当前业务Socket类别
    uint8_t reserved_0;                                      // 固定保留校验字节0
    uint8_t reserved_1;                                      // 固定保留校验字节1
} linkg_cellular_link_heartbeat_wire_t;

struct linkg_cellular_link_heartbeat
{
    linkg_thread_t  thread;         // 心跳工作线程
    int            *socket_fds;     // 借用Cellular Link三个业务Socket数组
    const uint16_t *service_ports;  // 借用Cellular Link三个业务端口数组
    const char     *interface_name; // 借用蜂窝出口接口名称
    uint32_t        link_id;        // 当前Cellular Link运行实例标识
};

_Static_assert(sizeof(linkg_cellular_link_heartbeat_wire_t) == 8U, "invalid cellular link heartbeat wire size");

/****************************** 模块常量数据 ******************************/

static const uint8_t g_cellular_link_heartbeat_magic[LINKG_CELLULAR_LINK_HEARTBEAT_MAGIC_SIZE] =
{
    'L',
    'G',
    'H',
    'B'
};

/****************************** 参数校验 ******************************/

/**
 * @brief 校验蜂窝业务链路心跳配置。
 */
static int _linkg_cellular_link_heartbeat_validate_config(const linkg_cellular_link_heartbeat_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->socket_fds == NULL || config->service_ports == NULL)
    {
        return -EINVAL;
    }

    if (config->interface_name == NULL || config->interface_name[0] == '\0')
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验心跳目标是否为有效Global IPv6 Endpoint。
 */
static int _linkg_cellular_link_heartbeat_validate_destination(const linkg_path_endpoint_t *destination)
{
    const struct sockaddr_in6 *address;

    if (destination == NULL)
    {
        return -EINVAL;
    }

    if (destination->length != sizeof(struct sockaddr_in6))
    {
        return -EDESTADDRREQ;
    }

    if (destination->address.ss_family != AF_INET6)
    {
        return -EAFNOSUPPORT;
    }

    address = (const struct sockaddr_in6 *)&destination->address;

    if (!linkg_network_ipv6_address_is_global(&address->sin6_addr))
    {
        return -EDESTADDRREQ;
    }

    return 0;
}

/****************************** Wire辅助 ******************************/

/**
 * @brief 构造指定业务类别的蜂窝链路心跳报文。
 */
static int _linkg_cellular_link_heartbeat_build_wire(linkg_link_tx_class_t tx_class, linkg_cellular_link_heartbeat_wire_t *wire)
{
    if (wire == NULL)
    {
        return -EINVAL;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    memset(wire, 0, sizeof(*wire));
    memcpy(wire->magic, g_cellular_link_heartbeat_magic, sizeof(wire->magic));

    wire->version    = LINKG_CELLULAR_LINK_HEARTBEAT_VERSION;
    wire->tx_class   = (uint8_t)tx_class;
    wire->reserved_0 = LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_0;
    wire->reserved_1 = LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_1;

    return 0;
}

/****************************** 接口辅助 ******************************/

/**
 * @brief 获取当前蜂窝出口接口索引。
 */
static int _linkg_cellular_link_heartbeat_interface_index(const linkg_cellular_link_heartbeat_t *heartbeat, unsigned int *interface_index)
{
    if (heartbeat == NULL || interface_index == NULL)
    {
        return -EINVAL;
    }

    errno = 0;
    *interface_index = if_nametoindex(heartbeat->interface_name);

    if (*interface_index == 0U)
    {
        return errno != 0 ? -errno : -ENODEV;
    }

    return 0;
}

/**
 * @brief 为心跳IPv6 UDP消息构造IPV6_PKTINFO控制信息。
 */
static int _linkg_cellular_link_heartbeat_prepare_pktinfo(struct msghdr *header, void *control, size_t control_size, unsigned int interface_index)
{
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;

    if (header == NULL || control == NULL || control_size < LINKG_CELLULAR_LINK_HEARTBEAT_TX_CONTROL_SIZE || interface_index == 0U)
    {
        return -EINVAL;
    }

    memset(control, 0, control_size);

    header->msg_control    = control;
    header->msg_controllen = control_size;

    cmsg = CMSG_FIRSTHDR(header);
    if (cmsg == NULL)
    {
        return -EIO;
    }

    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type  = IPV6_PKTINFO;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(*pktinfo));

    pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);

    memset(pktinfo, 0, sizeof(*pktinfo));
    pktinfo->ipi6_ifindex = interface_index;

    return 0;
}

/****************************** 心跳发送 ******************************/

/**
 * @brief 使用指定业务Socket向一个Cellular Peer发送心跳。
 */
static int _linkg_cellular_link_heartbeat_send_class(linkg_cellular_link_heartbeat_t *heartbeat, const linkg_path_endpoint_t *destination, linkg_link_tx_class_t tx_class, unsigned int interface_index)
{
    unsigned char                       control[LINKG_CELLULAR_LINK_HEARTBEAT_TX_CONTROL_SIZE];
    linkg_cellular_link_heartbeat_wire_t wire;
    struct sockaddr_in6                 target;
    struct msghdr                       message;
    struct iovec                        iovec;
    ssize_t                             sent;
    int                                 socket_fd;
    int                                 ret;

    if (heartbeat == NULL || destination == NULL)
    {
        return -EINVAL;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    ret = _linkg_cellular_link_heartbeat_validate_destination(destination);
    if (ret != 0)
    {
        return ret;
    }

    socket_fd = heartbeat->socket_fds[tx_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    if (heartbeat->service_ports[tx_class] == 0U)
    {
        return -EDESTADDRREQ;
    }

    ret = _linkg_cellular_link_heartbeat_build_wire(tx_class, &wire);
    if (ret != 0)
    {
        return ret;
    }

    target = *(const struct sockaddr_in6 *)&destination->address;
    target.sin6_port = htons(heartbeat->service_ports[tx_class]);

    memset(&message, 0, sizeof(message));
    memset(&iovec, 0, sizeof(iovec));

    iovec.iov_base = &wire;
    iovec.iov_len  = sizeof(wire);

    message.msg_name    = &target;
    message.msg_namelen = sizeof(target);
    message.msg_iov     = &iovec;
    message.msg_iovlen  = 1U;

    ret = _linkg_cellular_link_heartbeat_prepare_pktinfo(&message, control, sizeof(control), interface_index);
    if (ret != 0)
    {
        return ret;
    }

    do
    {
        sent = sendmsg(socket_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    while (sent < 0 && errno == EINTR);

    if (sent < 0)
    {
        return -errno;
    }

    if ((size_t)sent != sizeof(wire))
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 使用三个业务Socket向一个Cellular Peer发送本轮全部心跳。
 */
static int _linkg_cellular_link_heartbeat_send_peer(linkg_cellular_link_heartbeat_t *heartbeat, const linkg_path_endpoint_t *destination, unsigned int interface_index)
{
    static const linkg_link_tx_class_t classes[LINKG_LINK_TX_CLASS_COUNT] =
    {
        LINKG_LINK_TX_CLASS_DATA,
        LINKG_LINK_TX_CLASS_REALTIME,
        LINKG_LINK_TX_CLASS_VIDEO
    };

    uint32_t index;
    int      first_error;
    int      ret;

    first_error = 0;

    for (index = 0U; index < LINKG_LINK_TX_CLASS_COUNT; index++)
    {
        ret = _linkg_cellular_link_heartbeat_send_class(heartbeat, destination, classes[index], interface_index);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/**
 * @brief 向当前全部活动Cellular Path发送一轮业务链路心跳。
 */
static int _linkg_cellular_link_heartbeat_process(linkg_cellular_link_heartbeat_t *heartbeat)
{
    linkg_path_endpoint_t destinations[LINKG_NODE_PEER_MAX];
    unsigned int          interface_index;
    uint32_t              destination_count;
    uint32_t              index;
    int                   first_error;
    int                   ret;

    if (heartbeat == NULL || heartbeat->link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    ret = _linkg_cellular_link_heartbeat_interface_index(heartbeat, &interface_index);
    if (ret != 0)
    {
        return ret;
    }

    memset(destinations, 0, sizeof(destinations));

    destination_count = 0U;

    ret = linkg_node_get_path_endpoints(heartbeat->link_id,
                                        destinations,
                                        LINKG_NODE_PEER_MAX,
                                        &destination_count);
    if (ret != 0)
    {
        return ret;
    }

    first_error = 0;

    for (index = 0U; index < destination_count; index++)
    {
        ret = _linkg_cellular_link_heartbeat_send_peer(heartbeat,
                                                       &destinations[index],
                                                       interface_index);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 工作线程 ******************************/

/**
 * @brief 运行蜂窝业务链路心跳周期循环。
 */
static int _linkg_cellular_link_heartbeat_run(linkg_cellular_link_heartbeat_t *heartbeat)
{
    struct pollfd descriptor;
    int           poll_result;
    int           wakeup_fd;
    int           ret;

    if (heartbeat == NULL)
    {
        return -EINVAL;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(&heartbeat->thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    memset(&descriptor, 0, sizeof(descriptor));

    descriptor.fd     = wakeup_fd;
    descriptor.events = POLLIN;

    while (linkg_thread_is_running(&heartbeat->thread))
    {
        descriptor.revents = 0;

        do
        {
            poll_result = poll(&descriptor, 1U, LINKG_CELLULAR_LINK_HEARTBEAT_INTERVAL_MS);
        }
        while (poll_result < 0 && errno == EINTR && linkg_thread_is_running(&heartbeat->thread));

        if (poll_result < 0)
        {
            return -errno;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(&heartbeat->thread);
            if (ret != 0)
            {
                return ret;
            }

            if (!linkg_thread_is_running(&heartbeat->thread))
            {
                break;
            }
        }

        if (poll_result == 0)
        {
            // 单个Peer或接口临时失败不终止周期维护，下一轮继续重试。
            (void)_linkg_cellular_link_heartbeat_process(heartbeat);
        }
    }

    return 0;
}

/**
 * @brief 蜂窝业务链路心跳线程入口。
 */
static void _linkg_cellular_link_heartbeat_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_cellular_link_heartbeat_t *heartbeat;
    int                              ret;

    heartbeat = user_data;

    if (thread == NULL || heartbeat == NULL)
    {
        return;
    }

    ret = _linkg_cellular_link_heartbeat_run(heartbeat);
    if (ret != 0 && linkg_thread_is_running(thread))
    {
        LINKG_LOG_ERROR("cellular link heartbeat thread exited unexpectedly, error=%d", ret);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝业务链路心跳模块。
 */
int linkg_cellular_link_heartbeat_create(const linkg_cellular_link_heartbeat_config_t *config, linkg_cellular_link_heartbeat_t **out)
{
    linkg_cellular_link_heartbeat_t *heartbeat;
    int                              ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    ret = _linkg_cellular_link_heartbeat_validate_config(config);
    if (ret != 0)
    {
        return ret;
    }

    heartbeat = calloc(1, sizeof(*heartbeat));
    if (heartbeat == NULL)
    {
        return -ENOMEM;
    }

    heartbeat->socket_fds     = config->socket_fds;
    heartbeat->service_ports  = config->service_ports;
    heartbeat->interface_name = config->interface_name;
    heartbeat->link_id        = LINKG_LINK_ID_INVALID;

    ret = linkg_thread_init(&heartbeat->thread,
                            LINKG_CELLULAR_LINK_HEARTBEAT_THREAD_NAME,
                            _linkg_cellular_link_heartbeat_thread,
                            heartbeat);
    if (ret != 0)
    {
        free(heartbeat);
        return ret;
    }

    *out = heartbeat;

    return 0;
}

/**
 * @brief 启动蜂窝业务链路心跳维护。
 */
int linkg_cellular_link_heartbeat_start(linkg_cellular_link_heartbeat_t *heartbeat, uint32_t link_id)
{
    int ret;

    if (heartbeat == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    if (linkg_thread_is_started(&heartbeat->thread))
    {
        return -EALREADY;
    }

    heartbeat->link_id = link_id;

    ret = linkg_thread_start(&heartbeat->thread);
    if (ret != 0)
    {
        heartbeat->link_id = LINKG_LINK_ID_INVALID;
        return ret;
    }

    return 0;
}

/**
 * @brief 停止蜂窝业务链路心跳维护。
 */
int linkg_cellular_link_heartbeat_stop(linkg_cellular_link_heartbeat_t *heartbeat)
{
    int ret;

    if (heartbeat == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_thread_stop(&heartbeat->thread);
    if (ret != 0)
    {
        return ret;
    }

    heartbeat->link_id = LINKG_LINK_ID_INVALID;

    return 0;
}

/**
 * @brief 销毁蜂窝业务链路心跳模块。
 */
void linkg_cellular_link_heartbeat_destroy(linkg_cellular_link_heartbeat_t *heartbeat)
{
    if (heartbeat == NULL)
    {
        return;
    }

    linkg_thread_deinit(&heartbeat->thread);
    free(heartbeat);
}

/****************************** 报文识别 ******************************/

/**
 * @brief 判断收到的数据是否为当前业务Socket对应的蜂窝链路心跳。
 */
bool linkg_cellular_link_heartbeat_is_packet(const uint8_t *data, uint32_t length, linkg_link_tx_class_t tx_class)
{
    const linkg_cellular_link_heartbeat_wire_t *wire;

    if (data == NULL || length != sizeof(linkg_cellular_link_heartbeat_wire_t))
    {
        return false;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return false;
    }

    wire = (const linkg_cellular_link_heartbeat_wire_t *)data;

    if (memcmp(wire->magic, g_cellular_link_heartbeat_magic, sizeof(wire->magic)) != 0)
    {
        return false;
    }

    if (wire->version != LINKG_CELLULAR_LINK_HEARTBEAT_VERSION)
    {
        return false;
    }

    if (wire->tx_class != (uint8_t)tx_class)
    {
        return false;
    }

    if (wire->reserved_0 != LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_0)
    {
        return false;
    }

    return wire->reserved_1 == LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_1;
}
