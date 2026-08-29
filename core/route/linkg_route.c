/**
 * @file linkg_route.c
 * @brief LinkG虚拟节点路由管理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-29
 */

#include "linkg_route.h"

#include <arpa/inet.h>
#include <errno.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_network_ops.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_ROUTE_NETLINK_ATTRIBUTE_BUFFER_SIZE 64U   // Netlink路由属性缓冲区大小
#define LINKG_ROUTE_NETLINK_ACK_BUFFER_SIZE       4096U // Netlink ACK接收缓冲区大小

/****************************** 内部类型 ******************************/

typedef struct
{
    struct nlmsghdr header;                                                 // Netlink消息头
    struct rtmsg    route;                                                  // IPv4路由消息
    uint8_t         attributes[LINKG_ROUTE_NETLINK_ATTRIBUTE_BUFFER_SIZE];  // Netlink路由属性
} linkg_route_netlink_request_t;

typedef struct
{
    pthread_mutex_t        lock;            // 路由操作锁，串行保护Netlink请求和ACK
    linkg_network_config_t network_config;  // 当前网络配置快照
    uint32_t               interface_index; // linkg0接口索引
    uint32_t               sequence;        // Netlink请求序列号
    int                    netlink_fd;      // 持久化NETLINK_ROUTE套接字
    bool                   initialized;     // 模块是否已经初始化
} linkg_route_context_t;

/****************************** 全局上下文 ******************************/

static linkg_route_context_t g_route =
{
    .netlink_fd = -1 // Netlink套接字尚未创建
};

/****************************** Node辅助 ******************************/

/**
 * @brief 判断Node ID是否可以作为远端路由目标。
 */
static bool _linkg_route_node_id_valid(uint8_t node_id)
{
    if (node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return false;
    }

    return node_id != g_route.network_config.node_id;
}

/**
 * @brief 根据Node ID获取对应虚拟Endpoint子网。
 */
static int _linkg_route_get_node_subnet(uint8_t node_id, linkg_network_ipv4_config_t *subnet)
{
    int ret;

    if (subnet == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_route_node_id_valid(node_id))
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_node_virtual_subnet(&g_route.network_config, node_id, subnet);
    if (ret != 0)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 获取IPv4网络掩码对应的前缀长度。
 */
static int _linkg_route_prefix_length(const struct in_addr *netmask, uint8_t *prefix_length)
{
    uint32_t mask;
    uint8_t  prefix;

    if (netmask == NULL || prefix_length == NULL)
    {
        return -EINVAL;
    }

    if (!linkg_network_ipv4_netmask_valid(netmask))
    {
        return -EINVAL;
    }

    mask   = ntohl(netmask->s_addr);
    prefix = 0U;

    while ((mask & 0x80000000U) != 0U)
    {
        prefix++;
        mask <<= 1U;
    }

    *prefix_length = prefix;

    return 0;
}

/****************************** Netlink辅助 ******************************/

/**
 * @brief 打开并绑定持久化NETLINK_ROUTE套接字。
 */
static int _linkg_route_netlink_open(void)
{
    struct sockaddr_nl local_address;
    int                fd;
    int                ret;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        return -errno;
    }

    memset(&local_address, 0, sizeof(local_address));

    local_address.nl_family = AF_NETLINK;
    local_address.nl_pid    = 0U;
    local_address.nl_groups = 0U;

    if (bind(fd, (const struct sockaddr *)&local_address, sizeof(local_address)) != 0)
    {
        ret = -errno;

        close(fd);

        return ret;
    }

    return fd;
}

/**
 * @brief 向Netlink消息追加路由属性。
 */
static int _linkg_route_netlink_add_attribute(struct nlmsghdr *header, size_t capacity, uint16_t type, const void *data, size_t data_length)
{
    struct rtattr *attribute;
    size_t         offset;
    size_t         attribute_length;
    size_t         required_length;

    if (header == NULL || data == NULL || data_length == 0U)
    {
        return -EINVAL;
    }

    offset           = NLMSG_ALIGN(header->nlmsg_len);
    attribute_length = RTA_LENGTH(data_length);
    required_length  = offset + RTA_ALIGN(attribute_length);

    if (required_length > capacity)
    {
        return -ENOBUFS;
    }

    attribute = (struct rtattr *)((uint8_t *)header + offset);

    attribute->rta_type = type;
    attribute->rta_len  = (unsigned short)attribute_length;

    memcpy(RTA_DATA(attribute), data, data_length);

    header->nlmsg_len = (uint32_t)(offset + attribute_length);

    return 0;
}

/**
 * @brief 获取下一个Netlink请求序列号。
 *
 * 调用方必须已经持有Route路由操作锁。
 */
static uint32_t _linkg_route_netlink_next_sequence_locked(void)
{
    g_route.sequence++;

    if (g_route.sequence == 0U)
    {
        g_route.sequence++;
    }

    return g_route.sequence;
}

/**
 * @brief 等待指定Netlink请求的ACK。
 *
 * Route使用独立且不订阅任何Multicast Group的Netlink套接字，
 * 同时所有请求在Route锁下串行执行，因此同一时刻只有一个请求等待ACK。
 */
static int _linkg_route_netlink_wait_ack_locked(uint32_t sequence)
{
    uint8_t          buffer[LINKG_ROUTE_NETLINK_ACK_BUFFER_SIZE];
    struct nlmsghdr *message;
    ssize_t          received;
    int              remaining;

    for (;;)
    {
        do
        {
            received = recv(g_route.netlink_fd, buffer, sizeof(buffer), 0);
        }
        while (received < 0 && errno == EINTR);

        if (received < 0)
        {
            return -errno;
        }

        if (received == 0)
        {
            return -EIO;
        }

        remaining = (int)received;
        message   = (struct nlmsghdr *)buffer;

        while (NLMSG_OK(message, remaining))
        {
            if (message->nlmsg_seq != sequence)
            {
                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            if (message->nlmsg_type != NLMSG_ERROR)
            {
                return -EPROTO;
            }

            if (NLMSG_PAYLOAD(message, 0) < sizeof(struct nlmsgerr))
            {
                return -EBADMSG;
            }

            {
                const struct nlmsgerr *netlink_error;

                netlink_error = (const struct nlmsgerr *)NLMSG_DATA(message);

                if (netlink_error->error == 0)
                {
                    return 0;
                }

                if (netlink_error->error < 0)
                {
                    return netlink_error->error;
                }

                return -EIO;
            }
        }

        if (remaining != 0)
        {
            return -EBADMSG;
        }
    }
}

/**
 * @brief 发送Netlink路由请求并同步等待ACK。
 *
 * 调用方必须已经持有Route路由操作锁。
 */
static int _linkg_route_netlink_execute_locked(linkg_route_netlink_request_t *request)
{
    struct sockaddr_nl kernel_address;
    uint32_t           sequence;
    ssize_t            sent;

    if (request == NULL || g_route.netlink_fd < 0)
    {
        return -EINVAL;
    }

    sequence = _linkg_route_netlink_next_sequence_locked();

    request->header.nlmsg_seq = sequence;

    memset(&kernel_address, 0, sizeof(kernel_address));

    kernel_address.nl_family = AF_NETLINK;
    kernel_address.nl_pid    = 0U;

    do
    {
        sent = sendto(g_route.netlink_fd,
                      request,
                      request->header.nlmsg_len,
                      0,
                      (const struct sockaddr *)&kernel_address,
                      sizeof(kernel_address));
    }
    while (sent < 0 && errno == EINTR);

    if (sent < 0)
    {
        return -errno;
    }

    if ((size_t)sent != request->header.nlmsg_len)
    {
        return -EIO;
    }

    return _linkg_route_netlink_wait_ack_locked(sequence);
}

/****************************** Linux路由 ******************************/

/**
 * @brief 构造并执行虚拟节点Linux路由添加或删除操作。
 *
 * 调用方必须已经持有Route路由操作锁。
 */
static int _linkg_route_linux_modify_locked(uint16_t message_type, const linkg_network_ipv4_config_t *subnet)
{
    linkg_route_netlink_request_t request;
    uint8_t                       prefix_length;
    int                           ret;

    if (subnet == NULL)
    {
        return -EINVAL;
    }

    if (message_type != RTM_NEWROUTE && message_type != RTM_DELROUTE)
    {
        return -EINVAL;
    }

    ret = _linkg_route_prefix_length(&subnet->netmask, &prefix_length);
    if (ret != 0)
    {
        return ret;
    }

    memset(&request, 0, sizeof(request));

    request.header.nlmsg_len   = NLMSG_LENGTH(sizeof(request.route));
    request.header.nlmsg_type  = message_type;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;

    if (message_type == RTM_NEWROUTE)
    {
        request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_REPLACE;
    }

    request.route.rtm_family   = AF_INET;
    request.route.rtm_dst_len  = prefix_length;
    request.route.rtm_table    = RT_TABLE_MAIN;
    request.route.rtm_protocol = RTPROT_STATIC;
    request.route.rtm_scope    = RT_SCOPE_LINK;
    request.route.rtm_type     = RTN_UNICAST;

    ret = _linkg_route_netlink_add_attribute(&request.header, sizeof(request), RTA_DST, &subnet->ip.s_addr, sizeof(subnet->ip.s_addr));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_route_netlink_add_attribute(&request.header, sizeof(request), RTA_OIF, &g_route.interface_index, sizeof(g_route.interface_index));
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_route_netlink_execute_locked(&request);
}

/**
 * @brief 添加或替换指定虚拟节点Linux路由。
 *
 * 调用方必须已经持有Route路由操作锁。
 */
static int _linkg_route_linux_add_locked(const linkg_network_ipv4_config_t *subnet)
{
    return _linkg_route_linux_modify_locked(RTM_NEWROUTE, subnet);
}

/**
 * @brief 删除指定虚拟节点Linux路由。
 *
 * 路由或者linkg0已经不存在时均视为达到目标状态，
 * 保证删除操作严格幂等。
 *
 * 调用方必须已经持有Route路由操作锁。
 */
static int _linkg_route_linux_remove_locked(const linkg_network_ipv4_config_t *subnet)
{
    int ret;

    ret = _linkg_route_linux_modify_locked(RTM_DELROUTE, subnet);

    if (ret == -ENOENT ||
        ret == -ESRCH ||
        ret == -ENODEV)
    {
        return 0;
    }

    return ret;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化虚拟节点路由管理模块。
 *
 * 调用前linkg0必须已经创建。
 * network_config由Route复制保存，调用方无需保持原对象生命周期。
 *
 * Route不维护软件路由表，只持有一个NETLINK_ROUTE套接字并负责
 * 将远端Node ID映射到对应虚拟子网后操作Linux主路由表。
 */
int linkg_route_init(const linkg_network_config_t *network_config)
{
    linkg_network_ipv4_config_t virtual_network;
    uint32_t                    interface_index;
    int                         netlink_fd;
    int                         ret;

    if (network_config == NULL)
    {
        return -EINVAL;
    }

    if (g_route.initialized)
    {
        return -EALREADY;
    }

    if (network_config->node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        network_config->node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    memset(&virtual_network, 0, sizeof(virtual_network));

    ret = linkg_network_config_get_virtual_network(network_config, &virtual_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    interface_index = if_nametoindex(LINKG_RESOURCE_INTERFACE_TUN);
    if (interface_index == 0U)
    {
        return -ENODEV;
    }

    netlink_fd = _linkg_route_netlink_open();
    if (netlink_fd < 0)
    {
        return netlink_fd;
    }

    memset(&g_route, 0, sizeof(g_route));

    g_route.netlink_fd = -1;

    ret = pthread_mutex_init(&g_route.lock, NULL);
    if (ret != 0)
    {
        close(netlink_fd);

        memset(&g_route, 0, sizeof(g_route));
        g_route.netlink_fd = -1;

        return -ret;
    }

    g_route.network_config = *network_config;
    g_route.interface_index = interface_index;
    g_route.sequence        = 0U;
    g_route.netlink_fd      = netlink_fd;
    g_route.initialized     = true;

    return 0;
}

/**
 * @brief 反初始化虚拟节点路由管理模块。
 *
 * 调用前Discovery必须已经停止并且不得再执行Route操作。
 * Discovery负责在停止过程中撤销其管理的远端节点路由。
 */
int linkg_route_deinit(void)
{
    int first_error;
    int ret;

    if (!g_route.initialized)
    {
        return 0;
    }

    first_error = 0;

    if (g_route.netlink_fd >= 0)
    {
        if (close(g_route.netlink_fd) != 0)
        {
            first_error = -errno;
        }

        g_route.netlink_fd = -1;
    }

    ret = pthread_mutex_destroy(&g_route.lock);
    if (ret != 0 && first_error == 0)
    {
        first_error = -ret;
    }

    memset(&g_route, 0, sizeof(g_route));

    g_route.netlink_fd = -1;

    return first_error;
}

/****************************** 路由管理 ******************************/

/**
 * @brief 添加或更新指定远端节点的虚拟子网路由。
 *
 * 使用Netlink CREATE加REPLACE语义。
 * 对同一个Node重复调用不会产生重复路由，操作具有幂等性。
 */
int linkg_route_add_node(uint8_t node_id)
{
    linkg_network_ipv4_config_t subnet;
    int                         lock_ret;
    int                         ret;

    if (!g_route.initialized)
    {
        return -ENODEV;
    }

    lock_ret = pthread_mutex_lock(&g_route.lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!_linkg_route_node_id_valid(node_id))
    {
        pthread_mutex_unlock(&g_route.lock);
        return -EINVAL;
    }

    memset(&subnet, 0, sizeof(subnet));

    ret = _linkg_route_get_node_subnet(node_id, &subnet);
    if (ret == 0)
    {
        ret = _linkg_route_linux_add_locked(&subnet);
    }

    pthread_mutex_unlock(&g_route.lock);

    return ret;
}

/**
 * @brief 删除指定远端节点的虚拟子网路由。
 *
 * 路由已经不存在或者linkg0已经被删除时同样返回成功。
 */
int linkg_route_remove_node(uint8_t node_id)
{
    linkg_network_ipv4_config_t subnet;
    int                         lock_ret;
    int                         ret;

    if (!g_route.initialized)
    {
        return -ENODEV;
    }

    lock_ret = pthread_mutex_lock(&g_route.lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!_linkg_route_node_id_valid(node_id))
    {
        pthread_mutex_unlock(&g_route.lock);
        return -EINVAL;
    }

    memset(&subnet, 0, sizeof(subnet));

    ret = _linkg_route_get_node_subnet(node_id, &subnet);
    if (ret == 0)
    {
        ret = _linkg_route_linux_remove_locked(&subnet);
    }

    pthread_mutex_unlock(&g_route.lock);

    return ret;
}
