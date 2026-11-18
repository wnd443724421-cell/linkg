/**
 * @file linkg_fast_nat_internal.h
 * @brief LinkG Fast NAT内部类型及模块协作接口
 */

#ifndef LINKG_FAST_NAT_INTERNAL_H
#define LINKG_FAST_NAT_INTERNAL_H

#include "linkg_fast_nat_uapi.h"

#include <linux/ip.h>
#include <linux/netfilter.h>
#include <linux/skbuff.h>
#include <linux/types.h>

/****************************** 公共常量 ******************************/

#define LINKG_FAST_NAT_SLOT_COUNT      4096U       // SNAT Mapping槽位数量，同时用于端口范围校验
#define LINKG_FAST_NAT_TCP_MSS_MAX     1388U       // LinkG隧道下TCP MSS上限
#define LINKG_FAST_NAT_MARK_TYPE_MASK  0xC0000000U // Fast NAT占用的skb mark类型掩码
#define LINKG_FAST_NAT_MARK_HAIRPIN    0x80000000U // Ethernet Hairpin产生的报文
#define LINKG_FAST_NAT_MARK_LOCAL      0x40000000U // LOCAL_OUT目标转换产生的报文

/****************************** SNAT类型 ******************************/

#define LINKG_FAST_NAT_SNAT_VIRTUAL    1U // 远端Virtual源访问本节点Ethernet
#define LINKG_FAST_NAT_SNAT_TUN        2U // TUN节点地址访问本节点Ethernet
#define LINKG_FAST_NAT_SNAT_HAIRPIN    3U // Ethernet本地Hairpin访问
#define LINKG_FAST_NAT_SNAT_LOCAL      4U // 本机访问本节点Virtual Endpoint
#define LINKG_FAST_NAT_SNAT_UPLINK     5U // Ethernet业务设备通过外部上行访问网络

/****************************** 工作模式 ******************************/

typedef enum
{
    LINKG_FAST_NAT_MODE_NAT = 0, // Ethernet与Virtual子网不重叠，执行原有NETMAP模式
    LINKG_FAST_NAT_MODE_DIRECT   // Ethernet与本节点Virtual子网相同，使用直连模式
} linkg_fast_nat_mode_t;

/****************************** 规则定义 ******************************/

typedef enum
{
    LINKG_FAST_NAT_RULE_NEXT = 0, // 继续执行同一路径的下一条规则
    LINKG_FAST_NAT_RULE_STOP,     // 当前路径处理完成并放行
    LINKG_FAST_NAT_RULE_DROP      // 当前报文处理失败并丢弃
} linkg_fast_nat_rule_result_t;

typedef linkg_fast_nat_rule_result_t (*linkg_fast_nat_rule_func_t)(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);

typedef enum
{
    LINKG_FAST_NAT_PATH_ETHERNET_RX = 0, // Ethernet PRE_ROUTING
    LINKG_FAST_NAT_PATH_TUN_RX,          // TUN PRE_ROUTING
    LINKG_FAST_NAT_PATH_UPLINK_RX,       // Uplink PRE_ROUTING
    LINKG_FAST_NAT_PATH_LOCAL_OUT,       // 本机 LOCAL_OUT
    LINKG_FAST_NAT_PATH_ETHERNET_TX,     // Ethernet POST_ROUTING
    LINKG_FAST_NAT_PATH_TUN_TX,          // TUN POST_ROUTING
    LINKG_FAST_NAT_PATH_UPLINK_TX,       // Uplink POST_ROUTING
    LINKG_FAST_NAT_PATH_COUNT            // 路径数量
} linkg_fast_nat_path_t;

typedef struct
{
    const linkg_fast_nat_rule_func_t *rules; // 当前路径的只读规则函数表，不持有所有权
    __u32                             count; // 当前路径的规则数量，允许为0
} linkg_fast_nat_rule_chain_t;

typedef struct
{
    linkg_fast_nat_rule_chain_t paths[LINKG_FAST_NAT_PATH_COUNT]; // 七条Netfilter处理路径的规则链
} linkg_fast_nat_rule_set_t;

/****************************** Mapping定义 ******************************/

typedef struct
{
    __be32 original_ip; // SNAT前源IPv4地址
    __be32 remote_ip;   // SNAT前目的IPv4地址
    __be16 original_id; // SNAT前源TCP/UDP端口或ICMP Echo Identifier
    __be16 remote_id;   // SNAT前目的TCP/UDP端口或ICMP Echo Identifier
    __u8   protocol;    // IPv4上层协议
} linkg_fast_nat_mapping_key_t;

/****************************** Flow定义 ******************************/

typedef struct
{
    __be32 ethernet_ip; // Ethernet侧IPv4地址
    __be32 virtual_ip;  // Virtual侧IPv4地址
    __be16 ethernet_id; // Ethernet侧TCP/UDP端口或ICMP Echo Identifier
    __be16 virtual_id;  // Virtual侧TCP/UDP端口或ICMP Echo Identifier
    __u8   protocol;    // IPv4上层协议
} linkg_fast_nat_flow_key_t;

/****************************** 配置及运行状态 ******************************/

int                            linkg_fast_nat_config_validate(const linkg_fast_nat_config_t *config, linkg_fast_nat_mode_t *mode);
const linkg_fast_nat_config_t *linkg_fast_nat_get_config(void); // 仅供已进入RUNNING的数据路径使用，等待在途Hook退出后才能重配。
bool                           linkg_fast_nat_local_virtual_endpoint(const linkg_fast_nat_config_t *config, __be32 address);

/****************************** Mapping接口 ******************************/

// init在加载时调用，reset要求Hook及GC均已停止。
void linkg_fast_nat_mapping_init(void);
void linkg_fast_nat_mapping_reset(void);
void linkg_fast_nat_mapping_gc_start(void);
void linkg_fast_nat_mapping_gc_stop(void);
bool linkg_fast_nat_mapping_id_in_range(__u16 port_start, __be16 translated_id);
int  linkg_fast_nat_mapping_get_or_create(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u16 port_start, __be16 *translated_id);
int  linkg_fast_nat_mapping_reverse_lookup(__u16 port_start, __be16 translated_id, __u8 protocol, __be32 remote_ip, __be16 remote_id, __be32 *original_ip, __be16 *original_id, __u8 *snat_type);

/****************************** Flow接口 ******************************/

// init在加载时调用，reset要求Hook及GC均已停止。
void linkg_fast_nat_flow_init(void);
void linkg_fast_nat_flow_reset(void);
void linkg_fast_nat_flow_gc_start(void);
void linkg_fast_nat_flow_gc_stop(void);
int  linkg_fast_nat_flow_lookup(const linkg_fast_nat_flow_key_t *key);
int  linkg_fast_nat_flow_get_or_create(const linkg_fast_nat_flow_key_t *key);

/****************************** Related接口 ******************************/

void linkg_fast_nat_related_init(void);
void linkg_fast_nat_related_deinit(void);
void linkg_fast_nat_related_start(void);
void linkg_fast_nat_related_stop(void);
int  linkg_fast_nat_related_add(__be32 local_device_ip, __be16 local_device_port, __be16 gateway_port, __be32 remote_virtual_ip, __be16 remote_virtual_port, __u8 protocol);
int  linkg_fast_nat_related_translate_ethernet_rx(struct sk_buff *skb, __be32 gateway_ip);

/****************************** RTSP接口 ******************************/

void linkg_fast_nat_rtsp_observe_ethernet_rx(struct sk_buff *skb);
void linkg_fast_nat_rtsp_observe_tun_rx(struct sk_buff *skb);

/****************************** 公共规则接口 ******************************/

linkg_fast_nat_rule_result_t linkg_fast_nat_rule_ethernet_rx_reverse_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_ethernet_rx_related(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_tun_rx_protocol_helper(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_tun_tx_tcp_mss_clamp(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_uplink_rx_reverse_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_uplink_tx_source_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
bool                         linkg_fast_nat_uplink_fragment_requires_snat(struct sk_buff *skb);

/****************************** 公共动作接口 ******************************/

linkg_fast_nat_rule_result_t linkg_fast_nat_record_flow(struct sk_buff *skb, struct iphdr *iph);
linkg_fast_nat_rule_result_t linkg_fast_nat_apply_snat(struct sk_buff *skb, const linkg_fast_nat_mapping_key_t *key, __be32 translated_ip, __u16 port_start, __u8 snat_type);
int                         linkg_fast_nat_snat_key_get(struct sk_buff *skb, struct iphdr *iph, linkg_fast_nat_mapping_key_t *key);
int                         linkg_fast_nat_ethernet_tx_lookup_flow(const linkg_fast_nat_mapping_key_t *key);
int                         linkg_fast_nat_related_flow_create(__be32 server_virtual_ip, __be16 server_virtual_id, __be32 client_virtual_ip, __be16 client_virtual_id, __u8 protocol);

/****************************** 模式规则集 ******************************/

const linkg_fast_nat_rule_set_t *linkg_fast_nat_rules_nat_get(void);
const linkg_fast_nat_rule_set_t *linkg_fast_nat_rules_direct_get(void);

#endif // LINKG_FAST_NAT_INTERNAL_H
