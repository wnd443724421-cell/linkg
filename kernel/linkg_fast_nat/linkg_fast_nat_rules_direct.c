/**
 * @file linkg_fast_nat_rules_direct.c
 * @brief Direct模式规则
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"
#include "linkg_fast_nat_utils.h"

#include <linux/errno.h>
#include <linux/kernel.h>

/**
 * @brief 仅记录本地Ethernet设备主动访问远端Virtual的流。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_ethernet_rx_record_flow(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();

    (void)state;

    if (!linkg_fast_nat_ipv4_host_usable(iph->saddr, &config->ethernet_network) ||
        !linkg_fast_nat_ipv4_in_subnet(iph->daddr, &config->virtual_network) ||
        linkg_fast_nat_ipv4_in_subnet(iph->daddr, &config->local_virtual_subnet))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    return linkg_fast_nat_record_flow(skb, iph);
}

/**
 * @brief 保持本地通信直达，对远端访问本地设备按流方向执行SNAT。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_ethernet_tx_source_translate(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    linkg_fast_nat_mapping_key_t   key;
    __u8                           snat_type;
    int                            ret;

    (void)state;

    if (!linkg_fast_nat_ipv4_host_usable(iph->daddr, &config->ethernet_network) ||
        linkg_fast_nat_ipv4_in_subnet(iph->saddr, &config->local_virtual_subnet))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (linkg_fast_nat_ipv4_in_subnet(iph->saddr, &config->virtual_network))
    {
        snat_type = LINKG_FAST_NAT_SNAT_VIRTUAL;
    }
    else if (linkg_fast_nat_ipv4_in_subnet(iph->saddr, &config->tun_network))
    {
        snat_type = LINKG_FAST_NAT_SNAT_TUN;
    }
    else
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

    if (snat_type == LINKG_FAST_NAT_SNAT_VIRTUAL)
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
    }

    return linkg_fast_nat_apply_snat(skb, &key, config->ethernet_ip, config->snat_port_start, snat_type);
}

/****************************** Fast NAT规则表 ******************************/

static const linkg_fast_nat_rule_func_t g_ethernet_rx_rules[] =
{
    linkg_fast_nat_rule_ethernet_rx_reverse_snat,
    linkg_fast_nat_rule_ethernet_rx_related,
    _linkg_fast_nat_ethernet_rx_record_flow,
};

static const linkg_fast_nat_rule_func_t g_tun_rx_rules[] =
{
    linkg_fast_nat_rule_tun_rx_protocol_helper,
};

static const linkg_fast_nat_rule_func_t g_uplink_rx_rules[] =
{
    linkg_fast_nat_rule_uplink_rx_reverse_snat,
};

static const linkg_fast_nat_rule_func_t g_ethernet_tx_rules[] =
{
    _linkg_fast_nat_ethernet_tx_source_translate,
};

static const linkg_fast_nat_rule_func_t g_tun_tx_rules[] =
{
    linkg_fast_nat_rule_tun_tx_tcp_mss_clamp,
};

static const linkg_fast_nat_rule_func_t g_uplink_tx_rules[] =
{
    linkg_fast_nat_rule_uplink_tx_source_snat,
};

static const linkg_fast_nat_rule_set_t g_direct_rules =
{
    .paths =
    {
        [LINKG_FAST_NAT_PATH_ETHERNET_RX] = { g_ethernet_rx_rules, ARRAY_SIZE(g_ethernet_rx_rules) },
        [LINKG_FAST_NAT_PATH_TUN_RX    ] = { g_tun_rx_rules, ARRAY_SIZE(g_tun_rx_rules) },
        [LINKG_FAST_NAT_PATH_UPLINK_RX ] = { g_uplink_rx_rules, ARRAY_SIZE(g_uplink_rx_rules) },
        [LINKG_FAST_NAT_PATH_LOCAL_OUT ] = { NULL, 0U },
        [LINKG_FAST_NAT_PATH_ETHERNET_TX] = { g_ethernet_tx_rules, ARRAY_SIZE(g_ethernet_tx_rules) },
        [LINKG_FAST_NAT_PATH_TUN_TX    ] = { g_tun_tx_rules, ARRAY_SIZE(g_tun_tx_rules) },
        [LINKG_FAST_NAT_PATH_UPLINK_TX ] = { g_uplink_tx_rules, ARRAY_SIZE(g_uplink_tx_rules) },
    }
};

/**
 * @brief 获取Direct模式规则集。
 */
const linkg_fast_nat_rule_set_t *linkg_fast_nat_rules_direct_get(void)
{
    return &g_direct_rules;
}
