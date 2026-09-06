/**
 * @file linkg_fast_nat_utils.c
 * @brief LinkG Fast NAT公共辅助实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-05
 */

#include "linkg_fast_nat_internal.h"

#include <linux/errno.h>
#include <linux/icmp.h>
#include <linux/ip.h>
#include <linux/skbuff.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <linux/udp.h>
#include <net/ip.h>

#include <net/checksum.h>
#include <net/netfilter/nf_conntrack.h>

/****************************** 内部类型 ******************************/

/**
 * @brief TCP/UDP公共端口头。
 */
typedef struct
{
    __be16 source; // 源端口
    __be16 dest;   // 目的端口
} linkg_fast_nat_ports_t;

/****************************** IPv4辅助 ******************************/

/**
 * @brief 判断IPv4地址是否属于指定子网。
 */
bool linkg_fast_nat_ipv4_in_subnet(__be32 address, const linkg_fast_nat_ipv4_subnet_t *subnet)
{
    return (address & subnet->netmask) == subnet->network;
}

/**
 * @brief 判断IPv4子网是否已经规范化。
 */
bool linkg_fast_nat_ipv4_subnet_normalized(const linkg_fast_nat_ipv4_subnet_t *subnet)
{
    if (subnet == NULL)
    {
        return false;
    }

    return (subnet->network & subnet->netmask) == subnet->network;
}

/**
 * @brief 判断IPv4掩码是否合法且连续。
 */
bool linkg_fast_nat_ipv4_netmask_valid(__be32 netmask)
{
    __u32 mask;
    bool  zero_seen;
    int   bit;

    mask      = ntohl(netmask);
    zero_seen = false;

    for (bit = 31; bit >= 0; bit--)
    {
        if ((mask & (1U << bit)) != 0U)
        {
            if (zero_seen)
            {
                return false;
            }

            continue;
        }

        zero_seen = true;
    }

    return true;
}

/**
 * @brief 按目标子网替换IPv4网络前缀并保留Host部分。
 *
 * 调用前要求from和to使用相同netmask。
 */
__be32 linkg_fast_nat_ipv4_prefix_map(__be32 address, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to)
{
    return to->network | (address & ~from->netmask);
}

/****************************** skb辅助 ******************************/

/**
 * @brief 获取IPv4头并校验Fast NAT支持的数据包形态。
 */
int linkg_fast_nat_get_ipv4(struct sk_buff *skb, struct iphdr **iph, unsigned int *transport_offset)
{
    struct iphdr *header;
    unsigned int  offset;

    if (skb == NULL || iph == NULL || transport_offset == NULL)
    {
        return -EINVAL;
    }

    if (!pskb_may_pull(skb, sizeof(struct iphdr)))
    {
        return -EINVAL;
    }

    header = ip_hdr(skb);
    if (header == NULL || header->version != 4 || header->ihl < 5)
    {
        return -EINVAL;
    }

    offset = (unsigned int)header->ihl * 4U;
    if (!pskb_may_pull(skb, offset))
    {
        return -EINVAL;
    }

    header = ip_hdr(skb);
    if (ip_is_fragment(header))
    {
        return -EOPNOTSUPP;
    }

    *iph              = header;
    *transport_offset = offset;

    return 0;
}

/**
 * @brief 获取TCP/UDP端口或ICMP Echo Identifier。
 *
 * source为true时获取源端口，为false时获取目的端口。
 * ICMP Echo请求和响应均使用Echo Identifier。
 */
int linkg_fast_nat_get_transport_id(struct sk_buff *skb, const struct iphdr *iph, unsigned int transport_offset, bool source, __be16 *id)
{
    linkg_fast_nat_ports_t *ports;
    struct icmphdr         *icmph;

    if (skb == NULL || iph == NULL || id == NULL)
    {
        return -EINVAL;
    }

    switch (iph->protocol)
    {
        case IPPROTO_TCP:
        case IPPROTO_UDP:
            if (!pskb_may_pull(skb, transport_offset + sizeof(*ports)))
            {
                return -EINVAL;
            }

            ports = (linkg_fast_nat_ports_t *)(skb_network_header(skb) + transport_offset);
            *id   = source ? ports->source : ports->dest;
            return 0;

        case IPPROTO_ICMP:
            if (!pskb_may_pull(skb, transport_offset + sizeof(*icmph)))
            {
                return -EINVAL;
            }

            icmph = (struct icmphdr *)(skb_network_header(skb) + transport_offset);
            if (icmph->type != ICMP_ECHO && icmph->type != ICMP_ECHOREPLY)
            {
                return -EOPNOTSUPP;
            }

            *id = icmph->un.echo.id;
            return 0;

        default:
            return -EOPNOTSUPP;
    }
}

/**
 * @brief 将skb标记为不进入Linux通用conntrack。
 */
void linkg_fast_nat_mark_untracked(struct sk_buff *skb)
{
    if (skb == NULL)
    {
        return;
    }

    if (skb->_nfct == 0UL)
    {
        nf_ct_set(skb, NULL, IP_CT_UNTRACKED);
    }
}

/**
 * @brief 原地替换IPv4地址，并同步更新TCP/UDP伪首部校验和。
 */
int linkg_fast_nat_replace_ipv4(struct sk_buff *skb, bool source, __be32 new_ip)
{
    struct iphdr  *iph;
    struct tcphdr *tcph;
    struct udphdr *udph;
    unsigned int   transport_offset;
    unsigned int   write_length;
    __be32         old_ip;
    int            ret;

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    old_ip = source ? iph->saddr : iph->daddr;
    if (old_ip == new_ip)
    {
        return 0;
    }

    write_length = transport_offset;

    switch (iph->protocol)
    {
        case IPPROTO_TCP:
            write_length += sizeof(struct tcphdr);
            break;

        case IPPROTO_UDP:
            write_length += sizeof(struct udphdr);
            break;

        default:
            break;
    }

    if (!pskb_may_pull(skb, write_length) || skb_try_make_writable(skb, write_length))
    {
        return -ENOMEM;
    }

    iph = ip_hdr(skb);

    switch (iph->protocol)
    {
        case IPPROTO_TCP:
            tcph = (struct tcphdr *)(skb_network_header(skb) + transport_offset);
            inet_proto_csum_replace4(&tcph->check, skb, old_ip, new_ip, true);
            break;

        case IPPROTO_UDP:
            udph = (struct udphdr *)(skb_network_header(skb) + transport_offset);

            if (udph->check != 0 || skb->ip_summed == CHECKSUM_PARTIAL)
            {
                inet_proto_csum_replace4(&udph->check, skb, old_ip, new_ip, true);

                if (udph->check == 0)
                {
                    udph->check = CSUM_MANGLED_0;
                }
            }

            break;

        default:
            break;
    }

    csum_replace4(&iph->check, old_ip, new_ip);

    if (source)
    {
        iph->saddr = new_ip;
    }
    else
    {
        iph->daddr = new_ip;
    }

    return 0;
}

/**
 * @brief 原地替换TCP/UDP端口或ICMP Echo Identifier。
 */
int linkg_fast_nat_replace_transport_id(struct sk_buff *skb, bool source, __be16 new_id)
{
    struct iphdr           *iph;
    linkg_fast_nat_ports_t *ports;
    struct tcphdr          *tcph;
    struct udphdr          *udph;
    struct icmphdr         *icmph;
    unsigned int            transport_offset;
    unsigned int            write_length;
    __be16                  old_id;
    int                     ret;

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    switch (iph->protocol)
    {
        case IPPROTO_TCP:
            write_length = transport_offset + sizeof(struct tcphdr);
            break;

        case IPPROTO_UDP:
            write_length = transport_offset + sizeof(struct udphdr);
            break;

        case IPPROTO_ICMP:
            write_length = transport_offset + sizeof(struct icmphdr);
            break;

        default:
            return -EOPNOTSUPP;
    }

    if (!pskb_may_pull(skb, write_length) || skb_try_make_writable(skb, write_length))
    {
        return -ENOMEM;
    }

    iph = ip_hdr(skb);

    if (iph->protocol == IPPROTO_ICMP)
    {
        icmph = (struct icmphdr *)(skb_network_header(skb) + transport_offset);

        if (icmph->type != ICMP_ECHO && icmph->type != ICMP_ECHOREPLY)
        {
            return -EOPNOTSUPP;
        }

        old_id = icmph->un.echo.id;
        if (old_id == new_id)
        {
            return 0;
        }

        csum_replace2(&icmph->checksum, old_id, new_id);
        icmph->un.echo.id = new_id;

        return 0;
    }

    ports  = (linkg_fast_nat_ports_t *)(skb_network_header(skb) + transport_offset);
    old_id = source ? ports->source : ports->dest;

    if (old_id == new_id)
    {
        return 0;
    }

    if (iph->protocol == IPPROTO_TCP)
    {
        tcph = (struct tcphdr *)(skb_network_header(skb) + transport_offset);
        inet_proto_csum_replace2(&tcph->check, skb, old_id, new_id, true);
    }
    else
    {
        udph = (struct udphdr *)(skb_network_header(skb) + transport_offset);

        if (udph->check != 0 || skb->ip_summed == CHECKSUM_PARTIAL)
        {
            inet_proto_csum_replace2(&udph->check, skb, old_id, new_id, true);

            if (udph->check == 0)
            {
                udph->check = CSUM_MANGLED_0;
            }
        }
    }

    if (source)
    {
        ports->source = new_id;
    }
    else
    {
        ports->dest = new_id;
    }

    return 0;
}

/**
 * @brief 原地替换IPv4地址和TCP/UDP端口或ICMP Echo Identifier。
 */
int linkg_fast_nat_replace_tuple(struct sk_buff *skb, bool source, __be32 new_ip, __be16 new_id)
{
    int ret;

    ret = linkg_fast_nat_replace_transport_id(skb, source, new_id);
    if (ret != 0)
    {
        return ret;
    }

    return linkg_fast_nat_replace_ipv4(skb, source, new_ip);
}

/****************************** NAT基础动作 ******************************/

/**
 * @brief 执行无状态Destination前缀映射。
 */
int linkg_fast_nat_destination_netmap(struct sk_buff *skb, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be32        new_ip;
    int           ret;

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    new_ip = linkg_fast_nat_ipv4_prefix_map(iph->daddr, from, to);

    return linkg_fast_nat_replace_ipv4(skb, false, new_ip);
}

/**
 * @brief 执行无状态Source前缀映射。
 */
int linkg_fast_nat_source_netmap(struct sk_buff *skb, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be32        new_ip;
    int           ret;

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    new_ip = linkg_fast_nat_ipv4_prefix_map(iph->saddr, from, to);

    return linkg_fast_nat_replace_ipv4(skb, true, new_ip);
}
