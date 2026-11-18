/**
 * @file linkg_fast_nat_rules_nat.c
 * @brief 原有NAT模式规则
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"
#include "linkg_fast_nat_utils.h"

#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/rtnetlink.h>
#include <linux/netfilter_ipv4.h>

/**
 * @brief 记录Ethernet主动访问Virtual网络的原始流。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_ethernet_rx_record_flow(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();

    (void)state;

    if (!linkg_fast_nat_ipv4_host_usable(iph->saddr, &config->ethernet_network) ||
        !linkg_fast_nat_ipv4_in_subnet(iph->daddr, &config->virtual_network))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (linkg_fast_nat_ipv4_in_subnet(iph->daddr, &config->local_virtual_subnet) &&
        !linkg_fast_nat_ipv4_host_usable(iph->daddr, &config->local_virtual_subnet))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    return linkg_fast_nat_record_flow(skb, iph);
}

/**
 * @brief 处理Ethernet访问本地Virtual Endpoint的Hairpin Destination NETMAP。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_ethernet_rx_hairpin(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    unsigned int                   transport_offset;
    __be16                         transport_id;
    int                            ret;

    (void)state;

    if (!linkg_fast_nat_ipv4_host_usable(iph->saddr, &config->ethernet_network) ||
        !linkg_fast_nat_local_virtual_endpoint(config, iph->daddr))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &transport_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_destination_netmap(skb, &config->local_virtual_subnet, &config->ethernet_network);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    skb->mark = (skb->mark & ~LINKG_FAST_NAT_MARK_TYPE_MASK) | LINKG_FAST_NAT_MARK_HAIRPIN;

    return LINKG_FAST_NAT_RULE_STOP;
}

/**
 * @brief 处理TUN入口访问本地Ethernet网络的Destination NETMAP。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_tun_rx_destination_netmap(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    unsigned int                   transport_offset;
    __be16                         transport_id;
    int                            ret;

    (void)state;

    if (!linkg_fast_nat_ipv4_in_subnet(iph->saddr, &config->virtual_network) &&
        !linkg_fast_nat_ipv4_in_subnet(iph->saddr, &config->tun_network))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (!linkg_fast_nat_local_virtual_endpoint(config, iph->daddr))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &transport_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_destination_netmap(skb, &config->local_virtual_subnet, &config->ethernet_network);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_STOP;
}

/**
 * @brief 处理本机访问本节点Virtual Endpoint的Destination NETMAP。
 *
 * LOCAL_OUT发生在本机初始路由之后，修改目的IPv4地址后必须重新执行IPv4路由，
 * 并标记当前skb，使同一报文在Ethernet TX路径建立LOCAL类型SNAT Mapping。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_local_out_destination_netmap(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    unsigned int                   transport_offset;
    __be16                         transport_id;
    int                            ret;

    if (!linkg_fast_nat_local_virtual_endpoint(config, iph->daddr))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &transport_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_destination_netmap(skb, &config->local_virtual_subnet, &config->ethernet_network);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = ip_route_me_harder(state->net, skb, RTN_UNSPEC);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    skb->mark = (skb->mark & ~LINKG_FAST_NAT_MARK_TYPE_MASK) | LINKG_FAST_NAT_MARK_LOCAL;

    return LINKG_FAST_NAT_RULE_STOP;
}

/**
 * @brief 处理Ethernet出口的Fast NAT源地址转换。
 *
 * Hairpin和LOCAL_OUT通过skb mark选择专用SNAT类型。
 * Virtual源需要先查询Ethernet主动流表，命中表示当前报文属于返回方向并保留Virtual源地址；
 * 未命中时与TUN源统一建立SNAT Mapping并转换为本节点Ethernet地址和translated ID。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_ethernet_tx_source_translate(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    linkg_fast_nat_mapping_key_t   key;
    __u32                          mark_type;
    __u8                           snat_type;
    int                            ret;

    (void)state;

    if (!linkg_fast_nat_ipv4_host_usable(iph->daddr, &config->ethernet_network))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    ret = linkg_fast_nat_snat_key_get(skb, iph, &key);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    mark_type = skb->mark & LINKG_FAST_NAT_MARK_TYPE_MASK;

    if (mark_type == LINKG_FAST_NAT_MARK_HAIRPIN)
    {
        if (!linkg_fast_nat_ipv4_host_usable(key.original_ip, &config->ethernet_network))
        {
            return LINKG_FAST_NAT_RULE_NEXT;
        }

        snat_type = LINKG_FAST_NAT_SNAT_HAIRPIN;
    }
    else if (mark_type == LINKG_FAST_NAT_MARK_LOCAL)
    {
        snat_type = LINKG_FAST_NAT_SNAT_LOCAL;
    }
    else if (linkg_fast_nat_ipv4_in_subnet(key.original_ip, &config->virtual_network))
    {
        ret = linkg_fast_nat_ethernet_tx_lookup_flow(&key);
        if (ret == 0)
        {
            return LINKG_FAST_NAT_RULE_STOP;
        }

        if (ret != -ENOENT)
        {
            return LINKG_FAST_NAT_RULE_DROP;
        }

        snat_type = LINKG_FAST_NAT_SNAT_VIRTUAL;
    }
    else if (linkg_fast_nat_ipv4_in_subnet(key.original_ip, &config->tun_network))
    {
        snat_type = LINKG_FAST_NAT_SNAT_TUN;
    }
    else
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    return linkg_fast_nat_apply_snat(skb, &key, config->ethernet_ip, config->snat_port_start, snat_type);
}

/**
 * @brief 将进入LinkG网络的Ethernet源地址映射为对应Virtual地址。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_tun_tx_source_netmap(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    int                            ret;

    (void)state;

    if (!linkg_fast_nat_ipv4_host_usable(iph->saddr, &config->ethernet_network))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    ret = linkg_fast_nat_source_netmap(skb, &config->ethernet_network, &config->local_virtual_subnet);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_STOP;
}

/****************************** Fast NAT规则表 ******************************/

static const linkg_fast_nat_rule_func_t g_ethernet_rx_rules[] =
{
    linkg_fast_nat_rule_ethernet_rx_reverse_snat,
    linkg_fast_nat_rule_ethernet_rx_related,
    _linkg_fast_nat_ethernet_rx_record_flow,
    _linkg_fast_nat_ethernet_rx_hairpin,
};

static const linkg_fast_nat_rule_func_t g_tun_rx_rules[] =
{
    linkg_fast_nat_rule_tun_rx_protocol_helper,
    _linkg_fast_nat_tun_rx_destination_netmap,
};

static const linkg_fast_nat_rule_func_t g_uplink_rx_rules[] =
{
    linkg_fast_nat_rule_uplink_rx_reverse_snat,
};

static const linkg_fast_nat_rule_func_t g_local_out_rules[] =
{
    _linkg_fast_nat_local_out_destination_netmap,
};

static const linkg_fast_nat_rule_func_t g_ethernet_tx_rules[] =
{
    _linkg_fast_nat_ethernet_tx_source_translate,
};

static const linkg_fast_nat_rule_func_t g_tun_tx_rules[] =
{
    linkg_fast_nat_rule_tun_tx_tcp_mss_clamp,
    _linkg_fast_nat_tun_tx_source_netmap,
};

static const linkg_fast_nat_rule_func_t g_uplink_tx_rules[] =
{
    linkg_fast_nat_rule_uplink_tx_source_snat,
};

static const linkg_fast_nat_rule_set_t g_nat_rules =
{
    .paths =
    {
        [LINKG_FAST_NAT_PATH_ETHERNET_RX] = { g_ethernet_rx_rules, ARRAY_SIZE(g_ethernet_rx_rules) },
        [LINKG_FAST_NAT_PATH_TUN_RX    ] = { g_tun_rx_rules, ARRAY_SIZE(g_tun_rx_rules) },
        [LINKG_FAST_NAT_PATH_UPLINK_RX ] = { g_uplink_rx_rules, ARRAY_SIZE(g_uplink_rx_rules) },
        [LINKG_FAST_NAT_PATH_LOCAL_OUT ] = { g_local_out_rules, ARRAY_SIZE(g_local_out_rules) },
        [LINKG_FAST_NAT_PATH_ETHERNET_TX] = { g_ethernet_tx_rules, ARRAY_SIZE(g_ethernet_tx_rules) },
        [LINKG_FAST_NAT_PATH_TUN_TX    ] = { g_tun_tx_rules, ARRAY_SIZE(g_tun_tx_rules) },
        [LINKG_FAST_NAT_PATH_UPLINK_TX ] = { g_uplink_tx_rules, ARRAY_SIZE(g_uplink_tx_rules) },
    }
};

/**
 * @brief 获取NAT模式规则集。
 */
const linkg_fast_nat_rule_set_t *linkg_fast_nat_rules_nat_get(void)
{
    return &g_nat_rules;
}
