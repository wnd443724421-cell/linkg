/**
 * @file linkg_network_ops.c
 * @brief LinkG网络通用能力及接口操作实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#define _GNU_SOURCE

#include "linkg_network_ops.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>
#include <ifaddrs.h>
#include <net/if_arp.h>

#include "linkg_file.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_NETWORK_WAIT_INTERVAL_MS 50U      // 网络接口等待检查周期
#define LINKG_NETWORK_US_PER_MS        1000ULL  // 每毫秒包含的微秒数

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
int  linkg_network_interface_wait(const char *ifname, uint32_t timeout_ms)
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
 * @brief 设置网络接口启用状态。
 */
int  linkg_network_interface_set_up(const char *ifname, bool up)
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
int  linkg_network_interface_set_mtu(const char *ifname, uint32_t mtu)
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
 * @brief 设置网络接口IPv4地址和子网掩码。
 */
int  linkg_network_interface_set_ipv4(const char *ifname, const struct in_addr *address, const struct in_addr *netmask)
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
int  linkg_network_interface_get_ipv4(const char *ifname, struct in_addr *address)
{
    const struct sockaddr_in *socket_address;
    struct ifreq request;
    size_t name_length;
    int socket_fd;
    int ret;

    if (ifname == NULL)
    {
        return -EINVAL;
    }

    if (address == NULL)
    {
        return -EINVAL;
    }

    name_length = strnlen(ifname, IFNAMSIZ);
    if (name_length == 0U || name_length >= IFNAMSIZ)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));
    memset(&request, 0, sizeof(request));

    memcpy(request.ifr_name, ifname, name_length + 1U);

    socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd < 0)
    {
        return -errno;
    }

    ret = ioctl(socket_fd, SIOCGIFADDR, &request);
    if (ret != 0)
    {
        ret = -errno;
        close(socket_fd);
        return ret;
    }

    socket_address = (const struct sockaddr_in *)&request.ifr_addr;
    if (socket_address->sin_family != AF_INET)
    {
        close(socket_fd);
        return -EAFNOSUPPORT;
    }

    *address = socket_address->sin_addr;

    close(socket_fd);

    return 0;
}

/**
 * @brief 获取网络接口IPv6地址。
 */
static int _network_interface_get_ipv6(const char *ifname, bool global_only, struct in6_addr *address)
{
    const struct sockaddr_in6 *socket_address;
    struct ifaddrs            *interfaces;
    struct ifaddrs            *current;
    int                        ret;

    if (!_network_interface_name_valid(ifname) || address == NULL)
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
 * 忽略未指定、回环、组播及链路本地IPv6地址。
 */
int  linkg_network_interface_get_ipv6(const char *ifname, struct in6_addr *address)
{
    return _network_interface_get_ipv6(ifname, false, address);
}

/**
 * @brief 获取网络接口当前公网Global IPv6地址。
 */
int  linkg_network_interface_get_global_ipv6(const char *ifname, struct in6_addr *address)
{
    return _network_interface_get_ipv6(ifname, true, address);
}

/**
 * @brief 设置网络接口IPv6 Router Advertisement接收模式。
 *
 * @note FORCE模式对应Linux accept_ra=2，在IPv6 forwarding开启时仍允许接口接收RA。
 */
int  linkg_network_interface_ipv6_accept_ra_set(const char *ifname, linkg_network_ipv6_accept_ra_t mode)
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
int  linkg_network_interface_ipv6_accept_ra_get(const char *ifname, linkg_network_ipv6_accept_ra_t *mode)
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

/****************************** 系统网络 ******************************/

/**
 * @brief 设置系统IPv4转发状态。
 */
int  linkg_network_ipv4_forwarding_set(bool enabled)
{
    const char value = enabled ? '1' : '0';

    return linkg_file_write_all(LINKG_NETWORK_IPV4_FORWARD_PATH, &value, sizeof(value));
}

/**
 * @brief 设置网络接口MAC地址。
 */
int  linkg_network_interface_set_mac(const char *ifname, const uint8_t mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH])
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
