/**
 * @file linkg_fast_nat.c
 * @brief LinkG Fast NAT内核模块实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-01
 */

#include "linkg_fast_nat_uapi.h"

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/icmp.h>
#include <linux/if.h>
#include <linux/ip.h>
#include <linux/jhash.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netfilter.h>
#include <linux/netfilter_ipv4.h>
#include <linux/rtnetlink.h>
#include <linux/skbuff.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/tcp.h>
#include <linux/types.h>
#include <linux/uaccess.h>
#include <linux/udp.h>

#include <net/checksum.h>
#include <net/ip.h>
#include <net/net_namespace.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/route.h>

/****************************** 模块常量 ******************************/

#define LINKG_FAST_NAT_SLOT_COUNT           4096U                                                       // SNAT Hash槽位数量，必须为2的幂
#define LINKG_FAST_NAT_SLOT_MASK            (LINKG_FAST_NAT_SLOT_COUNT - 1U)                            // SNAT Hash槽位掩码
#define LINKG_FAST_NAT_ACTIVE_MAX           2000U                                                       // 最大活动SNAT映射数量
#define LINKG_FAST_NAT_HASH_SEED            0x4C4E4154U                                                 // "LNAT"固定Hash种子

#define LINKG_FAST_NAT_FLOW_SLOT_COUNT      4096U                                                       // Source NETMAP方向表槽位数量，必须为2的幂
#define LINKG_FAST_NAT_FLOW_SLOT_MASK       (LINKG_FAST_NAT_FLOW_SLOT_COUNT - 1U)                       // Source NETMAP方向表槽位掩码
#define LINKG_FAST_NAT_FLOW_ACTIVE_MAX      2000U                                                       // 最大活动Source NETMAP流数量
#define LINKG_FAST_NAT_FLOW_HASH_SEED       0x4C464C57U                                                 // "LFLW"固定Hash种子

#define LINKG_FAST_NAT_MARK_HAIRPIN         0x80000000U                                                 // 当前skb由本地Ethernet DNAT产生，需要Hairpin SNAT
#define LINKG_FAST_NAT_MARK_REVERSE         0x40000000U                                                 // 当前skb属于已有Fast NAT流的返回方向，禁止再次执行状态SNAT
#define LINKG_FAST_NAT_MARK_MASK            (LINKG_FAST_NAT_MARK_HAIRPIN | LINKG_FAST_NAT_MARK_REVERSE) // Fast NAT内部skb标记掩码

#define LINKG_FAST_NAT_PRIORITY_EARLY       (NF_IP_PRI_RAW + 50)                                        // conntrack之前执行
#define LINKG_FAST_NAT_PRIORITY_POSTROUTING (NF_IP_PRI_NAT_SRC - 10)                                    // 常规Source NAT之前执行

/****************************** SNAT定义 ******************************/

#define LINKG_FAST_NAT_ENTRY_FREE           0U                                                          // SNAT槽位空闲
#define LINKG_FAST_NAT_ENTRY_ACTIVE         1U                                                          // SNAT槽位有效

#define LINKG_FAST_NAT_FLOW_FREE            0U                                                          // Source NETMAP方向槽位空闲
#define LINKG_FAST_NAT_FLOW_ACTIVE          1U                                                          // Source NETMAP方向槽位有效

#define LINKG_FAST_NAT_SNAT_VIRTUAL         1U                                                          // 虚拟Endpoint访问Ethernet
#define LINKG_FAST_NAT_SNAT_TUN             2U                                                          // TUN节点访问Ethernet
#define LINKG_FAST_NAT_SNAT_HAIRPIN         3U                                                          // Ethernet Hairpin访问

/****************************** 内部类型 ******************************/

/**
 * @brief Fast NAT SNAT映射项。
 *
 * translated port不单独保存，其值固定为：
 * config.snat_port_start + 当前entry数组下标。
 *
 * 映射生命周期第一版仅在START到STOP之间有效，不执行运行期删除，
 * 因此开放寻址Hash不存在删除导致的probe链断裂问题。
 */
typedef struct
{
    __be32 original_ip;   // SNAT前源IPv4地址
    __be16 original_port; // SNAT前源端口或ICMP Echo Identifier
    __u8   snat_type;     // LINKG_FAST_NAT_SNAT_*
    __u8   state;         // LINKG_FAST_NAT_ENTRY_*
} linkg_fast_nat_entry_t;

/**
 * @brief Source NETMAP流方向项。
 *
 * 该表只用于区分：
 * 1. 本地Ethernet主动访问远端虚拟Endpoint后的返回包；
 * 2. 远端虚拟Endpoint主动访问本地Ethernet的新包。
 *
 * 两类包在linkg0返回方向具有相同的地址范围，必须保存最小流方向状态，
 * 才能避免返回包再次误命中VIRTUAL_ETHERNET_SNAT。
 */
typedef struct
{
    __be32 local_virtual_ip;  // Source NETMAP后的本地虚拟源IPv4
    __be32 remote_virtual_ip; // 远端虚拟Endpoint IPv4
    __be16 local_id;          // 本地TCP/UDP端口或ICMP Echo Identifier
    __be16 remote_id;         // 远端TCP/UDP端口或ICMP Echo Identifier
    __u8   protocol;          // IPv4上层协议
    __u8   state;             // LINKG_FAST_NAT_FLOW_*
    __u16  reserved;          // 对齐预留
} linkg_fast_nat_flow_entry_t;

/**
 * @brief Fast NAT运行上下文。
 */
typedef struct
{
    struct mutex                control_lock;                          // 控制面生命周期锁
    spinlock_t                  mapping_lock;                          // 新建SNAT映射锁
    spinlock_t                  flow_lock;                             // 新建Source NETMAP方向项锁
    linkg_fast_nat_config_t     config;                                // RUNNING期间只读配置
    linkg_fast_nat_entry_t      mappings[LINKG_FAST_NAT_SLOT_COUNT];   // translated port直接索引表
    linkg_fast_nat_flow_entry_t flows[LINKG_FAST_NAT_FLOW_SLOT_COUNT]; // Source NETMAP方向表
    __u32                       mapping_count;                         // 当前活动SNAT映射数量
    __u32                       flow_count;                            // 当前活动Source NETMAP流数量
    __u32                       state;                                 // LINKG_FAST_NAT_STATE_*
} linkg_fast_nat_context_t;

/**
 * @brief TCP/UDP公共端口头。
 */
typedef struct
{
    __be16 source; // 源端口
    __be16 dest;   // 目的端口
} linkg_fast_nat_ports_t;

/****************************** 全局上下文 ******************************/

static linkg_fast_nat_context_t g_fast_nat =
{
    .control_lock = __MUTEX_INITIALIZER(g_fast_nat.control_lock),   // 控制面生命周期锁
    .mapping_lock = __SPIN_LOCK_UNLOCKED(g_fast_nat.mapping_lock), // SNAT映射锁
    .flow_lock    = __SPIN_LOCK_UNLOCKED(g_fast_nat.flow_lock),    // Source NETMAP方向锁
    .state        = LINKG_FAST_NAT_STATE_UNCONFIGURED              // 尚未配置
};

/****************************** IPv4辅助 ******************************/

/**
 * @brief 判断IPv4地址是否属于指定子网。
 */
static bool _linkg_fast_nat_ipv4_in_subnet(__be32 address, const linkg_fast_nat_ipv4_subnet_t *subnet)
{
    return (address & subnet->netmask) == subnet->network;
}

/**
 * @brief 判断IPv4子网是否已经规范化。
 */
static bool _linkg_fast_nat_ipv4_subnet_normalized(const linkg_fast_nat_ipv4_subnet_t *subnet)
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
static bool _linkg_fast_nat_ipv4_netmask_valid(__be32 netmask)
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
static __be32 _linkg_fast_nat_ipv4_prefix_map(__be32 address, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to)
{
    return to->network | (address & ~from->netmask);
}

/**
 * @brief 判断目标地址是否属于Source NETMAP需要处理的LinkG地址空间。
 *
 * @note virtual_network用于节点虚拟Endpoint通信；
 *       tun_network用于TUN本机发起流量的返回路径。
 */
static bool _linkg_fast_nat_source_netmap_destination_match(__be32 address)
{
    return _linkg_fast_nat_ipv4_in_subnet(address, &g_fast_nat.config.virtual_network) ||
           _linkg_fast_nat_ipv4_in_subnet(address, &g_fast_nat.config.tun_network);
}

/****************************** skb辅助 ******************************/

/**
 * @brief 获取IPv4头并校验第一版Fast NAT支持的数据包形态。
 *
 * 第一版状态SNAT不处理IPv4 fragmentation。
 */
static int _linkg_fast_nat_get_ipv4(struct sk_buff *skb, struct iphdr **iph, unsigned int *transport_offset)
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
 * source=true获取源端口；source=false获取目的端口。
 * ICMP Echo Identifier在请求和响应两个方向均使用同一字段。
 */
static int _linkg_fast_nat_get_transport_id(struct sk_buff *skb, const struct iphdr *iph, unsigned int transport_offset, bool source, __be16 *id)
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
static void _linkg_fast_nat_mark_untracked(struct sk_buff *skb)
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
static int _linkg_fast_nat_replace_ipv4(struct sk_buff *skb, bool source, __be32 new_ip)
{
    struct iphdr  *iph;
    struct tcphdr *tcph;
    struct udphdr *udph;
    unsigned int   transport_offset;
    unsigned int   write_length;
    __be32         old_ip;
    int            ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
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
static int _linkg_fast_nat_replace_transport_id(struct sk_buff *skb, bool source, __be16 new_id)
{
    struct iphdr            *iph;
    linkg_fast_nat_ports_t  *ports;
    struct tcphdr           *tcph;
    struct udphdr           *udph;
    struct icmphdr          *icmph;
    unsigned int             transport_offset;
    unsigned int             write_length;
    __be16                   old_id;
    int                      ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
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
static int _linkg_fast_nat_replace_tuple(struct sk_buff *skb, bool source, __be32 new_ip, __be16 new_id)
{
    int ret;

    ret = _linkg_fast_nat_replace_transport_id(skb, source, new_id);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_fast_nat_replace_ipv4(skb, source, new_ip);
}

/****************************** SNAT映射 ******************************/

/**
 * @brief 计算原始IPv4和端口对应的初始Hash槽位。
 */
static __u32 _linkg_fast_nat_mapping_hash(__be32 original_ip, __be16 original_port)
{
    __u32 hash;

    hash = jhash_2words((__force __u32)original_ip, (__u32)ntohs(original_port), LINKG_FAST_NAT_HASH_SEED);

    return hash & LINKG_FAST_NAT_SLOT_MASK;
}

/**
 * @brief 清空全部SNAT映射。
 *
 * 仅在hooks未运行时调用。
 */
static void _linkg_fast_nat_mapping_reset(void)
{
    memset(g_fast_nat.mappings, 0, sizeof(g_fast_nat.mappings));
    g_fast_nat.mapping_count = 0U;
}

/**
 * @brief 无锁查找现有SNAT映射。
 *
 * RUNNING期间第一版映射只增加不删除，因此遇到FREE即可确定不存在。
 */
static int _linkg_fast_nat_mapping_lookup(__be32 original_ip, __be16 original_port, __u32 *slot)
{
    linkg_fast_nat_entry_t *entry;
    __u32                   current_slot;
    __u32                   probe;
    __u8                    state;

    if (slot == NULL)
    {
        return -EINVAL;
    }

    current_slot = _linkg_fast_nat_mapping_hash(original_ip, original_port);

    for (probe = 0U; probe < LINKG_FAST_NAT_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.mappings[current_slot];
        state = smp_load_acquire(&entry->state);

        if (state == LINKG_FAST_NAT_ENTRY_FREE)
        {
            return -ENOENT;
        }

        if (entry->original_ip == original_ip && entry->original_port == original_port)
        {
            *slot = current_slot;
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_SLOT_MASK;
    }

    return -ENOENT;
}

/**
 * @brief 获取现有SNAT映射或创建新映射。
 *
 * 已存在映射的常规数据路径无锁。
 * 只有首次建立新映射时进入mapping_lock并二次检查。
 */
static int _linkg_fast_nat_mapping_get_or_create(__be32 original_ip, __be16 original_port, __u8 snat_type, __be16 *translated_port)
{
    linkg_fast_nat_entry_t *entry;
    unsigned long           flags;
    __u32                   current_slot;
    __u32                   probe;
    __u32                   slot;
    __u16                   port;
    int                     ret;

    if (translated_port == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_fast_nat_mapping_lookup(original_ip, original_port, &slot);
    if (ret == 0)
    {
        entry = &g_fast_nat.mappings[slot];
        if (READ_ONCE(entry->snat_type) != snat_type)
        {
            return -EEXIST;
        }

        port = (__u16)(g_fast_nat.config.snat_port_start + slot);
        *translated_port = htons(port);
        return 0;
    }

    spin_lock_irqsave(&g_fast_nat.mapping_lock, flags);

    ret = _linkg_fast_nat_mapping_lookup(original_ip, original_port, &slot);
    if (ret == 0)
    {
        entry = &g_fast_nat.mappings[slot];
        if (entry->snat_type != snat_type)
        {
            spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
            return -EEXIST;
        }

        port = (__u16)(g_fast_nat.config.snat_port_start + slot);
        *translated_port = htons(port);
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return 0;
    }

    if (g_fast_nat.mapping_count >= LINKG_FAST_NAT_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOSPC;
    }

    current_slot = _linkg_fast_nat_mapping_hash(original_ip, original_port);

    for (probe = 0U; probe < LINKG_FAST_NAT_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.mappings[current_slot];

        if (entry->state == LINKG_FAST_NAT_ENTRY_FREE)
        {
            entry->original_ip   = original_ip;
            entry->original_port = original_port;
            entry->snat_type     = snat_type;

            /**
             * state最后发布，使反向无锁读取在看到ACTIVE后能够获得
             * 已经完整写入的original_ip/original_port/snat_type。
             */
            smp_store_release(&entry->state, LINKG_FAST_NAT_ENTRY_ACTIVE);

            g_fast_nat.mapping_count++;

            port = (__u16)(g_fast_nat.config.snat_port_start + current_slot);
            *translated_port = htons(port);

            spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_SLOT_MASK;
    }

    spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);

    return -ENOSPC;
}

/**
 * @brief 根据translated port直接O(1)获取SNAT反向映射。
 */
static int _linkg_fast_nat_mapping_reverse(__be16 translated_port, __be32 *original_ip, __be16 *original_port, __u8 *snat_type)
{
    linkg_fast_nat_entry_t *entry;
    __u32                   slot;
    __u16                   port;

    if (original_ip == NULL || original_port == NULL || snat_type == NULL)
    {
        return -EINVAL;
    }

    port = ntohs(translated_port);
    if (port < g_fast_nat.config.snat_port_start || port > g_fast_nat.config.snat_port_end)
    {
        return -ENOENT;
    }

    slot = (__u32)(port - g_fast_nat.config.snat_port_start);
    if (slot >= LINKG_FAST_NAT_SLOT_COUNT)
    {
        return -ENOENT;
    }

    entry = &g_fast_nat.mappings[slot];
    if (smp_load_acquire(&entry->state) != LINKG_FAST_NAT_ENTRY_ACTIVE)
    {
        return -ENOENT;
    }

    *original_ip   = entry->original_ip;
    *original_port = entry->original_port;
    *snat_type     = entry->snat_type;

    return 0;
}

/****************************** Source NETMAP方向表 ******************************/

/**
 * @brief 计算Source NETMAP正向流对应的初始Hash槽位。
 */
static __u32 _linkg_fast_nat_flow_hash(__u8 protocol, __be32 local_virtual_ip, __be16 local_id, __be32 remote_virtual_ip, __be16 remote_id)
{
    __u32 ids;
    __u32 hash;

    ids = ((__u32)ntohs(local_id) << 16U) | (__u32)ntohs(remote_id);

    hash = jhash_3words((__force __u32)local_virtual_ip, (__force __u32)remote_virtual_ip, ids, LINKG_FAST_NAT_FLOW_HASH_SEED ^ (__u32)protocol);

    return hash & LINKG_FAST_NAT_FLOW_SLOT_MASK;
}

/**
 * @brief 清空全部Source NETMAP方向状态。
 *
 * 第一版方向项仅在START到STOP之间增加，不执行运行期删除。
 */
static void _linkg_fast_nat_flow_reset(void)
{
    memset(g_fast_nat.flows, 0, sizeof(g_fast_nat.flows));
    g_fast_nat.flow_count = 0U;
}

/**
 * @brief 无锁查找Source NETMAP正向流。
 */
static int _linkg_fast_nat_flow_lookup(__u8 protocol, __be32 local_virtual_ip, __be16 local_id, __be32 remote_virtual_ip, __be16 remote_id, __u32 *slot)
{
    linkg_fast_nat_flow_entry_t *entry;
    __u32                        current_slot;
    __u32                        probe;
    __u8                         state;

    if (slot == NULL)
    {
        return -EINVAL;
    }

    current_slot = _linkg_fast_nat_flow_hash(protocol, local_virtual_ip, local_id, remote_virtual_ip, remote_id);

    for (probe = 0U; probe < LINKG_FAST_NAT_FLOW_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.flows[current_slot];
        state = smp_load_acquire(&entry->state);

        if (state == LINKG_FAST_NAT_FLOW_FREE)
        {
            return -ENOENT;
        }

        if (entry->protocol == protocol &&
            entry->local_virtual_ip == local_virtual_ip &&
            entry->local_id == local_id &&
            entry->remote_virtual_ip == remote_virtual_ip &&
            entry->remote_id == remote_id)
        {
            *slot = current_slot;
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_FLOW_SLOT_MASK;
    }

    return -ENOENT;
}

/**
 * @brief 获取已有Source NETMAP方向项或创建新方向项。
 *
 * 已有流的常规数据路径无锁，只有首次建立方向项时加锁。
 */
static int _linkg_fast_nat_flow_get_or_create(__u8 protocol, __be32 local_virtual_ip, __be16 local_id, __be32 remote_virtual_ip, __be16 remote_id)
{
    linkg_fast_nat_flow_entry_t *entry;
    unsigned long                flags;
    __u32                        current_slot;
    __u32                        probe;
    __u32                        slot;
    int                          ret;

    ret = _linkg_fast_nat_flow_lookup(protocol, local_virtual_ip, local_id, remote_virtual_ip, remote_id, &slot);
    if (ret == 0)
    {
        return 0;
    }

    spin_lock_irqsave(&g_fast_nat.flow_lock, flags);

    ret = _linkg_fast_nat_flow_lookup(protocol, local_virtual_ip, local_id, remote_virtual_ip, remote_id, &slot);
    if (ret == 0)
    {
        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
        return 0;
    }

    if (g_fast_nat.flow_count >= LINKG_FAST_NAT_FLOW_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
        return -ENOSPC;
    }

    current_slot = _linkg_fast_nat_flow_hash(protocol, local_virtual_ip, local_id, remote_virtual_ip, remote_id);

    for (probe = 0U; probe < LINKG_FAST_NAT_FLOW_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.flows[current_slot];

        if (entry->state == LINKG_FAST_NAT_FLOW_FREE)
        {
            entry->local_virtual_ip  = local_virtual_ip;
            entry->remote_virtual_ip = remote_virtual_ip;
            entry->local_id          = local_id;
            entry->remote_id         = remote_id;
            entry->protocol          = protocol;

            /**
             * state最后发布，确保无锁读取看到ACTIVE后其余字段已经完整。
             */
            smp_store_release(&entry->state, LINKG_FAST_NAT_FLOW_ACTIVE);

            g_fast_nat.flow_count++;

            spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_FLOW_SLOT_MASK;
    }

    spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);

    return -ENOSPC;
}

/**
 * @brief 为即将执行Source NETMAP的正向包建立最小方向状态。
 *
 * TCP/UDP使用源/目的端口；ICMP Echo使用Identifier。
 * 其他协议当前不建立方向项，但仍允许执行无状态Source NETMAP。
 */
static int _linkg_fast_nat_flow_track_source_netmap(struct sk_buff *skb)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be32        local_virtual_ip;
    __be16        local_id;
    __be16        remote_id;
    int           ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &local_id);
    if (ret == -EOPNOTSUPP)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &remote_id);
    if (ret == -EOPNOTSUPP)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    local_virtual_ip = _linkg_fast_nat_ipv4_prefix_map(iph->saddr, &g_fast_nat.config.ethernet_network, &g_fast_nat.config.local_virtual_subnet);

    return _linkg_fast_nat_flow_get_or_create(iph->protocol, local_virtual_ip, local_id, iph->daddr, remote_id);
}

/**
 * @brief 判断linkg0进入的数据包是否为已有Source NETMAP流的返回方向。
 *
 * 返回1表示命中返回方向；返回0表示不是；负值表示解析失败。
 */
static int _linkg_fast_nat_flow_reverse_match(struct sk_buff *skb)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be16        local_id;
    __be16        remote_id;
    __u32         slot;
    int           ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return 0;
    }

    ret = _linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &local_id);
    if (ret == -EOPNOTSUPP)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &remote_id);
    if (ret == -EOPNOTSUPP)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_flow_lookup(iph->protocol, iph->daddr, local_id, iph->saddr, remote_id, &slot);

    return ret == 0 ? 1 : 0;
}

/****************************** NAT基础动作 ******************************/

/**
 * @brief 执行无状态Destination前缀映射。
 */
static int _linkg_fast_nat_destination_netmap(struct sk_buff *skb, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be32        new_ip;
    int           ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    new_ip = _linkg_fast_nat_ipv4_prefix_map(iph->daddr, from, to);

    return _linkg_fast_nat_replace_ipv4(skb, false, new_ip);
}

/**
 * @brief 执行无状态Source前缀映射。
 */
static int _linkg_fast_nat_source_netmap(struct sk_buff *skb, const linkg_fast_nat_ipv4_subnet_t *from, const linkg_fast_nat_ipv4_subnet_t *to)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be32        new_ip;
    int           ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    new_ip = _linkg_fast_nat_ipv4_prefix_map(iph->saddr, from, to);

    return _linkg_fast_nat_replace_ipv4(skb, true, new_ip);
}

/**
 * @brief 执行Ethernet出口Fast SNAT。
 */
static int _linkg_fast_nat_ethernet_snat(struct sk_buff *skb, __u8 snat_type)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be16        original_port;
    __be16        translated_port;
    __be32        original_ip;
    int           ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &original_port);
    if (ret != 0)
    {
        return ret;
    }

    original_ip = iph->saddr;

    ret = _linkg_fast_nat_mapping_get_or_create(original_ip, original_port, snat_type, &translated_port);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_fast_nat_replace_tuple(skb, true, g_fast_nat.config.ethernet_ip, translated_port);
}

/**
 * @brief 尝试执行Ethernet入口SNAT反向恢复。
 *
 * 成功恢复返回1；不是Fast SNAT回包返回0；处理失败返回负errno。
 */
static int _linkg_fast_nat_reverse_snat(struct sk_buff *skb)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    __be32        original_ip;
    __be16        original_port;
    __be16        translated_port;
    __u8          snat_type;
    int           ret;

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return 0;
    }

    if (iph->daddr != g_fast_nat.config.ethernet_ip)
    {
        return 0;
    }

    ret = _linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &translated_port);
    if (ret != 0)
    {
        return 0;
    }

    ret = _linkg_fast_nat_mapping_reverse(translated_port, &original_ip, &original_port, &snat_type);
    if (ret != 0)
    {
        return 0;
    }

    _linkg_fast_nat_mark_untracked(skb);

    ret = _linkg_fast_nat_replace_tuple(skb, false, original_ip, original_port);
    if (ret != 0)
    {
        return ret;
    }

    /**
     * 所有Fast SNAT回包都属于已有映射的返回方向。
     * POSTROUTING仍允许先执行必要的Source NETMAP反向源地址恢复，
     * 随后必须跳过Rule 5/6/7，避免再次把返回包当成新的SNAT请求。
     */
    skb->mark |= LINKG_FAST_NAT_MARK_REVERSE;

    /**
     * Hairpin正向同时做过Destination NETMAP。
     * 反向恢复目的地址后，需要把Ethernet真实源地址恢复为
     * 本节点虚拟Endpoint源地址。
     */
    if (snat_type == LINKG_FAST_NAT_SNAT_HAIRPIN)
    {
        iph = ip_hdr(skb);

        if (_linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network))
        {
            ret = _linkg_fast_nat_source_netmap(skb, &g_fast_nat.config.ethernet_network, &g_fast_nat.config.local_virtual_subnet);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    return 1;
}

/****************************** 配置校验 ******************************/

/**
 * @brief 校验Fast NAT IPv4子网配置。
 */
static bool _linkg_fast_nat_config_subnet_valid(const linkg_fast_nat_ipv4_subnet_t *subnet)
{
    if (subnet == NULL)
    {
        return false;
    }

    if (!_linkg_fast_nat_ipv4_netmask_valid(subnet->netmask))
    {
        return false;
    }

    return _linkg_fast_nat_ipv4_subnet_normalized(subnet);
}

/**
 * @brief 判断当前是否启用了任一状态SNAT规则。
 */
static bool _linkg_fast_nat_config_snat_enabled(__u32 enabled_rules)
{
    return (enabled_rules & (LINKG_FAST_NAT_RULE_VIRTUAL_ETHERNET_SNAT | LINKG_FAST_NAT_RULE_TUN_ETHERNET_SNAT | LINKG_FAST_NAT_RULE_HAIRPIN_SNAT)) != 0U;
}

/**
 * @brief 校验用户态下发的Fast NAT配置。
 */
static int _linkg_fast_nat_config_validate(const linkg_fast_nat_config_t *config)
{
    __u32 port_count;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->version != LINKG_FAST_NAT_UAPI_VERSION || config->struct_size != sizeof(*config))
    {
        return -EPROTO;
    }

    if ((config->enabled_rules & ~LINKG_FAST_NAT_RULE_ALL) != 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_fast_nat_config_subnet_valid(&config->ethernet_network) ||
        !_linkg_fast_nat_config_subnet_valid(&config->tun_network) ||
        !_linkg_fast_nat_config_subnet_valid(&config->virtual_network) ||
        !_linkg_fast_nat_config_subnet_valid(&config->local_virtual_subnet))
    {
        return -EINVAL;
    }

    /**
     * 当前NETMAP实现保留Host部分，因此真实Ethernet子网和
     * 本节点虚拟Endpoint子网必须使用相同掩码。
     */
    if (config->ethernet_network.netmask != config->local_virtual_subnet.netmask)
    {
        return -EINVAL;
    }

    if (!_linkg_fast_nat_ipv4_in_subnet(config->ethernet_ip, &config->ethernet_network))
    {
        return -EINVAL;
    }

    if (!_linkg_fast_nat_ipv4_in_subnet(config->local_virtual_subnet.network, &config->virtual_network))
    {
        return -EINVAL;
    }

    if (config->ethernet_ifindex <= 0 || config->tun_ifindex <= 0)
    {
        return -EINVAL;
    }

    if (_linkg_fast_nat_config_snat_enabled(config->enabled_rules))
    {
        if (config->snat_port_end < config->snat_port_start)
        {
            return -EINVAL;
        }

        port_count = (__u32)config->snat_port_end - (__u32)config->snat_port_start + 1U;
        if (port_count != LINKG_FAST_NAT_SLOT_COUNT)
        {
            return -EINVAL;
        }
    }

    return 0;
}

/****************************** PREROUTING ******************************/

/**
 * @brief 判断当前PREROUTING包是否属于后续Source NETMAP快速路径。
 */
static bool _linkg_fast_nat_prerouting_source_netmap_match(const struct nf_hook_state *state, const struct iphdr *iph)
{
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_SOURCE_NETMAP) == 0U)
    {
        return false;
    }

    if (state->in == NULL || state->in->ifindex != g_fast_nat.config.ethernet_ifindex)
    {
        return false;
    }

    return _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network) &&
           _linkg_fast_nat_source_netmap_destination_match(iph->daddr);
}

/**
 * @brief Netfilter PRE_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_prerouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    int           ret;

    (void)priv;

    if (READ_ONCE(g_fast_nat.state) != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return NF_ACCEPT;
    }

    if (state->in == NULL)
    {
        return NF_ACCEPT;
    }

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return NF_ACCEPT;
    }

    /**
     * Ethernet SNAT回包优先处理。
     * translated port直接定位entry，不执行Hash查找。
     */
    if (state->in->ifindex == g_fast_nat.config.ethernet_ifindex)
    {
        ret = _linkg_fast_nat_reverse_snat(skb);
        if (ret < 0)
        {
            return NF_DROP;
        }

        if (ret > 0)
        {
            return NF_ACCEPT;
        }

        iph = ip_hdr(skb);
    }

    /**
     * Source NETMAP返回方向识别。
     *
     * 本地Ethernet主动访问远端虚拟Endpoint后的返回包，与远端主动访问
     * 本地Ethernet的新包在地址范围上完全相同。这里在执行Rule 1 DNAT前
     * 通过最小流方向表区分两者，命中返回方向后留下REVERSE标记。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_SOURCE_NETMAP) != 0U &&
        state->in->ifindex == g_fast_nat.config.tun_ifindex &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.local_virtual_subnet))
    {
        ret = _linkg_fast_nat_flow_reverse_match(skb);
        if (ret < 0)
        {
            return NF_DROP;
        }

        if (ret > 0)
        {
            _linkg_fast_nat_mark_untracked(skb);
            skb->mark |= LINKG_FAST_NAT_MARK_REVERSE;
        }
    }

    /**
     * Rule 1:
     * linkg0进入且目标属于本节点虚拟Endpoint子网，
     * 将目的IP映射为本节点Ethernet真实地址。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_LINKG_TO_ETHERNET_DNAT) != 0U &&
        state->in->ifindex == g_fast_nat.config.tun_ifindex &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.local_virtual_subnet))
    {
        _linkg_fast_nat_mark_untracked(skb);

        ret = _linkg_fast_nat_destination_netmap(skb, &g_fast_nat.config.local_virtual_subnet, &g_fast_nat.config.ethernet_network);
        if (ret != 0)
        {
            return NF_DROP;
        }

        return NF_ACCEPT;
    }

    /**
     * Rule 6的SNAT发生在POSTROUTING，这里提前将其标记为UNTRACKED。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_TUN_ETHERNET_SNAT) != 0U &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.tun_network) &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        _linkg_fast_nat_mark_untracked(skb);
    }

    /**
     * Rule 5若直接访问Ethernet真实地址，也必须在conntrack之前标记。
     * 远端访问虚拟Endpoint的路径已经由Rule 1完成标记。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_VIRTUAL_ETHERNET_SNAT) != 0U &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.virtual_network) &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        _linkg_fast_nat_mark_untracked(skb);
    }

    /**
     * Rule 2:
     * Ethernet访问本节点虚拟Endpoint时执行Destination NETMAP。
     * 若Rule 7启用，同时给本skb留下Hairpin标志供POSTROUTING使用。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_LOCAL_ETHERNET_DESTINATION_NETMAP) != 0U &&
        state->in->ifindex == g_fast_nat.config.ethernet_ifindex &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.local_virtual_subnet))
    {
        _linkg_fast_nat_mark_untracked(skb);

        if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_HAIRPIN_SNAT) != 0U)
        {
            skb->mark |= LINKG_FAST_NAT_MARK_HAIRPIN;
        }

        ret = _linkg_fast_nat_destination_netmap(skb, &g_fast_nat.config.local_virtual_subnet, &g_fast_nat.config.ethernet_network);
        if (ret != 0)
        {
            skb->mark &= ~LINKG_FAST_NAT_MARK_HAIRPIN;
            return NF_DROP;
        }

        return NF_ACCEPT;
    }

    /**
     * Rule 4只在POSTROUTING修改源地址，但必须在这里提前绕过conntrack。
     */
    iph = ip_hdr(skb);
    if (_linkg_fast_nat_prerouting_source_netmap_match(state, iph))
    {
        _linkg_fast_nat_mark_untracked(skb);
    }

    return NF_ACCEPT;
}

/****************************** LOCAL_OUT ******************************/

/**
 * @brief Netfilter LOCAL_OUT Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_local_out(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    bool          reroute;
    int           ret;

    (void)priv;

    if (READ_ONCE(g_fast_nat.state) != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return NF_ACCEPT;
    }

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return NF_ACCEPT;
    }

    reroute = false;

    /**
     * Rule 3:
     * 本机访问本节点虚拟Endpoint时，将目的地址直接映射为
     * Ethernet真实地址。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_LOCAL_OUTPUT_DESTINATION_NETMAP) != 0U &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.local_virtual_subnet))
    {
        _linkg_fast_nat_mark_untracked(skb);

        ret = _linkg_fast_nat_destination_netmap(skb, &g_fast_nat.config.local_virtual_subnet, &g_fast_nat.config.ethernet_network);
        if (ret != 0)
        {
            return NF_DROP;
        }

        reroute = true;
        iph     = ip_hdr(skb);
    }

    /**
     * 本机产生但后续需要在POSTROUTING执行的Fast NAT流量，
     * 在LOCAL_OUT的conntrack hook之前提前标记UNTRACKED。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_SOURCE_NETMAP) != 0U &&
    _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network) &&
    _linkg_fast_nat_source_netmap_destination_match(iph->daddr))
    {
        _linkg_fast_nat_mark_untracked(skb);
    }

    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_VIRTUAL_ETHERNET_SNAT) != 0U &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.virtual_network) &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        _linkg_fast_nat_mark_untracked(skb);
    }

    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_TUN_ETHERNET_SNAT) != 0U &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.tun_network) &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        _linkg_fast_nat_mark_untracked(skb);
    }

    /**
     * LOCAL_OUT在首次路由之后执行，修改目的地址后必须重新选路。
     */
    if (reroute)
    {
        ret = ip_route_me_harder(state->net, skb, RTN_UNSPEC);
        if (ret != 0)
        {
            return NF_DROP;
        }
    }

    return NF_ACCEPT;
}

/****************************** POSTROUTING ******************************/

/**
 * @brief Netfilter POST_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_postrouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    int           ret;

    (void)priv;

    if (READ_ONCE(g_fast_nat.state) != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return NF_ACCEPT;
    }

    if (state->out == NULL)
    {
        return NF_ACCEPT;
    }

    ret = _linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;
        return NF_ACCEPT;
    }

    /**
     * Rule 4:
     * Ethernet进入LinkG时，将源地址映射为本节点虚拟Endpoint地址。
     *
     * 目标为virtual_network时属于Ethernet主动访问远端虚拟Endpoint，
     * 需要建立Source NETMAP方向状态。
     *
     * 目标为tun_network时属于TUN发起流量的返回路径，
     * 只执行无状态Source NETMAP，不建立方向状态。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_SOURCE_NETMAP) != 0U &&
        state->out->ifindex == g_fast_nat.config.tun_ifindex &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network) &&
        _linkg_fast_nat_source_netmap_destination_match(iph->daddr))
    {
        if (_linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.virtual_network))
        {
            ret = _linkg_fast_nat_flow_track_source_netmap(skb);
            if (ret != 0)
            {
                skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;
                return NF_DROP;
            }
        }

        ret = _linkg_fast_nat_source_netmap(skb,
                                            &g_fast_nat.config.ethernet_network,
                                            &g_fast_nat.config.local_virtual_subnet);
        if (ret != 0)
        {
            skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;
            return NF_DROP;
        }

        iph = ip_hdr(skb);
    }

    /**
     * 已有Fast NAT流的返回包已经完成必要的反向恢复。
     * Rule 4位于本判断之前，仍可完成Ethernet真实源地址到虚拟源地址的
     * 无状态恢复；此后禁止再次进入Rule 5/6/7状态SNAT。
     */
    if ((skb->mark & LINKG_FAST_NAT_MARK_REVERSE) != 0U)
    {
        skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;
        return NF_ACCEPT;
    }

    /**
     * Rule 7:
     * 仅对Rule 2实际DNAT过的同一个skb执行Hairpin SNAT。
     * 避免把普通Ethernet二层/三层本地通信误当成Hairpin。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_HAIRPIN_SNAT) != 0U &&
        (skb->mark & LINKG_FAST_NAT_MARK_HAIRPIN) != 0U &&
        state->out->ifindex == g_fast_nat.config.ethernet_ifindex)
    {
        ret = _linkg_fast_nat_ethernet_snat(skb, LINKG_FAST_NAT_SNAT_HAIRPIN);

        skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;

        return ret == 0 ? NF_ACCEPT : NF_DROP;
    }

    skb->mark &= ~LINKG_FAST_NAT_MARK_HAIRPIN;

    iph = ip_hdr(skb);

    /**
     * Rule 5:
     * 虚拟Endpoint访问本地Ethernet时使用本机Ethernet地址和
     * Fast NAT translated port作为源tuple。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_VIRTUAL_ETHERNET_SNAT) != 0U &&
        state->out->ifindex == g_fast_nat.config.ethernet_ifindex &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.virtual_network) &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        ret = _linkg_fast_nat_ethernet_snat(skb, LINKG_FAST_NAT_SNAT_VIRTUAL);

        skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;

        return ret == 0 ? NF_ACCEPT : NF_DROP;
    }

    /**
     * Rule 6:
     * TUN节点访问本地Ethernet时使用相同Fast SNAT mapping engine。
     */
    if ((g_fast_nat.config.enabled_rules & LINKG_FAST_NAT_RULE_TUN_ETHERNET_SNAT) != 0U &&
        state->out->ifindex == g_fast_nat.config.ethernet_ifindex &&
        _linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.tun_network) &&
        _linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        ret = _linkg_fast_nat_ethernet_snat(skb, LINKG_FAST_NAT_SNAT_TUN);

        skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;

        return ret == 0 ? NF_ACCEPT : NF_DROP;
    }

    skb->mark &= ~LINKG_FAST_NAT_MARK_MASK;

    return NF_ACCEPT;
}

/****************************** Netfilter Hook ******************************/

static struct nf_hook_ops g_fast_nat_hooks[] =
{
    {
        .hook     = _linkg_fast_nat_prerouting,       // PRE_ROUTING处理函数
        .pf       = NFPROTO_IPV4,                     // IPv4协议族
        .hooknum  = NF_INET_PRE_ROUTING,              // PRE_ROUTING Hook
        .priority = LINKG_FAST_NAT_PRIORITY_EARLY     // conntrack之前执行
    },
    {
        .hook     = _linkg_fast_nat_local_out,        // LOCAL_OUT处理函数
        .pf       = NFPROTO_IPV4,                     // IPv4协议族
        .hooknum  = NF_INET_LOCAL_OUT,                // LOCAL_OUT Hook
        .priority = LINKG_FAST_NAT_PRIORITY_EARLY     // conntrack之前执行
    },
    {
        .hook     = _linkg_fast_nat_postrouting,      // POST_ROUTING处理函数
        .pf       = NFPROTO_IPV4,                     // IPv4协议族
        .hooknum  = NF_INET_POST_ROUTING,             // POST_ROUTING Hook
        .priority = LINKG_FAST_NAT_PRIORITY_POSTROUTING // 常规Source NAT之前执行
    }
};

/****************************** 生命周期 ******************************/

/**
 * @brief 启动Fast NAT数据面。
 */
static int _linkg_fast_nat_start(void)
{
    int ret;

    if (g_fast_nat.state == LINKG_FAST_NAT_STATE_RUNNING)
    {
        return 0;
    }

    if (g_fast_nat.state != LINKG_FAST_NAT_STATE_CONFIGURED)
    {
        return -ENODEV;
    }

    _linkg_fast_nat_mapping_reset();
    _linkg_fast_nat_flow_reset();

    ret = nf_register_net_hooks(&init_net, g_fast_nat_hooks, ARRAY_SIZE(g_fast_nat_hooks));
    if (ret != 0)
    {
        return ret;
    }

    WRITE_ONCE(g_fast_nat.state, LINKG_FAST_NAT_STATE_RUNNING);

    return 0;
}

/**
 * @brief 停止Fast NAT数据面。
 */
static int _linkg_fast_nat_stop(void)
{
    if (g_fast_nat.state != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return 0;
    }

    WRITE_ONCE(g_fast_nat.state, LINKG_FAST_NAT_STATE_CONFIGURED);

    nf_unregister_net_hooks(&init_net, g_fast_nat_hooks, ARRAY_SIZE(g_fast_nat_hooks));

    _linkg_fast_nat_mapping_reset();
    _linkg_fast_nat_flow_reset();

    return 0;
}

/****************************** ioctl ******************************/

/**
 * @brief 设置Fast NAT运行配置。
 */
static int _linkg_fast_nat_set_config(unsigned long arg)
{
    linkg_fast_nat_config_t config;
    int                     ret;

    if (copy_from_user(&config, (void __user *)arg, sizeof(config)) != 0U)
    {
        return -EFAULT;
    }

    ret = _linkg_fast_nat_config_validate(&config);
    if (ret != 0)
    {
        return ret;
    }

    if (g_fast_nat.state == LINKG_FAST_NAT_STATE_RUNNING)
    {
        return -EBUSY;
    }

    g_fast_nat.config = config;
    g_fast_nat.state  = LINKG_FAST_NAT_STATE_CONFIGURED;

    return 0;
}

/**
 * @brief 获取Fast NAT运行状态。
 */
static int _linkg_fast_nat_get_status(unsigned long arg)
{
    linkg_fast_nat_status_t status;

    memset(&status, 0, sizeof(status));

    status.version       = LINKG_FAST_NAT_UAPI_VERSION;
    status.struct_size   = sizeof(status);
    status.state         = READ_ONCE(g_fast_nat.state);
    status.enabled_rules = g_fast_nat.config.enabled_rules;

    if (copy_to_user((void __user *)arg, &status, sizeof(status)) != 0U)
    {
        return -EFAULT;
    }

    return 0;
}

/**
 * @brief 处理Fast NAT字符设备ioctl。
 */
static long _linkg_fast_nat_ioctl(struct file *file, unsigned int command, unsigned long arg)
{
    long ret;

    (void)file;

    mutex_lock(&g_fast_nat.control_lock);

    switch (command)
    {
        case LINKG_FAST_NAT_IOC_SET_CONFIG:
            ret = _linkg_fast_nat_set_config(arg);
            break;

        case LINKG_FAST_NAT_IOC_START:
            ret = _linkg_fast_nat_start();
            break;

        case LINKG_FAST_NAT_IOC_STOP:
            ret = _linkg_fast_nat_stop();
            break;

        case LINKG_FAST_NAT_IOC_GET_STATUS:
            ret = _linkg_fast_nat_get_status(arg);
            break;

        default:
            ret = -ENOTTY;
            break;
    }

    mutex_unlock(&g_fast_nat.control_lock);

    return ret;
}

static const struct file_operations g_fast_nat_file_operations =
{
    .owner          = THIS_MODULE,              // 模块所有者
    .unlocked_ioctl = _linkg_fast_nat_ioctl,    // 原生ioctl处理
#ifdef CONFIG_COMPAT
    .compat_ioctl   = _linkg_fast_nat_ioctl,    // 兼容ioctl处理
#endif
};

static struct miscdevice g_fast_nat_misc_device =
{
    .minor = MISC_DYNAMIC_MINOR,           // 动态分配次设备号
    .name  = LINKG_FAST_NAT_DEVICE_NAME,   // 字符设备名称
    .fops  = &g_fast_nat_file_operations   // 字符设备操作集
};

/****************************** 模块入口 ******************************/

/**
 * @brief 初始化LinkG Fast NAT内核模块。
 */
static int __init _linkg_fast_nat_module_init(void)
{
    int ret;

    _linkg_fast_nat_mapping_reset();
    _linkg_fast_nat_flow_reset();

    ret = misc_register(&g_fast_nat_misc_device);
    if (ret != 0)
    {
        return ret;
    }

    pr_info("LinkG Fast NAT: module loaded\n");

    return 0;
}

/**
 * @brief 卸载LinkG Fast NAT内核模块。
 */
static void __exit _linkg_fast_nat_module_exit(void)
{
    mutex_lock(&g_fast_nat.control_lock);

    if (g_fast_nat.state == LINKG_FAST_NAT_STATE_RUNNING)
    {
        _linkg_fast_nat_stop();
    }

    g_fast_nat.state = LINKG_FAST_NAT_STATE_UNCONFIGURED;

    mutex_unlock(&g_fast_nat.control_lock);

    misc_deregister(&g_fast_nat_misc_device);

    pr_info("LinkG Fast NAT: module unloaded\n");
}

module_init(_linkg_fast_nat_module_init);
module_exit(_linkg_fast_nat_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dawn");
MODULE_DESCRIPTION("LinkG lightweight high-performance IPv4 NAT");
MODULE_VERSION("1.0.0");
