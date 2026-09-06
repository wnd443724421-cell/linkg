/**
 * @file linkg_network_ops.c
 * @brief LinkG网络通用能力及接口操作实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-07-23
 */

#define _GNU_SOURCE

#include "linkg_network_ops.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <limits.h>
#include <linux/netlink.h>
#include <linux/rtnetlink.h>
#include <net/if.h>
#include <net/if_arp.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <linux/sockios.h>

#include "linkg_file.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_NETWORK_WAIT_INTERVAL_MS      50U                                     // 网络接口等待检查周期
#define LINKG_NETWORK_US_PER_MS             1000ULL                                 // 每毫秒包含的微秒数
#define LINKG_NETWORK_ROUTE_BUFFER_SIZE     16384U                                  // 路由Netlink接收缓存大小
#define LINKG_NETWORK_IPV4_FORWARD_PATH     "/proc/sys/net/ipv4/ip_forward"         // IPv4转发控制
#define LINKG_NETWORK_IPV6_ACCEPT_RA_FORMAT "/proc/sys/net/ipv6/conf/%s/accept_ra"  // IPv6 RA接收控制
#define LINKG_NETWORK_SYSCTL_PATH_SIZE      128U                                    // sysctl路径缓存大小

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查网络接口名称是否有效。
 */
static bool _network_interface_name_valid(const char *ifname)
{
    size_t length;

    if (ifname == NULL)
    {
        return false;
    }

    length = strnlen(ifname, IFNAMSIZ);

    return length > 0U && length < IFNAMSIZ;
}

/**
 * @brief 初始化网络接口请求结构。
 */
static int _network_ifreq_init(const char *ifname, struct ifreq *ifr)
{
    size_t length;

    if (!_network_interface_name_valid(ifname) || ifr == NULL)
    {
        return -EINVAL;
    }

    length = strnlen(ifname, IFNAMSIZ);

    memset(ifr, 0, sizeof(*ifr));
    memcpy(ifr->ifr_name, ifname, length);

    return 0;
}

/**
 * @brief 打开网络控制接口套接字。
 */
static int _network_control_socket_open(void)
{
    int fd;

    fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
    {
        return -errno;
    }

    return fd;
}

/**
 * @brief 执行网络接口控制命令。
 */
static int _network_interface_ioctl(int fd, unsigned long request, struct ifreq *ifr)
{
    if (fd < 0 || ifr == NULL)
    {
        return -EINVAL;
    }

    if (ioctl(fd, request, ifr) != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 关闭网络控制接口套接字。
 */
static int _network_control_socket_close(int fd, int result)
{
    if (fd < 0)
    {
        return result;
    }

    if (close(fd) != 0 && result == 0)
    {
        return -errno;
    }

    return result;
}

/**
 * @brief 生成网络接口IPv6 RA sysctl路径。
 */
static int _network_ipv6_accept_ra_path(const char *ifname, char *path, size_t path_size)
{
    int length;

    if (!_network_interface_name_valid(ifname) || path == NULL || path_size == 0U)
    {
        return -EINVAL;
    }

    if (!linkg_network_interface_exists(ifname))
    {
        return -ENODEV;
    }

    length = snprintf(path, path_size, LINKG_NETWORK_IPV6_ACCEPT_RA_FORMAT, ifname);
    if (length < 0)
    {
        return -EIO;
    }

    if ((size_t)length >= path_size)
    {
        return -ENAMETOOLONG;
    }

    return 0;
}

/**
 * @brief 填充IPv4套接字地址。
 */
static void _network_sockaddr_ipv4_init(struct sockaddr_in *socket_address, const struct in_addr *address)
{
    memset(socket_address, 0, sizeof(*socket_address));

    socket_address->sin_family = AF_INET;
    socket_address->sin_addr   = *address;
}


/**
 * @brief 判断IPv6地址是否属于指定网络前缀。
 */
static bool _network_ipv6_prefix_match(const struct in6_addr *address, const struct in6_addr *prefix, uint8_t prefix_length)
{
    size_t  full_bytes;
    uint8_t remaining_bits;
    uint8_t mask;

    if (address == NULL || prefix == NULL || prefix_length > 128U)
    {
        return false;
    }

    full_bytes = prefix_length / 8U;
    remaining_bits = prefix_length % 8U;

    if (full_bytes > 0U && memcmp(address->s6_addr, prefix->s6_addr, full_bytes) != 0)
    {
        return false;
    }

    if (remaining_bits == 0U)
    {
        return true;
    }

    mask = (uint8_t)(0xFFU << (8U - remaining_bits));

    return (address->s6_addr[full_bytes] & mask) == (prefix->s6_addr[full_bytes] & mask);
}

/**
 * @brief 获取网络接口IPv4地址类属性。
 */
static int _network_interface_get_ipv4_value(const char *ifname, unsigned long request, struct in_addr *value)
{
    const struct sockaddr_in *socket_address;
    struct ifreq              ifr;
    int                       fd;
    int                       ret;

    if (value == NULL || (request != SIOCGIFADDR && request != SIOCGIFNETMASK))
    {
        return -EINVAL;
    }

    memset(value, 0, sizeof(*value));

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    ret = _network_interface_ioctl(fd, request, &ifr);
    if (ret != 0)
    {
        return _network_control_socket_close(fd, ret);
    }

    socket_address = request == SIOCGIFNETMASK
        ? (const struct sockaddr_in *)&ifr.ifr_netmask
        : (const struct sockaddr_in *)&ifr.ifr_addr;

    if (socket_address->sin_family != AF_INET)
    {
        return _network_control_socket_close(fd, -EAFNOSUPPORT);
    }

    *value = socket_address->sin_addr;

    return _network_control_socket_close(fd, 0);
}

/**
 * @brief 打开并绑定NETLINK_ROUTE套接字。
 */
static int _network_route_socket_open(void)
{
    struct sockaddr_nl local_address;
    int                fd;

    fd = socket(AF_NETLINK, SOCK_RAW | SOCK_CLOEXEC, NETLINK_ROUTE);
    if (fd < 0)
    {
        return -errno;
    }

    memset(&local_address, 0, sizeof(local_address));
    local_address.nl_family = AF_NETLINK;

    if (bind(fd, (const struct sockaddr *)&local_address, sizeof(local_address)) != 0)
    {
        int ret;

        ret = -errno;
        close(fd);
        return ret;
    }

    return fd;
}

/**
 * @brief 从RTA属性读取uint32_t数值。
 */
static int _network_route_attribute_get_u32(const struct rtattr *attribute, uint32_t *value)
{
    if (attribute == NULL || value == NULL)
    {
        return -EINVAL;
    }

    if (RTA_PAYLOAD(attribute) < sizeof(*value))
    {
        return -EBADMSG;
    }

    memcpy(value, RTA_DATA(attribute), sizeof(*value));

    return 0;
}

/**
 * @brief 查询指定接口和地址族的主路由表默认网关。
 *
 * @note 若同一接口存在多条默认路由，优先返回metric最小的路由。
 */
static int _network_route_get_default_gateway(const char *ifname, int family, void *gateway, size_t gateway_size)
{
    union
    {
        struct in_addr  ipv4;
        struct in6_addr ipv6;
    } candidate_gateway;
    struct
    {
        struct nlmsghdr header;
        struct rtmsg    route;
    } request;
    struct sockaddr_nl kernel_address;
    unsigned char      buffer[LINKG_NETWORK_ROUTE_BUFFER_SIZE];
    uint32_t           best_metric;
    unsigned int       ifindex;
    bool               found;
    int                fd;
    ssize_t            sent;

    if (!_network_interface_name_valid(ifname) || gateway == NULL)
    {
        return -EINVAL;
    }

    if ((family == AF_INET && gateway_size != sizeof(struct in_addr)) ||
        (family == AF_INET6 && gateway_size != sizeof(struct in6_addr)) ||
        (family != AF_INET && family != AF_INET6))
    {
        return -EINVAL;
    }

    memset(gateway, 0, gateway_size);

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0U)
    {
        return -ENODEV;
    }

    fd = _network_route_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    memset(&request, 0, sizeof(request));
    request.header.nlmsg_len   = NLMSG_LENGTH(sizeof(request.route));
    request.header.nlmsg_type  = RTM_GETROUTE;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_DUMP;
    request.header.nlmsg_seq   = 1U;
    request.route.rtm_family   = (unsigned char)family;
    request.route.rtm_table    = RT_TABLE_UNSPEC;

    memset(&kernel_address, 0, sizeof(kernel_address));
    kernel_address.nl_family = AF_NETLINK;

    sent = sendto(fd,
                  &request,
                  request.header.nlmsg_len,
                  0,
                  (const struct sockaddr *)&kernel_address,
                  sizeof(kernel_address));
    if (sent < 0)
    {
        return _network_control_socket_close(fd, -errno);
    }

    if ((size_t)sent != request.header.nlmsg_len)
    {
        return _network_control_socket_close(fd, -EIO);
    }

    found = false;
    best_metric = UINT32_MAX;

    while (true)
    {
        struct nlmsghdr *message;
        ssize_t          received;
        int              remaining;

        received = recv(fd, buffer, sizeof(buffer), 0);
        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return _network_control_socket_close(fd, -errno);
        }

        if (received == 0)
        {
            return _network_control_socket_close(fd, -EIO);
        }

        if (received > INT_MAX)
        {
            return _network_control_socket_close(fd, -EOVERFLOW);
        }

        remaining = (int)received;
        message = (struct nlmsghdr *)buffer;

        while (NLMSG_OK(message, remaining))
        {
            struct rtmsg *route;
            struct rtattr *attribute;
            uint32_t       route_table;
            uint32_t       route_ifindex;
            uint32_t       route_metric;
            int            attributes_length;
            bool           gateway_present;
            int            ret;

            if (message->nlmsg_seq != request.header.nlmsg_seq)
            {
                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            if (message->nlmsg_type == NLMSG_DONE)
            {
                if ((message->nlmsg_flags & NLM_F_DUMP_INTR) != 0U)
                {
                    return _network_control_socket_close(fd, -EINTR);
                }

                return _network_control_socket_close(fd, found ? 0 : -ENOENT);
            }

            if (message->nlmsg_type == NLMSG_ERROR)
            {
                const struct nlmsgerr *netlink_error;

                if (NLMSG_PAYLOAD(message, 0) < sizeof(*netlink_error))
                {
                    return _network_control_socket_close(fd, -EBADMSG);
                }

                netlink_error = (const struct nlmsgerr *)NLMSG_DATA(message);
                if (netlink_error->error != 0)
                {
                    return _network_control_socket_close(fd, netlink_error->error);
                }

                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            if (message->nlmsg_type != RTM_NEWROUTE || NLMSG_PAYLOAD(message, 0) < sizeof(struct rtmsg))
            {
                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            route = (struct rtmsg *)NLMSG_DATA(message);
            if (route->rtm_family != family || route->rtm_dst_len != 0U || route->rtm_type != RTN_UNICAST)
            {
                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            route_table = route->rtm_table;
            route_ifindex = 0U;
            route_metric = 0U;
            gateway_present = false;
            memset(&candidate_gateway, 0, sizeof(candidate_gateway));

            attributes_length = RTM_PAYLOAD(message);
            attribute = RTM_RTA(route);

            while (RTA_OK(attribute, attributes_length))
            {
                switch (attribute->rta_type)
                {
                    case RTA_OIF:
                        ret = _network_route_attribute_get_u32(attribute, &route_ifindex);
                        if (ret != 0)
                        {
                            return _network_control_socket_close(fd, ret);
                        }
                        break;

                    case RTA_PRIORITY:
                        ret = _network_route_attribute_get_u32(attribute, &route_metric);
                        if (ret != 0)
                        {
                            return _network_control_socket_close(fd, ret);
                        }
                        break;

                    case RTA_TABLE:
                        ret = _network_route_attribute_get_u32(attribute, &route_table);
                        if (ret != 0)
                        {
                            return _network_control_socket_close(fd, ret);
                        }
                        break;

                    case RTA_GATEWAY:
                        if (RTA_PAYLOAD(attribute) < gateway_size)
                        {
                            return _network_control_socket_close(fd, -EBADMSG);
                        }

                        memcpy(&candidate_gateway, RTA_DATA(attribute), gateway_size);
                        gateway_present = true;
                        break;

                    default:
                        break;
                }

                attribute = RTA_NEXT(attribute, attributes_length);
            }

            if (attributes_length != 0)
            {
                return _network_control_socket_close(fd, -EBADMSG);
            }

            if (route_table == RT_TABLE_MAIN &&
                route_ifindex == ifindex &&
                gateway_present &&
                (!found || route_metric < best_metric))
            {
                memcpy(gateway, &candidate_gateway, gateway_size);
                best_metric = route_metric;
                found = true;
            }

            message = NLMSG_NEXT(message, remaining);
        }

        if (remaining != 0)
        {
            return _network_control_socket_close(fd, -EBADMSG);
        }
    }
}

/****************************** IPv4转换 ******************************/

/**
 * @brief 将字符串转换为IPv4地址。
 */
bool linkg_network_ipv4_from_string(const char *string, struct in_addr *out)
{
    struct in_addr address;

    if (string == NULL || out == NULL)
    {
        return false;
    }

    if (inet_pton(AF_INET, string, &address) != 1)
    {
        return false;
    }

    *out = address;

    return true;
}

/**
 * @brief 将IPv4地址转换为字符串。
 */
bool linkg_network_ipv4_to_string(const struct in_addr *address, char *out, size_t out_size)
{
    if (address == NULL || out == NULL || out_size < INET_ADDRSTRLEN)
    {
        return false;
    }

    return inet_ntop(AF_INET, address, out, out_size) != NULL;
}

/**
 * @brief 根据前缀长度生成IPv4网络掩码。
 */
bool linkg_network_ipv4_netmask_from_prefix(uint8_t prefix_length, struct in_addr *out)
{
    uint32_t mask;

    if (out == NULL || prefix_length == 0U || prefix_length > 32U)
    {
        return false;
    }

    mask = prefix_length == 32U
        ? UINT32_MAX
        : UINT32_MAX << (32U - prefix_length);

    out->s_addr = htonl(mask);

    return true;
}

/**
 * @brief 根据IPv4地址和子网掩码计算网络地址。
 */
bool linkg_network_ipv4_network_address(const struct in_addr *address, const struct in_addr *netmask, struct in_addr *network)
{
    if (address == NULL || netmask == NULL || network == NULL)
    {
        return false;
    }

    if (!linkg_network_ipv4_netmask_valid(netmask))
    {
        return false;
    }

    network->s_addr = address->s_addr & netmask->s_addr;

    return true;
}

/****************************** IPv4校验 ******************************/

/**
 * @brief 检查IPv4地址是否可作为普通单播主机地址。
 *
 * @note 该接口只根据地址本身排除特殊用途地址，
 *       无法判断具体子网中的Network/Broadcast地址。
 */
bool linkg_network_ipv4_address_valid(const struct in_addr *address)
{
    uint32_t value;

    if (address == NULL)
    {
        return false;
    }

    value = ntohl(address->s_addr);

    // 拒绝0.0.0.0/8。
    if ((value & 0xFF000000U) == 0x00000000U)
    {
        return false;
    }

    // 拒绝127.0.0.0/8 Loopback。
    if ((value & 0xFF000000U) == 0x7F000000U)
    {
        return false;
    }

    // 拒绝224.0.0.0/4 Multicast。
    if ((value & 0xF0000000U) == 0xE0000000U)
    {
        return false;
    }

    // 拒绝240.0.0.0/4 Reserved，同时包含255.255.255.255。
    if ((value & 0xF0000000U) == 0xF0000000U)
    {
        return false;
    }

    return true;
}

/**
 * @brief 检查网络IPv4网络掩码是否有效。
 */
bool linkg_network_ipv4_netmask_valid(const struct in_addr *netmask)
{
    uint32_t mask;
    uint32_t inverted;

    if (netmask == NULL)
    {
        return false;
    }

    mask = ntohl(netmask->s_addr);
    if (mask == 0U)
    {
        return false;
    }

    inverted = ~mask;

    return (inverted & (inverted + 1U)) == 0U;
}

/****************************** IPv6校验 ******************************/

/**
 * @brief 判断IPv6地址是否属于公网Global Unicast范围2000::/3。
 */
bool linkg_network_ipv6_address_is_global(const struct in6_addr *address)
{
    if (address == NULL)
    {
        return false;
    }

    return (address->s6_addr[0] & 0xE0U) == 0x20U;
}

/****************************** 网络接口 ******************************/

/**
 * @brief 检查网络接口是否存在。
 */
bool linkg_network_interface_exists(const char *ifname)
{
    if (!_network_interface_name_valid(ifname))
    {
        return false;
    }

    return if_nametoindex(ifname) != 0U;
}

/**
 * @brief 等待网络接口出现。
 */
int linkg_network_interface_wait(const char *ifname, uint32_t timeout_ms)
{
    uint64_t start_us;
    uint64_t current_us;
    uint64_t deadline_us;
    uint64_t remaining_us;
    uint32_t sleep_ms;
    int      ret;

    if (!_network_interface_name_valid(ifname))
    {
        return -EINVAL;
    }

    if (linkg_network_interface_exists(ifname))
    {
        return 0;
    }

    if (timeout_ms == 0U)
    {
        return -ETIMEDOUT;
    }

    start_us = linkg_time_monotonic_us();
    if (start_us == 0U)
    {
        return -EIO;
    }

    deadline_us = start_us + (uint64_t)timeout_ms * LINKG_NETWORK_US_PER_MS;

    while (true)
    {
        current_us = linkg_time_monotonic_us();
        if (current_us == 0U || current_us < start_us)
        {
            return -EIO;
        }

        if (current_us >= deadline_us)
        {
            return -ETIMEDOUT;
        }

        remaining_us = deadline_us - current_us;
        sleep_ms     = LINKG_NETWORK_WAIT_INTERVAL_MS;

        if (remaining_us < (uint64_t)sleep_ms * LINKG_NETWORK_US_PER_MS)
        {
            sleep_ms = (uint32_t)((remaining_us + LINKG_NETWORK_US_PER_MS - 1U) /
                                  LINKG_NETWORK_US_PER_MS);
        }

        ret = linkg_time_sleep_ms(sleep_ms);
        if (ret != 0)
        {
            return ret;
        }

        if (linkg_network_interface_exists(ifname))
        {
            return 0;
        }
    }
}

/**
 * @brief 获取网络接口当前启用状态。
 */
int linkg_network_interface_is_up(const char *ifname, bool *up)
{
    struct ifreq ifr;
    int          fd;
    int          ret;

    if (up == NULL)
    {
        return -EINVAL;
    }

    *up = false;

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    ret = _network_interface_ioctl(fd, SIOCGIFFLAGS, &ifr);
    if (ret != 0)
    {
        return _network_control_socket_close(fd, ret);
    }

    *up = (ifr.ifr_flags & IFF_UP) != 0;

    return _network_control_socket_close(fd, 0);
}

/**
 * @brief 设置网络接口启用状态。
 */
int linkg_network_interface_set_up(const char *ifname, bool up)
{
    struct ifreq ifr;
    bool         current_up;
    int          fd;
    int          ret;

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    ret = _network_interface_ioctl(fd, SIOCGIFFLAGS, &ifr);
    if (ret != 0)
    {
        return _network_control_socket_close(fd, ret);
    }

    current_up = (ifr.ifr_flags & IFF_UP) != 0;
    if (current_up == up)
    {
        return _network_control_socket_close(fd, 0);
    }

    if (up)
    {
        ifr.ifr_flags = (short)(ifr.ifr_flags | IFF_UP);
    }
    else
    {
        ifr.ifr_flags = (short)(ifr.ifr_flags & (short)~IFF_UP);
    }

    ret = _network_interface_ioctl(fd, SIOCSIFFLAGS, &ifr);

    return _network_control_socket_close(fd, ret);
}

/**
 * @brief 设置网络接口MTU。
 */
int linkg_network_interface_set_mtu(const char *ifname, uint32_t mtu)
{
    struct ifreq ifr;
    int          fd;
    int          ret;

    if (mtu == 0U || mtu > (uint32_t)INT_MAX)
    {
        return -EINVAL;
    }

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    ifr.ifr_mtu = (int)mtu;

    ret = _network_interface_ioctl(fd, SIOCSIFMTU, &ifr);

    return _network_control_socket_close(fd, ret);
}

/**
 * @brief 设置网络接口发送队列长度。
 */
int linkg_network_interface_set_tx_queue_length(const char *ifname, uint32_t queue_length)
{
    struct ifreq ifr;
    int          fd;
    int          ret;

    if (queue_length == 0U ||
        queue_length > (uint32_t)INT_MAX)
    {
        return -EINVAL;
    }

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    ifr.ifr_qlen = (int)queue_length;

    ret = _network_interface_ioctl(fd, SIOCSIFTXQLEN, &ifr);

    return _network_control_socket_close(fd, ret);
}

/**
 * @brief 设置网络接口MAC地址。
 */
int linkg_network_interface_set_mac(const char *ifname, const uint8_t mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH])
{
    struct ifreq ifr;
    int fd;
    int ret;

    if (!_network_interface_name_valid(ifname) || mac == NULL)
    {
        return -EINVAL;
    }

    if ((mac[0] & 0x01U) != 0U)
    {
        return -EINVAL;
    }

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    ifr.ifr_hwaddr.sa_family = ARPHRD_ETHER;
    memcpy(ifr.ifr_hwaddr.sa_data, mac, LINKG_NETWORK_MAC_ADDRESS_LENGTH);

    ret = _network_interface_ioctl(fd, SIOCSIFHWADDR, &ifr);

    return _network_control_socket_close(fd, ret);
}

/**
 * @brief 设置网络接口IPv4地址和子网掩码。
 */
int linkg_network_interface_set_ipv4(const char *ifname, const struct in_addr *address, const struct in_addr *netmask)
{
    struct sockaddr_in socket_address;
    struct ifreq       ifr;
    int                fd;
    int                ret;

    if (!_network_interface_name_valid(ifname) ||
        !linkg_network_ipv4_address_valid(address) ||
        !linkg_network_ipv4_netmask_valid(netmask))
    {
        return -EINVAL;
    }

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return ret;
    }

    fd = _network_control_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    _network_sockaddr_ipv4_init(&socket_address, address);
    memcpy(&ifr.ifr_addr, &socket_address, sizeof(socket_address));

    ret = _network_interface_ioctl(fd, SIOCSIFADDR, &ifr);
    if (ret != 0)
    {
        return _network_control_socket_close(fd, ret);
    }

    ret = _network_ifreq_init(ifname, &ifr);
    if (ret != 0)
    {
        return _network_control_socket_close(fd, ret);
    }

    _network_sockaddr_ipv4_init(&socket_address, netmask);
    memcpy(&ifr.ifr_netmask, &socket_address, sizeof(socket_address));

    ret = _network_interface_ioctl(fd, SIOCSIFNETMASK, &ifr);

    return _network_control_socket_close(fd, ret);
}

/**
 * @brief 获取网络接口当前IPv4地址。
 */
int linkg_network_interface_get_ipv4(const char *ifname, struct in_addr *address)
{
    return _network_interface_get_ipv4_value(ifname, SIOCGIFADDR, address);
}

/**
 * @brief 获取网络接口当前IPv4子网掩码。
 */
int linkg_network_interface_get_ipv4_netmask(const char *ifname, struct in_addr *netmask)
{
    return _network_interface_get_ipv4_value(ifname, SIOCGIFNETMASK, netmask);
}

/**
 * @brief 增加或删除网络接口IPv4地址。
 */
static int _network_interface_modify_ipv4_address(const char *ifname, const struct in_addr *address, uint8_t prefix_length, bool add)
{
    struct
    {
        struct nlmsghdr header;
        struct ifaddrmsg address;
        unsigned char attributes[2U * RTA_SPACE(sizeof(struct in_addr))];
    } request;
    struct sockaddr_nl kernel_address;
    struct rtattr     *attribute;
    unsigned char      buffer[4096];
    unsigned int       ifindex;
    uint32_t           sequence;
    ssize_t            received;
    ssize_t            sent;
    int                remaining;
    int                fd;

    if (!_network_interface_name_valid(ifname) ||
        !linkg_network_ipv4_address_valid(address) ||
        prefix_length > 32U)
    {
        return -EINVAL;
    }

    ifindex = if_nametoindex(ifname);
    if (ifindex == 0U)
    {
        return -ENODEV;
    }

    fd = _network_route_socket_open();
    if (fd < 0)
    {
        return fd;
    }

    memset(&request, 0, sizeof(request));

    sequence = 1U;

    request.header.nlmsg_len   = NLMSG_LENGTH(sizeof(request.address));
    request.header.nlmsg_type  = add ? RTM_NEWADDR : RTM_DELADDR;
    request.header.nlmsg_flags = NLM_F_REQUEST | NLM_F_ACK;
    request.header.nlmsg_seq   = sequence;

    if (add)
    {
        request.header.nlmsg_flags |= NLM_F_CREATE | NLM_F_EXCL;
    }

    request.address.ifa_family    = AF_INET;
    request.address.ifa_prefixlen = prefix_length;
    request.address.ifa_scope     = RT_SCOPE_UNIVERSE;
    request.address.ifa_index     = ifindex;

    attribute = (struct rtattr *)((unsigned char *)&request +
                                  NLMSG_ALIGN(request.header.nlmsg_len));

    attribute->rta_type = IFA_LOCAL;
    attribute->rta_len  = RTA_LENGTH(sizeof(*address));

    memcpy(RTA_DATA(attribute), address, sizeof(*address));

    request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) +
                               RTA_LENGTH(sizeof(*address));

    attribute = (struct rtattr *)((unsigned char *)&request +
                                  NLMSG_ALIGN(request.header.nlmsg_len));

    attribute->rta_type = IFA_ADDRESS;
    attribute->rta_len  = RTA_LENGTH(sizeof(*address));

    memcpy(RTA_DATA(attribute), address, sizeof(*address));

    request.header.nlmsg_len = NLMSG_ALIGN(request.header.nlmsg_len) +
                               RTA_LENGTH(sizeof(*address));

    memset(&kernel_address, 0, sizeof(kernel_address));
    kernel_address.nl_family = AF_NETLINK;

    sent = sendto(fd,
                  &request,
                  request.header.nlmsg_len,
                  0,
                  (const struct sockaddr *)&kernel_address,
                  sizeof(kernel_address));
    if (sent < 0)
    {
        return _network_control_socket_close(fd, -errno);
    }

    if ((size_t)sent != request.header.nlmsg_len)
    {
        return _network_control_socket_close(fd, -EIO);
    }

    while (true)
    {
        struct nlmsghdr *message;

        received = recv(fd, buffer, sizeof(buffer), 0);
        if (received < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }

            return _network_control_socket_close(fd, -errno);
        }

        if (received == 0)
        {
            return _network_control_socket_close(fd, -EIO);
        }

        if (received > INT_MAX)
        {
            return _network_control_socket_close(fd, -EOVERFLOW);
        }

        remaining = (int)received;
        message   = (struct nlmsghdr *)buffer;

        while (NLMSG_OK(message, remaining))
        {
            const struct nlmsgerr *netlink_error;

            if (message->nlmsg_seq != sequence)
            {
                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            if (message->nlmsg_type != NLMSG_ERROR)
            {
                message = NLMSG_NEXT(message, remaining);
                continue;
            }

            if (NLMSG_PAYLOAD(message, 0) < sizeof(*netlink_error))
            {
                return _network_control_socket_close(fd, -EBADMSG);
            }

            netlink_error = (const struct nlmsgerr *)NLMSG_DATA(message);

            if (netlink_error->error != 0)
            {
                return _network_control_socket_close(fd, netlink_error->error);
            }

            return _network_control_socket_close(fd, 0);
        }

        if (remaining != 0)
        {
            return _network_control_socket_close(fd, -EBADMSG);
        }
    }
}

/**
 * @brief 向网络接口增加IPv4地址。
 */
int linkg_network_interface_add_ipv4(const char *ifname, const struct in_addr *address, uint8_t prefix_length)
{
    return _network_interface_modify_ipv4_address(ifname, address, prefix_length, true);
}

/**
 * @brief 从网络接口删除IPv4地址。
 */
int linkg_network_interface_remove_ipv4(const char *ifname, const struct in_addr *address, uint8_t prefix_length)
{
    return _network_interface_modify_ipv4_address(ifname, address, prefix_length, false);
}

/**
 * @brief 获取满足指定条件的网络接口IPv6地址。
 */
static int _network_interface_get_ipv6(const char *ifname, bool global_only, const struct in6_addr *prefix, uint8_t prefix_length, struct in6_addr *address)
{
    const struct sockaddr_in6 *socket_address;
    struct ifaddrs            *interfaces;
    struct ifaddrs            *current;
    int                        ret;

    if (!_network_interface_name_valid(ifname) || address == NULL)
    {
        return -EINVAL;
    }

    if (prefix != NULL && prefix_length > 128U)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));

    if (!linkg_network_interface_exists(ifname))
    {
        return -ENODEV;
    }

    if (getifaddrs(&interfaces) != 0)
    {
        return -errno;
    }

    ret = -EADDRNOTAVAIL;

    for (current = interfaces; current != NULL; current = current->ifa_next)
    {
        if (current->ifa_name == NULL || current->ifa_addr == NULL)
        {
            continue;
        }

        if (strcmp(current->ifa_name, ifname) != 0)
        {
            continue;
        }

        if (current->ifa_addr->sa_family != AF_INET6)
        {
            continue;
        }

        socket_address = (const struct sockaddr_in6 *)current->ifa_addr;

        if (IN6_IS_ADDR_UNSPECIFIED(&socket_address->sin6_addr) ||
            IN6_IS_ADDR_LOOPBACK(&socket_address->sin6_addr) ||
            IN6_IS_ADDR_MULTICAST(&socket_address->sin6_addr) ||
            IN6_IS_ADDR_LINKLOCAL(&socket_address->sin6_addr))
        {
            continue;
        }

        if (global_only && !linkg_network_ipv6_address_is_global(&socket_address->sin6_addr))
        {
            continue;
        }

        if (prefix != NULL && !_network_ipv6_prefix_match(&socket_address->sin6_addr, prefix, prefix_length))
        {
            continue;
        }

        *address = socket_address->sin6_addr;
        ret = 0;
        break;
    }

    freeifaddrs(interfaces);

    return ret;
}

/**
 * @brief 获取网络接口当前IPv6地址。
 *
 * @note 忽略未指定、回环、组播及链路本地IPv6地址。
 */
int linkg_network_interface_get_ipv6(const char *ifname, struct in6_addr *address)
{
    return _network_interface_get_ipv6(ifname, false, NULL, 0U, address);
}

/**
 * @brief 获取网络接口当前公网Global IPv6地址。
 */
int linkg_network_interface_get_global_ipv6(const char *ifname, struct in6_addr *address)
{
    return _network_interface_get_ipv6(ifname, true, NULL, 0U, address);
}

/**
 * @brief 获取网络接口指定前缀下的公网Global IPv6地址。
 */
int linkg_network_interface_get_global_ipv6_in_prefix(const char *ifname, const struct in6_addr *prefix, uint8_t prefix_length, struct in6_addr *address)
{
    if (prefix == NULL || prefix_length > 128U)
    {
        return -EINVAL;
    }

    return _network_interface_get_ipv6(ifname, true, prefix, prefix_length, address);
}

/**
 * @brief 设置网络接口IPv6 Router Advertisement接收模式。
 *
 * @note FORCE模式对应Linux accept_ra=2，在IPv6 forwarding开启时仍允许接口接收RA。
 */
int linkg_network_interface_ipv6_accept_ra_set(const char *ifname, linkg_network_ipv6_accept_ra_t mode)
{
    char path[LINKG_NETWORK_SYSCTL_PATH_SIZE];
    char value;
    int ret;

    if (mode < LINKG_NETWORK_IPV6_ACCEPT_RA_DISABLED || mode > LINKG_NETWORK_IPV6_ACCEPT_RA_FORCE)
    {
        return -EINVAL;
    }

    ret = _network_ipv6_accept_ra_path(ifname, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    value = (char)('0' + (int)mode);

    return linkg_file_write_all(path, &value, sizeof(value));
}

/**
 * @brief 获取网络接口IPv6 Router Advertisement接收模式。
 */
int linkg_network_interface_ipv6_accept_ra_get(const char *ifname, linkg_network_ipv6_accept_ra_t *mode)
{
    char path[LINKG_NETWORK_SYSCTL_PATH_SIZE];
    FILE *stream;
    int close_ret;
    int value;
    int ret;

    if (mode == NULL)
    {
        return -EINVAL;
    }

    *mode = LINKG_NETWORK_IPV6_ACCEPT_RA_DISABLED;

    ret = _network_ipv6_accept_ra_path(ifname, path, sizeof(path));
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_file_stream_open(path, "r", &stream);
    if (ret != 0)
    {
        return ret;
    }

    value = fgetc(stream);
    if (value == EOF)
    {
        ret = ferror(stream) ? (errno != 0 ? -errno : -EIO) : -EBADMSG;
    }
    else if (value < '0' || value > '2')
    {
        ret = -EBADMSG;
    }
    else
    {
        *mode = (linkg_network_ipv6_accept_ra_t)(value - '0');
        ret = 0;
    }

    close_ret = linkg_file_stream_close(&stream);
    if (ret == 0 && close_ret != 0)
    {
        ret = close_ret;
    }

    return ret;
}

/**
 * @brief 判断两个IPv4网段是否存在重叠。
 */
bool linkg_network_ipv4_subnet_overlap(const struct in_addr *left_ip, const struct in_addr *left_netmask, const struct in_addr *right_ip, const struct in_addr *right_netmask)
{
    uint32_t left_address;
    uint32_t left_mask;
    uint32_t left_first;
    uint32_t left_last;
    uint32_t right_address;
    uint32_t right_mask;
    uint32_t right_first;
    uint32_t right_last;

    if (left_ip == NULL ||
        left_netmask == NULL ||
        right_ip == NULL ||
        right_netmask == NULL)
    {
        return false;
    }

    if (!linkg_network_ipv4_netmask_valid(left_netmask) ||
        !linkg_network_ipv4_netmask_valid(right_netmask))
    {
        return false;
    }

    left_address = ntohl(left_ip->s_addr);
    left_mask = ntohl(left_netmask->s_addr);

    right_address = ntohl(right_ip->s_addr);
    right_mask = ntohl(right_netmask->s_addr);

    left_first = left_address & left_mask;
    left_last = left_first | ~left_mask;

    right_first = right_address & right_mask;
    right_last = right_first | ~right_mask;

    return left_first <= right_last && right_first <= left_last;
}

/****************************** 路由查询 ******************************/

/**
 * @brief 获取指定接口主路由表中的IPv4默认网关。
 */
int linkg_network_route_get_ipv4_default_gateway(const char *ifname, struct in_addr *gateway)
{
    return _network_route_get_default_gateway(ifname, AF_INET, gateway, sizeof(*gateway));
}

/**
 * @brief 获取指定接口主路由表中的IPv6默认网关。
 *
 * @note IPv6默认网关通常为FE80::/10链路本地地址，这是正常的下一跳形式。
 */
int linkg_network_route_get_ipv6_default_gateway(const char *ifname, struct in6_addr *gateway)
{
    return _network_route_get_default_gateway(ifname, AF_INET6, gateway, sizeof(*gateway));
}

/****************************** 系统网络 ******************************/

/**
 * @brief 设置系统IPv4转发状态。
 */
int linkg_network_ipv4_forwarding_set(bool enabled)
{
    const char value = enabled ? '1' : '0';

    return linkg_file_write_all(LINKG_NETWORK_IPV4_FORWARD_PATH, &value, sizeof(value));
}
