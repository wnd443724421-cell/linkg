/**
 * @file linkg_fast_nat_rules_common.c
 * @brief NAT与Direct共享的报文处理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"
#include "linkg_fast_nat_utils.h"

#include <linux/errno.h>
#include <linux/inetdevice.h>
#include <linux/netdevice.h>
#include <linux/rtnetlink.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <net/ip.h>

/**
 * @brief 为协议派生数据流预建客户端侧Ethernet主动Flow。
 *
 * server_virtual_ip/server_virtual_id描述远端业务服务端Virtual Endpoint，
 * client_virtual_ip/client_virtual_id描述本节点客户端Virtual Endpoint。
 * 预建Flow后，派生数据到达本节点Ethernet出口时会保留远端Virtual源地址。
 */
int linkg_fast_nat_related_flow_create(__be32 server_virtual_ip, __be16 server_virtual_id, __be32 client_virtual_ip, __be16 client_virtual_id, __u8 protocol)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    linkg_fast_nat_flow_key_t      key;
    __be32                         client_real_ip;

    if (server_virtual_ip == 0 || client_virtual_ip == 0 || server_virtual_id == 0 || client_virtual_id == 0)
    {
        return -EINVAL;
    }

    if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
    {
        return -EPROTONOSUPPORT;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(server_virtual_ip, &config->virtual_network))
    {
        return -EINVAL;
    }

    if (!linkg_fast_nat_local_virtual_endpoint(config, client_virtual_ip))
    {
        return -ENOENT;
    }

    client_real_ip = linkg_fast_nat_ipv4_prefix_map(client_virtual_ip,
                                                      &config->local_virtual_subnet,
                                                      &config->ethernet_network);
    if (!linkg_fast_nat_ipv4_host_usable(client_real_ip, &config->ethernet_network))
    {
        return -EINVAL;
    }

    memset(&key, 0, sizeof(key));

    key.ethernet_ip = client_real_ip;
    key.virtual_ip  = server_virtual_ip;
    key.ethernet_id = client_virtual_id;
    key.virtual_id  = server_virtual_id;
    key.protocol    = protocol;

    return linkg_fast_nat_flow_get_or_create(&key);
}

/**
 * @brief 获取指定网络接口当前可用的IPv4地址。
 */
static int _linkg_fast_nat_interface_ipv4_get(const struct net_device *device, __be32 *address)
{
    struct in_device *in_device;
    __be32            selected_address;

    if (device == NULL || address == NULL)
    {
        return -EINVAL;
    }

    selected_address = 0;

    rcu_read_lock();

    in_device = __in_dev_get_rcu(device);
    if (in_device != NULL)
    {
        selected_address = inet_confirm_addr(dev_net(device), in_device, 0, 0, RT_SCOPE_UNIVERSE);
    }

    rcu_read_unlock();

    if (selected_address == 0)
    {
        return -EADDRNOTAVAIL;
    }

    *address = selected_address;

    return 0;
}

/**
 * @brief 判断IPv4分片是否来自需要执行Uplink SNAT的Ethernet Host。
 */
bool linkg_fast_nat_uplink_fragment_requires_snat(struct sk_buff *skb)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    struct iphdr                  *iph;

    if (skb == NULL || !pskb_may_pull(skb, sizeof(struct iphdr)))
    {
        return false;
    }

    iph = ip_hdr(skb);
    if (iph == NULL || iph->version != 4 || iph->ihl < 5 || !ip_is_fragment(iph))
    {
        return false;
    }

    return linkg_fast_nat_ipv4_host_usable(iph->saddr, &config->ethernet_network) &&
           iph->saddr != config->ethernet_ip;
}

/**
 * @brief 恢复Ethernet入口的Fast NAT SNAT返回报文。
 *
 * 仅处理目的IPv4为本节点Ethernet地址、且目的ID落在SNAT translated ID范围内的报文。
 * 验证translated ID和远端端点后恢复原始目的IPv4和目的ID。
 * Hairpin和LOCAL_OUT Mapping额外将回包源地址从真实Ethernet地址映射回对应Virtual地址。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_ethernet_rx_reverse_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    unsigned int                   transport_offset;
    __be32                         original_ip;
    __be16                         source_id;
    __be16                         destination_id;
    __be16                         original_id;
    __u8                           snat_type;
    int                            ret;

    (void)state;

    if (iph->daddr != config->ethernet_ip)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &destination_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    if (!linkg_fast_nat_mapping_id_in_range(config->snat_port_start, destination_id))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    iph = ip_hdr(skb);

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &source_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    iph = ip_hdr(skb);

    ret = linkg_fast_nat_mapping_reverse_lookup(config->snat_port_start, destination_id, iph->protocol, iph->saddr, source_id, &original_ip, &original_id, &snat_type);
    if (ret == -ENOENT)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    if (snat_type == LINKG_FAST_NAT_SNAT_UPLINK)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (snat_type == LINKG_FAST_NAT_SNAT_HAIRPIN || snat_type == LINKG_FAST_NAT_SNAT_LOCAL)
    {
        ret = linkg_fast_nat_source_netmap(skb, &config->ethernet_network, &config->local_virtual_subnet);
        if (ret != 0)
        {
            return LINKG_FAST_NAT_RULE_DROP;
        }

        iph = ip_hdr(skb);
    }

    ret = linkg_fast_nat_replace_tuple(skb, false, original_ip, original_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    // Virtual客户端的RTSP响应已经恢复原始目的地址，此时可解析SETUP协商出的RTP/RTCP端口。
    if (snat_type == LINKG_FAST_NAT_SNAT_VIRTUAL)
    {
        iph = ip_hdr(skb);

        if (iph->protocol == IPPROTO_TCP)
        {
            linkg_fast_nat_rtsp_observe_ethernet_rx(skb);
        }
    }

    return LINKG_FAST_NAT_RULE_STOP;
}

/**
 * @brief 匹配Ethernet入口的协议派生数据流并恢复远端Virtual目标。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_ethernet_rx_related(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    int                            ret;

    (void)state;
    (void)iph;

    ret = linkg_fast_nat_related_translate_ethernet_rx(skb, config->ethernet_ip);
    if (ret == -ENOENT || ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    /**
     * Related命中后Destination已经恢复为远端Virtual地址。
     * PRE_ROUTING后继续正常路由到TUN，由当前模式的TUN TX规则处理源地址。
     * 不继续执行record_flow，避免把服务器主动派生流错误登记为普通Ethernet主动Flow。
     */
    return LINKG_FAST_NAT_RULE_STOP;
}

/**
 * @brief 观察TUN入口控制协议并预建协议派生数据流状态。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_tun_rx_protocol_helper(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    (void)state;

    if (iph->protocol != IPPROTO_TCP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    linkg_fast_nat_rtsp_observe_tun_rx(skb);

    return LINKG_FAST_NAT_RULE_NEXT;
}

/**
 * @brief 恢复Uplink入口的Internet SNAT返回报文。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_uplink_rx_reverse_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    unsigned int                   transport_offset;
    __be32                         uplink_ip;
    __be32                         original_ip;
    __be16                         source_id;
    __be16                         destination_id;
    __be16                         original_id;
    __u8                           snat_type;
    int                            ret;

    if (config->uplink_ifindex <= 0 || state->in == NULL ||
        state->in->ifindex != config->uplink_ifindex)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    ret = _linkg_fast_nat_interface_ipv4_get(state->in, &uplink_ip);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (iph->daddr != uplink_ip)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &destination_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    if (!linkg_fast_nat_mapping_id_in_range(config->snat_port_start, destination_id))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    iph = ip_hdr(skb);

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &source_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    iph = ip_hdr(skb);

    ret = linkg_fast_nat_mapping_reverse_lookup(config->snat_port_start, destination_id, iph->protocol, iph->saddr, source_id, &original_ip, &original_id, &snat_type);
    if (ret == -ENOENT)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    if (snat_type != LINKG_FAST_NAT_SNAT_UPLINK)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (!linkg_fast_nat_ipv4_host_usable(original_ip, &config->ethernet_network) || original_ip == config->ethernet_ip)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_replace_tuple(skb, false, original_ip, original_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_STOP;
}

/**
 * @brief 限制进入LinkG网络的TCP握手报文MSS，避免LinkG传输层对TCP数据再次分片。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_tun_tx_tcp_mss_clamp(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    struct tcphdr *tcph;
    unsigned int   transport_offset;
    int            ret;

    (void)state;

    if (iph->protocol != IPPROTO_TCP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    if (!pskb_may_pull(skb, transport_offset + sizeof(struct tcphdr)))
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    iph  = ip_hdr(skb);
    tcph = (struct tcphdr *)(skb_network_header(skb) + transport_offset);

    if (!tcph->syn)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    ret = linkg_fast_nat_tcp_mss_clamp(skb, LINKG_FAST_NAT_TCP_MSS_MAX);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    return LINKG_FAST_NAT_RULE_NEXT;
}

/**
 * @brief 将Ethernet Host通过外部上行发送的报文执行Source NAT。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_rule_uplink_tx_source_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_config_t *config = linkg_fast_nat_get_config();
    linkg_fast_nat_mapping_key_t   mapping_key;
    unsigned int                   transport_offset;
    __be32                         uplink_ip;
    __be16                         source_id;
    __be16                         destination_id;
    int                            ret;

    if (config->uplink_ifindex <= 0 || state->out == NULL ||
        state->out->ifindex != config->uplink_ifindex)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (!linkg_fast_nat_ipv4_host_usable(iph->saddr, &config->ethernet_network) || iph->saddr == config->ethernet_ip)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &source_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    iph = ip_hdr(skb);

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &destination_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    iph = ip_hdr(skb);

    ret = _linkg_fast_nat_interface_ipv4_get(state->out, &uplink_ip);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    memset(&mapping_key, 0, sizeof(mapping_key));

    mapping_key.original_ip = iph->saddr;
    mapping_key.remote_ip   = iph->daddr;
    mapping_key.original_id = source_id;
    mapping_key.remote_id   = destination_id;
    mapping_key.protocol    = iph->protocol;

    return linkg_fast_nat_apply_snat(skb, &mapping_key, uplink_ip, config->snat_port_start, LINKG_FAST_NAT_SNAT_UPLINK);
}

/**
 * @brief 记录Ethernet主动访问Virtual网络的原始流。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_record_flow(struct sk_buff *skb, struct iphdr *iph)
{
    linkg_fast_nat_flow_key_t key;
    unsigned int              transport_offset;
    int                       ret;

    memset(&key, 0, sizeof(key));

    key.ethernet_ip = iph->saddr;
    key.virtual_ip  = iph->daddr;
    key.protocol    = iph->protocol;

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &key.ethernet_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    iph = ip_hdr(skb);

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &key.virtual_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_NEXT;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_flow_get_or_create(&key);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_NEXT;
}

/**
 * @brief 读取Ethernet出口报文的原始SNAT五元组。
 */
int linkg_fast_nat_snat_key_get(struct sk_buff *skb, struct iphdr *iph, linkg_fast_nat_mapping_key_t *key)
{
    unsigned int transport_offset;
    int          ret;

    memset(key, 0, sizeof(*key));

    key->original_ip = iph->saddr;
    key->remote_ip   = iph->daddr;
    key->protocol    = iph->protocol;
    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &key->original_id);
    if (ret != 0)
    {
        return ret;
    }

    return linkg_fast_nat_get_transport_id(skb, ip_hdr(skb), transport_offset, false, &key->remote_id);
}

/**
 * @brief 查询当前报文是否是Ethernet主动流的远端回包。
 */
int linkg_fast_nat_ethernet_tx_lookup_flow(const linkg_fast_nat_mapping_key_t *key)
{
    linkg_fast_nat_flow_key_t flow_key;

    memset(&flow_key, 0, sizeof(flow_key));

    flow_key.ethernet_ip = key->remote_ip;
    flow_key.virtual_ip  = key->original_ip;
    flow_key.ethernet_id = key->remote_id;
    flow_key.virtual_id  = key->original_id;
    flow_key.protocol   = key->protocol;

    return linkg_fast_nat_flow_lookup(&flow_key);
}

/**
 * @brief 使用指定出口地址执行SNAT并清理内部标记。
 */
linkg_fast_nat_rule_result_t linkg_fast_nat_apply_snat(struct sk_buff *skb, const linkg_fast_nat_mapping_key_t *key, __be32 translated_ip, __u16 port_start, __u8 snat_type)
{
    __be16 translated_id;
    int    ret;

    ret = linkg_fast_nat_mapping_get_or_create(key, snat_type, port_start, &translated_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_replace_tuple(skb, true, translated_ip, translated_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    if (snat_type == LINKG_FAST_NAT_SNAT_HAIRPIN || snat_type == LINKG_FAST_NAT_SNAT_LOCAL)
    {
        skb->mark &= ~LINKG_FAST_NAT_MARK_TYPE_MASK;
    }

    return LINKG_FAST_NAT_RULE_STOP;
}
