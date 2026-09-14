/**
 * @file linkg_fast_nat_internal.h
 * @brief LinkG Fast NAT内部接口
 */

#ifndef LINKG_FAST_NAT_INTERNAL_H
#define LINKG_FAST_NAT_INTERNAL_H

#include "linkg_fast_nat_uapi.h"

#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/types.h>

/****************************** 状态定义 ******************************/

#define LINKG_FAST_NAT_STATE_UNCONFIGURED 0U // 尚未配置
#define LINKG_FAST_NAT_STATE_CONFIGURED   1U // 已配置但未启动
#define LINKG_FAST_NAT_STATE_RUNNING      2U // Fast NAT正在运行

/****************************** IPv4辅助 ******************************/

bool   linkg_fast_nat_ipv4_in_subnet(__be32 address, const linkg_fast_nat_ipv4_subnet_t *subnet);
bool   linkg_fast_nat_ipv4_subnet_normalized(const linkg_fast_nat_ipv4_subnet_t *subnet);
bool   linkg_fast_nat_ipv4_netmask_valid(__be32 netmask);
bool   linkg_fast_nat_ipv4_subnet_contains(const linkg_fast_nat_ipv4_subnet_t *outer, const linkg_fast_nat_ipv4_subnet_t *inner);
bool   linkg_fast_nat_ipv4_subnet_overlap(const linkg_fast_nat_ipv4_subnet_t *left, const linkg_fast_nat_ipv4_subnet_t *right);
__be32 linkg_fast_nat_ipv4_prefix_map(__be32 address, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to);

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

/****************************** Related流 ******************************/

void linkg_fast_nat_related_init(void);
void linkg_fast_nat_related_deinit(void);
void linkg_fast_nat_related_start(void);
void linkg_fast_nat_related_stop(void);
int  linkg_fast_nat_related_add(__be32 local_real_ip, __be16 local_real_id, __be16 gateway_id, __be32 remote_virtual_ip, __be16 remote_virtual_id, __u8 protocol);
int  linkg_fast_nat_related_translate_ethernet_rx(struct sk_buff *skb, __be32 gateway_ip);
int  linkg_fast_nat_related_flow_create(__be32 server_virtual_ip, __be16 server_virtual_id, __be32 client_virtual_ip, __be16 client_virtual_id, __u8 protocol);

/****************************** RTSP协议扩展 ******************************/

void linkg_fast_nat_rtsp_observe_ethernet_rx(struct sk_buff *skb);
void linkg_fast_nat_rtsp_observe_tun_rx(struct sk_buff *skb);

#endif
