/**
 * @file linkg_fast_nat_main.c
 * @brief Fast NAT控制面、启停与Netfilter分发
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"
#include "linkg_fast_nat_utils.h"

#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/netdevice.h>
#include <linux/netfilter_ipv4.h>
#include <linux/uaccess.h>
#include <net/net_namespace.h>

/****************************** 模块状态 ******************************/

#define LINKG_FAST_NAT_STATE_UNCONFIGURED   0U                       // 尚未配置
#define LINKG_FAST_NAT_STATE_CONFIGURED     1U                       // 已配置但未启动
#define LINKG_FAST_NAT_STATE_RUNNING        2U                       // 数据面运行中
#define LINKG_FAST_NAT_PRIORITY_EARLY       (NF_IP_PRI_RAW + 50)     // conntrack之前执行
#define LINKG_FAST_NAT_PRIORITY_POSTROUTING (NF_IP_PRI_NAT_SRC - 10) // 常规SNAT之前执行

typedef struct
{
    struct mutex                     control_lock; // ioctl与启停控制锁
    linkg_fast_nat_config_t          config;       // 运行期间只读配置
    const linkg_fast_nat_rule_set_t *rules;        // 配置时选定的规则集
    __u32                            state;        // 当前模块运行状态
} linkg_fast_nat_context_t;

static linkg_fast_nat_context_t g_fast_nat =
{
    .control_lock = __MUTEX_INITIALIZER(g_fast_nat.control_lock),
    .state        = LINKG_FAST_NAT_STATE_UNCONFIGURED
};

/**
 * @brief 获取当前数据面配置，仅在已进入RUNNING的数据路径中使用。
 */
const linkg_fast_nat_config_t *linkg_fast_nat_get_config(void)
{
    return &g_fast_nat.config;
}

/**
 * @brief 检查数据面状态并取得启动时发布的配置和规则集。
 */
static bool _linkg_fast_nat_is_running(void)
{
    return smp_load_acquire(&g_fast_nat.state) == LINKG_FAST_NAT_STATE_RUNNING;
}

/****************************** 规则执行 ******************************/

/**
 * @brief 执行当前Fast NAT路径的规则表。
 */
static unsigned int _linkg_fast_nat_run_rules(linkg_fast_nat_path_t path, struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    const linkg_fast_nat_rule_chain_t *chain = &g_fast_nat.rules->paths[path];
    linkg_fast_nat_rule_result_t       result;
    __u32                              index;

    for (index = 0U; index < chain->count; index++)
    {
        result = chain->rules[index](skb, state, iph);

        if (result == LINKG_FAST_NAT_RULE_STOP)
        {
            return NF_ACCEPT;
        }

        if (result == LINKG_FAST_NAT_RULE_DROP)
        {
            return NF_DROP;
        }

        iph = ip_hdr(skb);
    }

    return NF_ACCEPT;
}

/****************************** Netfilter Hook ******************************/

/**
 * @brief Netfilter PRE_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_prerouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    int           ret;

    (void)priv;

    if (!_linkg_fast_nat_is_running())
    {
        return NF_ACCEPT;
    }

    if (state->in == NULL)
    {
        return NF_ACCEPT;
    }

    if (state->in->ifindex != g_fast_nat.config.ethernet_ifindex &&
        state->in->ifindex != g_fast_nat.config.tun_ifindex &&
        (g_fast_nat.config.uplink_ifindex <= 0 ||
         state->in->ifindex != g_fast_nat.config.uplink_ifindex))
    {
        return NF_ACCEPT;
    }

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return NF_ACCEPT;
    }

    if (state->in->ifindex == g_fast_nat.config.ethernet_ifindex)
    {
        return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_ETHERNET_RX, skb, state, iph);
    }

    if (state->in->ifindex == g_fast_nat.config.tun_ifindex)
    {
        return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_TUN_RX, skb, state, iph);
    }

    if (g_fast_nat.config.uplink_ifindex > 0 && state->in->ifindex == g_fast_nat.config.uplink_ifindex)
    {
        return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_UPLINK_RX, skb, state, iph);
    }

    return NF_ACCEPT;
}

/**
 * @brief Netfilter LOCAL_OUT Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_local_out(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    int           ret;

    (void)priv;

    if (!_linkg_fast_nat_is_running())
    {
        return NF_ACCEPT;
    }

    if (g_fast_nat.rules->paths[LINKG_FAST_NAT_PATH_LOCAL_OUT].count == 0U)
    {
        return NF_ACCEPT;
    }

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return NF_ACCEPT;
    }

    return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_LOCAL_OUT, skb, state, iph);
}

/**
 * @brief Netfilter POST_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_postrouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
{
    struct iphdr *iph;
    unsigned int  transport_offset;
    int           ret;

    (void)priv;

    if (!_linkg_fast_nat_is_running())
    {
        return NF_ACCEPT;
    }

    if (state->out == NULL)
    {
        return NF_ACCEPT;
    }

    if (state->out->ifindex != g_fast_nat.config.ethernet_ifindex &&
        state->out->ifindex != g_fast_nat.config.tun_ifindex &&
        (g_fast_nat.config.uplink_ifindex <= 0 ||
         state->out->ifindex != g_fast_nat.config.uplink_ifindex))
    {
        return NF_ACCEPT;
    }

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        if (g_fast_nat.config.uplink_ifindex > 0 &&
            state->out->ifindex == g_fast_nat.config.uplink_ifindex &&
            ret == -EOPNOTSUPP &&
            linkg_fast_nat_uplink_fragment_requires_snat(skb))
        {
            return NF_DROP;
        }

        return NF_ACCEPT;
    }

    if (state->out->ifindex == g_fast_nat.config.ethernet_ifindex)
    {
        return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_ETHERNET_TX, skb, state, iph);
    }

    if (state->out->ifindex == g_fast_nat.config.tun_ifindex)
    {
        return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_TUN_TX, skb, state, iph);
    }

    if (g_fast_nat.config.uplink_ifindex > 0 &&
        state->out->ifindex == g_fast_nat.config.uplink_ifindex)
    {
        return _linkg_fast_nat_run_rules(LINKG_FAST_NAT_PATH_UPLINK_TX, skb, state, iph);
    }

    return NF_ACCEPT;
}

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

    linkg_fast_nat_mapping_reset();
    linkg_fast_nat_flow_reset();

    ret = nf_register_net_hooks(&init_net, g_fast_nat_hooks, ARRAY_SIZE(g_fast_nat_hooks));
    if (ret != 0)
    {
        // 注册失败时内核可能已回滚部分Hook，等待已进入的回调退出。
        synchronize_net();
        return ret;
    }

    linkg_fast_nat_related_start();
    linkg_fast_nat_mapping_gc_start();
    linkg_fast_nat_flow_gc_start();

    // 所有状态准备完毕后再允许Hook处理报文。
    smp_store_release(&g_fast_nat.state, LINKG_FAST_NAT_STATE_RUNNING);

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
    synchronize_net();

    linkg_fast_nat_flow_gc_stop();
    linkg_fast_nat_mapping_gc_stop();
    linkg_fast_nat_related_stop();
    linkg_fast_nat_mapping_reset();
    linkg_fast_nat_flow_reset();

    return 0;
}

/****************************** ioctl ******************************/

/**
 * @brief 设置Fast NAT运行配置。
 */
static int _linkg_fast_nat_set_config(unsigned long arg)
{
    linkg_fast_nat_config_t config;
    linkg_fast_nat_mode_t   mode;
    int                     ret;

    if (copy_from_user(&config, (void __user *)arg, sizeof(config)) != 0U)
    {
        return -EFAULT;
    }

    ret = linkg_fast_nat_config_validate(&config, &mode);
    if (ret != 0)
    {
        return ret;
    }

    if (g_fast_nat.state == LINKG_FAST_NAT_STATE_RUNNING)
    {
        return -EBUSY;
    }

    g_fast_nat.config = config;
    g_fast_nat.rules  = (mode == LINKG_FAST_NAT_MODE_DIRECT) ? linkg_fast_nat_rules_direct_get() : linkg_fast_nat_rules_nat_get();
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
    .owner          = THIS_MODULE,           // 模块所有者
    .unlocked_ioctl = _linkg_fast_nat_ioctl, // 原生ioctl处理
#ifdef CONFIG_COMPAT
    .compat_ioctl   = _linkg_fast_nat_ioctl,    // 兼容ioctl处理
#endif
};

static struct miscdevice g_fast_nat_misc_device =
{
    .minor = MISC_DYNAMIC_MINOR,         // 动态分配次设备号
    .name  = LINKG_FAST_NAT_DEVICE_NAME, // 字符设备名称
    .fops  = &g_fast_nat_file_operations // 字符设备操作集
};

/****************************** 模块入口 ******************************/

/**
 * @brief 初始化LinkG Fast NAT内核模块。
 */
static int __init _linkg_fast_nat_module_init(void)
{
    int ret;

    linkg_fast_nat_flow_init();
    linkg_fast_nat_mapping_init();
    linkg_fast_nat_related_init();

    ret = misc_register(&g_fast_nat_misc_device);
    if (ret != 0)
    {
        linkg_fast_nat_related_deinit();
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
    linkg_fast_nat_related_deinit();

    pr_info("LinkG Fast NAT: module unloaded\n");
}

module_init(_linkg_fast_nat_module_init);
module_exit(_linkg_fast_nat_module_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Dawn");
MODULE_DESCRIPTION("LinkG lightweight high-performance IPv4 NAT");
MODULE_VERSION("1.0.0");
