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

#include "linkg_fast_nat_internal.h"

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

#define LINKG_FAST_NAT_ENTRY_FREE           0U // SNAT槽位空闲
#define LINKG_FAST_NAT_ENTRY_ACTIVE         1U // SNAT槽位有效
#define LINKG_FAST_NAT_ENTRY_TOMBSTONE      2U // SNAT超时

#define LINKG_FAST_NAT_FLOW_FREE            0U // Source NETMAP方向槽位空闲
#define LINKG_FAST_NAT_FLOW_ACTIVE          1U // Source NETMAP方向槽位有效

#define LINKG_FAST_NAT_SNAT_VIRTUAL         1U // 虚拟Endpoint访问Ethernet
#define LINKG_FAST_NAT_SNAT_TUN             2U // TUN节点访问Ethernet
#define LINKG_FAST_NAT_SNAT_HAIRPIN         3U // Ethernet Hairpin访问


typedef enum
{
    LINKG_FAST_NAT_RULE_CONTINUE = 0, // 当前规则未命中，继续后续规则
    LINKG_FAST_NAT_RULE_ACCEPT,       // 当前规则完成处理，结束Fast NAT规则链
    LINKG_FAST_NAT_RULE_DROP          // 当前规则处理失败，丢弃数据包
} linkg_fast_nat_rule_result_t;

typedef linkg_fast_nat_rule_result_t (*linkg_fast_nat_rule_func_t)(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);

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
    __be32        original_ip;   // SNAT前源IPv4地址
    __be16        original_port; // SNAT前源端口或ICMP Echo Identifier
    unsigned long last_seen;     // 最近一次命中的jiffies
    __u8          protocol;      // IPv4上层协议
    __u8          snat_type;     // LINKG_FAST_NAT_SNAT_*
    __u8          state;         // LINKG_FAST_NAT_ENTRY_*
    __u8          reserved;      // 对齐预留
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
    __be32        local_virtual_ip;  // Source NETMAP后的本地虚拟源IPv4
    __be32        remote_virtual_ip; // 远端虚拟Endpoint IPv4
    __be16        local_id;          // 本地TCP/UDP端口或ICMP Echo Identifier
    __be16        remote_id;         // 远端TCP/UDP端口或ICMP Echo Identifier
    unsigned long last_seen;         // 最近一次命中的jiffies
    __u8          protocol;          // IPv4上层协议
    __u8          state;             // LINKG_FAST_NAT_FLOW_*
    __u16         reserved;          // 对齐预留
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
    .mapping_lock = __SPIN_LOCK_UNLOCKED(g_fast_nat.mapping_lock),  // SNAT映射锁
    .flow_lock    = __SPIN_LOCK_UNLOCKED(g_fast_nat.flow_lock),     // Source NETMAP方向锁
    .state        = LINKG_FAST_NAT_STATE_UNCONFIGURED               // 尚未配置
};

/* 前置声明 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_postrouting_ethernet_to_virtual(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);

/****************************** Fast NAT规则表 ******************************/

static const linkg_fast_nat_rule_func_t g_prerouting_rules[] =
{
};

static const linkg_fast_nat_rule_func_t g_local_out_rules[] =
{
};

static const linkg_fast_nat_rule_func_t g_postrouting_rules[] =
{
    _linkg_fast_nat_postrouting_ethernet_to_virtual,
};


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
 * @brief 清空全部Source NETMAP方向状态。
 *
 * 第一版方向项仅在START到STOP之间增加，不执行运行期删除。
 */
static void _linkg_fast_nat_flow_reset(void)
{
    memset(g_fast_nat.flows, 0, sizeof(g_fast_nat.flows));
    g_fast_nat.flow_count = 0U;
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

    if (!linkg_fast_nat_ipv4_netmask_valid(subnet->netmask))
    {
        return false;
    }

    return linkg_fast_nat_ipv4_subnet_normalized(subnet);
}

/**
 * @brief 校验用户态下发的Fast NAT配置。
 */
static int _linkg_fast_nat_config_validate(const linkg_fast_nat_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->version != LINKG_FAST_NAT_UAPI_VERSION || config->struct_size != sizeof(*config))
    {
        return -EPROTO;
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

    if (!linkg_fast_nat_ipv4_in_subnet(config->ethernet_ip, &config->ethernet_network))
    {
        return -EINVAL;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(config->local_virtual_subnet.network, &config->virtual_network))
    {
        return -EINVAL;
    }

    if (config->ethernet_ifindex <= 0 || config->tun_ifindex <= 0)
    {
        return -EINVAL;
    }

    if (config->snat_port_start == 0U)
    {
        return -EINVAL;
    }

    if ((__u32)config->snat_port_start + LINKG_FAST_NAT_SLOT_COUNT - 1U > 65535U)
    {
        return -EINVAL;
    }


    return 0;
}

/****************************** PREROUTING ******************************/

/**
 * @brief Netfilter PRE_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_prerouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{

    return NF_ACCEPT;
}

/****************************** LOCAL_OUT ******************************/

/**
 * @brief Netfilter LOCAL_OUT Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_local_out(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{


    return NF_ACCEPT;
}


/****************************** POSTROUTING ******************************/

/**
 * @brief 处理Ethernet流量进入远端虚拟网络的Source NETMAP规则。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_postrouting_ethernet_to_virtual(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    int ret;

    if (state->out == NULL ||
        state->out->ifindex != g_fast_nat.config.tun_ifindex)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network) ||
        !linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.virtual_network))
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    ret = linkg_fast_nat_source_netmap(skb, &g_fast_nat.config.ethernet_network, &g_fast_nat.config.local_virtual_subnet);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_ACCEPT;
}

/**
 * @brief Netfilter POST_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_postrouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    linkg_fast_nat_rule_result_t result;
    struct iphdr                *iph;
    unsigned int                 transport_offset;
    unsigned int                 index;
    int                          ret;

    (void)priv;

    if (READ_ONCE(g_fast_nat.state) != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return NF_ACCEPT;
    }

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return NF_ACCEPT;
    }

    for (index = 0U; index < ARRAY_SIZE(g_postrouting_rules); index++)
    {
        result = g_postrouting_rules[index](skb, state, iph);

        if (result == LINKG_FAST_NAT_RULE_ACCEPT)
        {
            return NF_ACCEPT;
        }

        if (result == LINKG_FAST_NAT_RULE_DROP)
        {
            return NF_DROP;
        }
    }

    return NF_ACCEPT;
}

/****************************** Netfilter Hook ******************************/

static struct nf_hook_ops g_fast_nat_hooks[] =
{
    {
        .hook     = _linkg_fast_nat_prerouting,         // PRE_ROUTING处理函数
        .pf       = NFPROTO_IPV4,                       // IPv4协议族
        .hooknum  = NF_INET_PRE_ROUTING,                // PRE_ROUTING Hook
        .priority = LINKG_FAST_NAT_PRIORITY_EARLY       // conntrack之前执行
    },
    {
        .hook     = _linkg_fast_nat_local_out,          // LOCAL_OUT处理函数
        .pf       = NFPROTO_IPV4,                       // IPv4协议族
        .hooknum  = NF_INET_LOCAL_OUT,                  // LOCAL_OUT Hook
        .priority = LINKG_FAST_NAT_PRIORITY_EARLY       // conntrack之前执行
    },
    {
        .hook     = _linkg_fast_nat_postrouting,        // POST_ROUTING处理函数
        .pf       = NFPROTO_IPV4,                       // IPv4协议族
        .hooknum  = NF_INET_POST_ROUTING,               // POST_ROUTING Hook
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
