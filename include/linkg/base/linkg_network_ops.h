/**
 * @file linkg_network_ops.h
 * @brief LinkG通用网络能力接口
 */

#ifndef LINKG_NETWORK_OPS_H
#define LINKG_NETWORK_OPS_H

#include <netinet/in.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 宏定义 ******************************/

#define LINKG_NETWORK_MAC_ADDRESS_LENGTH 6U // MAC地址长度

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_NETWORK_IPV6_ACCEPT_RA_DISABLED = 0, // 禁止接收IPv6 RA
    LINKG_NETWORK_IPV6_ACCEPT_RA_ENABLED,      // 主机模式接收IPv6 RA
    LINKG_NETWORK_IPV6_ACCEPT_RA_FORCE         // 开启转发时仍强制接收IPv6 RA
} linkg_network_ipv6_accept_ra_t;              // IPv6 Router Advertisement接收模式

/****************************** IPv4转换 ******************************/

bool linkg_network_ipv4_from_string(const char *string, struct in_addr *out);
bool linkg_network_ipv4_to_string(const struct in_addr *address, char *out, size_t out_size);
bool linkg_network_ipv4_netmask_from_prefix(uint8_t prefix_length, struct in_addr *out);
bool linkg_network_ipv4_network_address(const struct in_addr *address, const struct in_addr *netmask, struct in_addr *network);

/****************************** 地址校验 ******************************/

bool linkg_network_ipv4_address_valid(const struct in_addr *address);
bool linkg_network_ipv4_netmask_valid(const struct in_addr *netmask);
bool linkg_network_ipv4_subnet_overlap(const struct in_addr *left_ip, const struct in_addr *left_netmask, const struct in_addr *right_ip, const struct in_addr *right_netmask);
bool linkg_network_ipv6_address_is_global(const struct in6_addr *address);

/****************************** 网络接口 ******************************/

bool linkg_network_interface_exists(const char *ifname);
int  linkg_network_interface_wait(const char *ifname, uint32_t timeout_ms);
int  linkg_network_interface_set_up(const char *ifname, bool up);
int  linkg_network_interface_set_mtu(const char *ifname, uint32_t mtu);
int  linkg_network_interface_set_mac(const char *ifname, const uint8_t mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH]);
int  linkg_network_interface_set_ipv4(const char *ifname, const struct in_addr *address, const struct in_addr *netmask);
int  linkg_network_interface_get_ipv4(const char *ifname, struct in_addr *address);
int  linkg_network_interface_get_ipv6(const char *ifname, struct in6_addr *address);
int  linkg_network_interface_get_global_ipv6(const char *ifname, struct in6_addr *address);
int  linkg_network_interface_ipv6_accept_ra_set(const char *ifname, linkg_network_ipv6_accept_ra_t mode);
int  linkg_network_interface_ipv6_accept_ra_get(const char *ifname, linkg_network_ipv6_accept_ra_t *mode);

/****************************** 系统网络 ******************************/

int linkg_network_ipv4_forwarding_set(bool enabled);

#ifdef __cplusplus
}
#endif

#endif // LINKG_NETWORK_OPS_H
