/**
 * @file linkg_fast_nat_utils.h
 * @brief LinkG Fast NAT公共工具接口
 */

#ifndef LINKG_FAST_NAT_UTILS_H
#define LINKG_FAST_NAT_UTILS_H

#include "linkg_fast_nat_uapi.h"

#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/types.h>

/****************************** IPv4辅助 ******************************/

bool   linkg_fast_nat_ipv4_in_subnet(__be32 address, const linkg_fast_nat_ipv4_subnet_t *subnet);
bool   linkg_fast_nat_ipv4_subnet_normalized(const linkg_fast_nat_ipv4_subnet_t *subnet);
bool   linkg_fast_nat_ipv4_netmask_valid(__be32 netmask);
bool   linkg_fast_nat_ipv4_subnet_contains(const linkg_fast_nat_ipv4_subnet_t *outer, const linkg_fast_nat_ipv4_subnet_t *inner);
bool   linkg_fast_nat_ipv4_subnet_overlap(const linkg_fast_nat_ipv4_subnet_t *left, const linkg_fast_nat_ipv4_subnet_t *right);
__be32 linkg_fast_nat_ipv4_prefix_map(__be32 address, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to);
bool   linkg_fast_nat_ipv4_subnet_equal(const linkg_fast_nat_ipv4_subnet_t *left, const linkg_fast_nat_ipv4_subnet_t *right);
bool   linkg_fast_nat_ipv4_host_usable(__be32 address, const linkg_fast_nat_ipv4_subnet_t *subnet);

/****************************** skb辅助 ******************************/

int  linkg_fast_nat_get_ipv4(struct sk_buff *skb, struct iphdr **iph, unsigned int *transport_offset);
int  linkg_fast_nat_get_transport_id(struct sk_buff *skb, const struct iphdr *iph, unsigned int transport_offset, bool source, __be16 *id);
void linkg_fast_nat_mark_untracked(struct sk_buff *skb);
int  linkg_fast_nat_replace_ipv4(struct sk_buff *skb, bool source, __be32 new_ip);
int  linkg_fast_nat_replace_transport_id(struct sk_buff *skb, bool source, __be16 new_id);
int  linkg_fast_nat_replace_tuple(struct sk_buff *skb, bool source, __be32 new_ip, __be16 new_id);
int  linkg_fast_nat_tcp_mss_clamp(struct sk_buff *skb, __u16 max_mss);

/****************************** NAT基础动作 ******************************/

int linkg_fast_nat_destination_netmap(struct sk_buff *skb, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to);
int linkg_fast_nat_source_netmap(struct sk_buff *skb, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to);

#endif // LINKG_FAST_NAT_UTILS_H
