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
#include <linux/jiffies.h>
#include <linux/timer.h>
#include <net/checksum.h>
#include <net/ip.h>
#include <net/net_namespace.h>
#include <net/netfilter/nf_conntrack.h>
#include <net/route.h>

#include "linkg_fast_nat_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_FAST_NAT_SLOT_COUNT             4096U                                                       // SNAT Hash槽位数量，必须为2的幂
#define LINKG_FAST_NAT_SLOT_MASK              (LINKG_FAST_NAT_SLOT_COUNT - 1U)                            // SNAT Hash槽位掩码
#define LINKG_FAST_NAT_ACTIVE_MAX             2000U                                                       // 最大活动SNAT映射数量
#define LINKG_FAST_NAT_HASH_SEED              0x4C4E4154U                                                 // "LNAT"固定Hash种子
#define LINKG_FAST_NAT_MAPPING_FAST_COUNT     4U                                                          // SNAT映射快速缓存槽位数量，必须为2的幂
#define LINKG_FAST_NAT_MAPPING_FAST_MASK      (LINKG_FAST_NAT_MAPPING_FAST_COUNT - 1U)                    // SNAT映射快速缓存槽位掩码
#define LINKG_FAST_NAT_MAPPING_SLOT_INVALID   0xFFFFFFFFU                                                 // 无效SNAT映射Hash槽位
#define LINKG_FAST_NAT_MAPPING_REFRESH_TIME   (1U * HZ)                                                   // 活动Mapping最近命中时间最小刷新周期
#define LINKG_FAST_NAT_MAPPING_TIMEOUT        (60U * HZ)                                                  // Mapping无流量超时时间
#define LINKG_FAST_NAT_MAPPING_GC_INTERVAL    (6U * HZ)                                                   // Mapping超时扫描周期

#define LINKG_FAST_NAT_FLOW_SLOT_COUNT        4096U                                                       // Ethernet主动流Hash槽位数量，必须为2的幂
#define LINKG_FAST_NAT_FLOW_SLOT_MASK         (LINKG_FAST_NAT_FLOW_SLOT_COUNT - 1U)                       // Ethernet主动流Hash槽位掩码
#define LINKG_FAST_NAT_FLOW_ACTIVE_MAX        2000U                                                       // 最大活动Ethernet主动流数量
#define LINKG_FAST_NAT_FLOW_HASH_SEED         0x4C464C57U                                                 // "LFLW"固定Hash种子
#define LINKG_FAST_NAT_FLOW_FAST_COUNT        4U                                                          // Ethernet主动流快速缓存槽位数量，必须为2的幂
#define LINKG_FAST_NAT_FLOW_FAST_MASK         (LINKG_FAST_NAT_FLOW_FAST_COUNT - 1U)                       // Ethernet主动流快速缓存槽位掩码
#define LINKG_FAST_NAT_FLOW_SLOT_INVALID      0xFFFFFFFFU                                                 // 无效Flow Hash槽位
#define LINKG_FAST_NAT_FLOW_REFRESH_TIME      (1U  * HZ)                                                  // 活动Flow最近命中时间最小刷新周期
#define LINKG_FAST_NAT_FLOW_TIMEOUT           (60U * HZ)                                                  // Flow无流量超时时间
#define LINKG_FAST_NAT_FLOW_GC_INTERVAL       (5U  * HZ)                                                  // Flow超时扫描周期

#define LINKG_FAST_NAT_MARK_HAIRPIN           0x80000000U                                                 // 当前skb由本地Ethernet DNAT产生，需要Hairpin SNAT

#define LINKG_FAST_NAT_PRIORITY_EARLY         (NF_IP_PRI_RAW + 50)                                        // conntrack之前执行
#define LINKG_FAST_NAT_PRIORITY_POSTROUTING   (NF_IP_PRI_NAT_SRC - 10)                                    // 常规Source NAT之前执行


/****************************** SNAT定义 ******************************/

#define LINKG_FAST_NAT_FLOW_FREE            0U // Flow槽位从未使用
#define LINKG_FAST_NAT_FLOW_ACTIVE          1U // Flow槽位当前有效
#define LINKG_FAST_NAT_FLOW_TOMBSTONE       2U // Flow已超时，可被新Flow复用

#define LINKG_FAST_NAT_ENTRY_FREE           0U // SNAT映射槽未使用
#define LINKG_FAST_NAT_ENTRY_ACTIVE         1U // SNAT映射槽有效
#define LINKG_FAST_NAT_ENTRY_TOMBSTONE      2U // SNAT映射槽已删除但需保持Hash探测链

#define LINKG_FAST_NAT_SNAT_VIRTUAL         1U // 虚拟Endpoint访问Ethernet
#define LINKG_FAST_NAT_SNAT_TUN             2U // TUN节点访问Ethernet
#define LINKG_FAST_NAT_SNAT_HAIRPIN         3U // Ethernet Hairpin访问

typedef enum
{
    LINKG_FAST_NAT_RULE_CONTINUE = 0, // 当前规则不终止Fast NAT规则链，继续后续规则
    LINKG_FAST_NAT_RULE_DONE,         // 当前规则完成处理，结束Fast NAT规则链
    LINKG_FAST_NAT_RULE_DROP          // 当前规则处理失败，丢弃数据包
} linkg_fast_nat_rule_result_t;

typedef linkg_fast_nat_rule_result_t (*linkg_fast_nat_rule_func_t)(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);

/**
 * @brief Fast NAT SNAT映射标准Key。
 *
 * Key始终保存执行SNAT前的原始五元组，
 * 用于正向报文查找和复用已有SNAT映射。
 */
typedef struct
{
    __be32 original_ip; // SNAT前源IPv4地址
    __be32 remote_ip;   // SNAT前目的IPv4地址
    __be16 original_id; // SNAT前源TCP/UDP端口或ICMP Echo Identifier
    __be16 remote_id;   // SNAT前目的TCP/UDP端口或ICMP Echo Identifier
    __u8   protocol;    // IPv4上层协议
} linkg_fast_nat_mapping_key_t;

/**
 * @brief Fast NAT SNAT映射项。
 *
 * translated ID不单独保存，其值固定为：
 * config.snat_port_start + 当前entry数组下标。
 *
 * 映射运行期由Mapping GC执行超时回收，删除槽位进入TOMBSTONE状态，
 * 保持开放寻址Hash的probe链连续并允许后续新Mapping复用。
 */
typedef struct
{
    linkg_fast_nat_mapping_key_t key;       // SNAT前原始五元组
    unsigned long                last_seen; // 最近一次活动时间
    __u8                         snat_type; // SNAT来源类型
    __u8                         state;     // LINKG_FAST_NAT_ENTRY_*
} linkg_fast_nat_entry_t;

/**
 * @brief Ethernet主动访问Virtual网络的标准流Key。
 */
typedef struct
{
    __be32 ethernet_ip; // Ethernet侧IPv4
    __be32 virtual_ip;  // Virtual侧IPv4
    __be16 ethernet_id; // Ethernet侧TCP/UDP端口或ICMP Echo Identifier
    __be16 virtual_id;  // Virtual侧TCP/UDP端口或ICMP Echo Identifier
    __u8   protocol;    // IPv4上层协议
} linkg_fast_nat_flow_key_t;

/**
 * @brief Ethernet主动访问Virtual网络的流方向项。
 *
 * Key始终按照Ethernet -> Virtual方向保存，
 * 正向流和返回流均规范化为相同Key后查询。
 */
typedef struct
{
    linkg_fast_nat_flow_key_t key;       // 标准Ethernet -> Virtual流Key
    unsigned long             last_seen; // 最近一次活动时间
    __u8                      state;     // LINKG_FAST_NAT_FLOW_*
} linkg_fast_nat_flow_entry_t;

/**
 * @brief Fast NAT运行上下文。
 */
typedef struct
{
    struct mutex                control_lock;                                          // 控制面生命周期锁
    spinlock_t                  mapping_lock;                                          // SNAT映射表写锁
    spinlock_t                  flow_lock;                                             // Ethernet主动流表写锁
    struct timer_list           mapping_gc_timer;                                      // SNAT映射老化定时器
    struct timer_list           flow_gc_timer;                                         // Ethernet主动流老化定时器
    linkg_fast_nat_config_t     config;                                                // RUNNING期间只读配置

    linkg_fast_nat_entry_t      mappings[LINKG_FAST_NAT_SLOT_COUNT];                   // SNAT映射Hash表，数组下标对应translated ID
    __u32                       fast_mapping_slots[LINKG_FAST_NAT_MAPPING_FAST_COUNT]; // 高频Mapping对应的Hash槽位
    __u32                       fast_mapping_last_hit;                                 // 最近一次命中的Mapping快速缓存槽位
    __u32                       fast_mapping_next_replace;                             // 下一次Mapping快速缓存替换槽位
    __u32                       mapping_count;                                         // 当前活动SNAT映射数量

    linkg_fast_nat_flow_entry_t flows[LINKG_FAST_NAT_FLOW_SLOT_COUNT];                 // Ethernet主动Virtual流Hash表
    __u32                       fast_flow_slots[LINKG_FAST_NAT_FLOW_FAST_COUNT];       // 高频Flow对应的Hash槽位
    __u32                       fast_flow_last_hit;                                    // 最近一次命中的Flow快速缓存槽位
    __u32                       fast_flow_next_replace;                                // 下一次Flow快速缓存替换槽位
    __u32                       flow_count;                                            // 当前活动Ethernet主动流数量

    __u32                       state;                                                 // LINKG_FAST_NAT_STATE_*
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

static linkg_fast_nat_rule_result_t _linkg_fast_nat_prerouting_reverse_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
static linkg_fast_nat_rule_result_t _linkg_fast_nat_prerouting_ethernet_flow_record(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
static linkg_fast_nat_rule_result_t _linkg_fast_nat_prerouting_linkg_to_ethernet(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
static linkg_fast_nat_rule_result_t _linkg_fast_nat_postrouting_ethernet_to_linkg(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);
static linkg_fast_nat_rule_result_t _linkg_fast_nat_postrouting_source_translate(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph);

/****************************** Fast NAT规则表 ******************************/

static const linkg_fast_nat_rule_func_t g_prerouting_rules[] =
{
    _linkg_fast_nat_prerouting_reverse_snat,
    _linkg_fast_nat_prerouting_ethernet_flow_record,
    _linkg_fast_nat_prerouting_linkg_to_ethernet,
};

static const linkg_fast_nat_rule_func_t g_local_out_rules[] =
{
};

static const linkg_fast_nat_rule_func_t g_postrouting_rules[] =
{
    _linkg_fast_nat_postrouting_ethernet_to_linkg,
    _linkg_fast_nat_postrouting_source_translate,
};

/****************************** SNAT映射 ******************************/

/**
 * @brief 清空全部SNAT映射状态。
 *
 * 仅在Mapping老化定时器和Netfilter hooks停止后调用。
 */
static void _linkg_fast_nat_mapping_reset(void)
{
    memset(g_fast_nat.mappings, 0, sizeof(g_fast_nat.mappings));
    memset(g_fast_nat.fast_mapping_slots, 0xFF, sizeof(g_fast_nat.fast_mapping_slots));

    g_fast_nat.fast_mapping_last_hit     = 0U;
    g_fast_nat.fast_mapping_next_replace = 0U;
    g_fast_nat.mapping_count             = 0U;
}

/**
 * @brief 判断两个SNAT映射Key是否一致。
 */
static bool _linkg_fast_nat_mapping_key_equal(const linkg_fast_nat_mapping_key_t *left, const linkg_fast_nat_mapping_key_t *right)
{
    return left->protocol    == right->protocol &&
           left->original_ip == right->original_ip &&
           left->original_id == right->original_id &&
           left->remote_ip   == right->remote_ip &&
           left->remote_id   == right->remote_id;
}

/**
 * @brief 计算SNAT映射对应的初始Hash槽位。
 */
static __u32 _linkg_fast_nat_mapping_hash(const linkg_fast_nat_mapping_key_t *key)
{
    __u32 ids;
    __u32 hash;

    ids = ((__u32)ntohs(key->original_id) << 16U) | (__u32)ntohs(key->remote_id);

    hash = jhash_3words((__force __u32)key->original_ip,
                        (__force __u32)key->remote_ip,
                        ids,
                        LINKG_FAST_NAT_HASH_SEED ^ (__u32)key->protocol);

    return hash & LINKG_FAST_NAT_SLOT_MASK;
}

/**
 * @brief 按最小刷新周期更新Mapping最近活动时间。
 *
 * 高频数据包只读取last_seen，达到刷新周期后才进入写锁更新，
 * 从而避免每个数据包都写共享Mapping状态。
 */
static int _linkg_fast_nat_mapping_refresh(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u32 slot)
{
    linkg_fast_nat_entry_t *entry;
    unsigned long           flags;
    unsigned long           last_seen;
    unsigned long           now;

    if (key == NULL || slot >= LINKG_FAST_NAT_SLOT_COUNT)
    {
        return -EINVAL;
    }

    entry     = &g_fast_nat.mappings[slot];
    now       = jiffies;
    last_seen = READ_ONCE(entry->last_seen);

    if (!time_after_eq(now, last_seen + LINKG_FAST_NAT_MAPPING_REFRESH_TIME))
    {
        return 0;
    }

    spin_lock_irqsave(&g_fast_nat.mapping_lock, flags);

    if (entry->state != LINKG_FAST_NAT_ENTRY_ACTIVE ||
        entry->snat_type != snat_type ||
        !_linkg_fast_nat_mapping_key_equal(&entry->key, key))
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOENT;
    }

    entry->last_seen = now;

    spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);

    return 0;
}

/**
 * @brief 从快速缓存查找SNAT映射。
 */
static int _linkg_fast_nat_mapping_fast_lookup(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u32 *slot)
{
    linkg_fast_nat_entry_t *entry;
    __u32                   cached_slot;
    __u32                   start;
    __u32                   offset;
    __u32                   index;

    if (key == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    start = READ_ONCE(g_fast_nat.fast_mapping_last_hit);

    for (offset = 0U; offset < LINKG_FAST_NAT_MAPPING_FAST_COUNT; offset++)
    {
        index       = (start + offset) & LINKG_FAST_NAT_MAPPING_FAST_MASK;
        cached_slot = READ_ONCE(g_fast_nat.fast_mapping_slots[index]);

        if (cached_slot == LINKG_FAST_NAT_MAPPING_SLOT_INVALID || cached_slot >= LINKG_FAST_NAT_SLOT_COUNT)
        {
            continue;
        }

        entry = &g_fast_nat.mappings[cached_slot];

        if (smp_load_acquire(&entry->state) != LINKG_FAST_NAT_ENTRY_ACTIVE)
        {
            continue;
        }

        if (READ_ONCE(entry->snat_type) != snat_type || !_linkg_fast_nat_mapping_key_equal(&entry->key, key))
        {
            continue;
        }

        if (index != start)
        {
            WRITE_ONCE(g_fast_nat.fast_mapping_last_hit, index);
        }

        *slot = cached_slot;
        return 0;
    }

    return -ENOENT;
}

/**
 * @brief 将Mapping Hash槽位写入快速缓存。
 *
 * 快速缓存仅作为查询提示，竞争覆盖不会影响Hash主表正确性。
 */
static void _linkg_fast_nat_mapping_fast_update(__u32 slot)
{
    __u32 index;

    if (slot >= LINKG_FAST_NAT_SLOT_COUNT)
    {
        return;
    }

    index = READ_ONCE(g_fast_nat.fast_mapping_next_replace);

    WRITE_ONCE(g_fast_nat.fast_mapping_slots[index], slot);
    WRITE_ONCE(g_fast_nat.fast_mapping_last_hit, index);
    WRITE_ONCE(g_fast_nat.fast_mapping_next_replace, (index + 1U) & LINKG_FAST_NAT_MAPPING_FAST_MASK);
}

/**
 * @brief 无锁查找SNAT映射Hash主表。
 */
static int _linkg_fast_nat_mapping_hash_lookup(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u32 *slot)
{
    linkg_fast_nat_entry_t *entry;
    __u32                   current_slot;
    __u32                   probe;
    __u8                    state;

    if (key == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    current_slot = _linkg_fast_nat_mapping_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.mappings[current_slot];
        state = smp_load_acquire(&entry->state);

        if (state == LINKG_FAST_NAT_ENTRY_FREE)
        {
            return -ENOENT;
        }

        if (state == LINKG_FAST_NAT_ENTRY_ACTIVE &&
            READ_ONCE(entry->snat_type) == snat_type &&
            _linkg_fast_nat_mapping_key_equal(&entry->key, key))
        {
            *slot = current_slot;
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_SLOT_MASK;
    }

    return -ENOENT;
}

/**
 * @brief 查找SNAT映射。
 *
 * 优先查询4项快速缓存，未命中时查询Hash主表并回填快速缓存。
 */
static int _linkg_fast_nat_mapping_lookup(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u32 *slot)
{
    int ret;

    ret = _linkg_fast_nat_mapping_fast_lookup(key, snat_type, slot);
    if (ret == 0)
    {
        ret = _linkg_fast_nat_mapping_refresh(key, snat_type, *slot);
        if (ret == 0)
        {
            return 0;
        }

        if (ret != -ENOENT)
        {
            return ret;
        }
    }
    else if (ret != -ENOENT)
    {
        return ret;
    }

    ret = _linkg_fast_nat_mapping_hash_lookup(key, snat_type, slot);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_mapping_refresh(key, snat_type, *slot);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_fast_nat_mapping_fast_update(*slot);

    return 0;
}

/**
 * @brief 获取已有SNAT映射或创建新映射。
 */
static int _linkg_fast_nat_mapping_get_or_create(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u32 *slot)
{
    linkg_fast_nat_entry_t *entry;
    unsigned long           flags;
    __u32                   insert_slot;
    __u32                   current_slot;
    __u32                   probe;
    __u32                   found_slot;
    __u8                    state;
    int                     ret;

    if (key == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    if (snat_type != LINKG_FAST_NAT_SNAT_VIRTUAL &&
        snat_type != LINKG_FAST_NAT_SNAT_TUN &&
        snat_type != LINKG_FAST_NAT_SNAT_HAIRPIN)
    {
        return -EINVAL;
    }

    ret = _linkg_fast_nat_mapping_lookup(key, snat_type, &found_slot);
    if (ret == 0)
    {
        *slot = found_slot;
        return 0;
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    spin_lock_irqsave(&g_fast_nat.mapping_lock, flags);

    ret = _linkg_fast_nat_mapping_hash_lookup(key, snat_type, &found_slot);
    if (ret == 0)
    {
        g_fast_nat.mappings[found_slot].last_seen = jiffies;

        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);

        _linkg_fast_nat_mapping_fast_update(found_slot);

        *slot = found_slot;
        return 0;
    }

    if (ret != -ENOENT)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return ret;
    }

    if (g_fast_nat.mapping_count >= LINKG_FAST_NAT_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOSPC;
    }

    insert_slot  = LINKG_FAST_NAT_MAPPING_SLOT_INVALID;
    current_slot = _linkg_fast_nat_mapping_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.mappings[current_slot];
        state = entry->state;

        if (state == LINKG_FAST_NAT_ENTRY_TOMBSTONE || state == LINKG_FAST_NAT_ENTRY_FREE)
        {
            insert_slot = current_slot;
            break;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_SLOT_MASK;
    }

    if (insert_slot == LINKG_FAST_NAT_MAPPING_SLOT_INVALID)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOSPC;
    }

    entry = &g_fast_nat.mappings[insert_slot];

    entry->key       = *key;
    entry->last_seen = jiffies;
    entry->snat_type = snat_type;

    smp_store_release(&entry->state, LINKG_FAST_NAT_ENTRY_ACTIVE);

    g_fast_nat.mapping_count++;

    spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);

    _linkg_fast_nat_mapping_fast_update(insert_slot);

    *slot = insert_slot;
    return 0;
}

/**
 * @brief 根据translated ID对应槽位反查SNAT映射。
 *
 * 回包通过translated ID直接定位Mapping槽位，不执行Hash查询。
 * TCP/UDP额外校验远端源端口；ICMP Echo只有Identifier，
 * translated Identifier已经用于定位槽位，因此仅校验协议和远端IPv4地址。
 */
static int _linkg_fast_nat_mapping_reverse_lookup(__u32 slot,
                                                  __u8 protocol,
                                                  __be32 remote_ip,
                                                  __be16 remote_id,
                                                  __be32 *original_ip,
                                                  __be16 *original_id)
{
    linkg_fast_nat_entry_t *entry;
    unsigned long           flags;
    unsigned long           last_seen;
    unsigned long           now;
    bool                    check_remote_id;

    if (slot >= LINKG_FAST_NAT_SLOT_COUNT || original_ip == NULL || original_id == NULL)
    {
        return -EINVAL;
    }

    entry = &g_fast_nat.mappings[slot];

    if (smp_load_acquire(&entry->state) != LINKG_FAST_NAT_ENTRY_ACTIVE)
    {
        return -ENOENT;
    }

    if (READ_ONCE(entry->key.protocol) != protocol)
    {
        return -ENOENT;
    }

    if (READ_ONCE(entry->key.remote_ip) != remote_ip)
    {
        return -ENOENT;
    }

    check_remote_id = protocol == IPPROTO_TCP || protocol == IPPROTO_UDP;
    if (check_remote_id && READ_ONCE(entry->key.remote_id) != remote_id)
    {
        return -ENOENT;
    }

    now       = jiffies;
    last_seen = READ_ONCE(entry->last_seen);

    if (!time_after_eq(now, last_seen + LINKG_FAST_NAT_MAPPING_REFRESH_TIME))
    {
        *original_ip = READ_ONCE(entry->key.original_ip);
        *original_id = READ_ONCE(entry->key.original_id);

        if (smp_load_acquire(&entry->state) != LINKG_FAST_NAT_ENTRY_ACTIVE)
        {
            return -ENOENT;
        }

        if (READ_ONCE(entry->key.protocol) != protocol || READ_ONCE(entry->key.remote_ip) != remote_ip)
        {
            return -ENOENT;
        }

        if (check_remote_id && READ_ONCE(entry->key.remote_id) != remote_id)
        {
            return -ENOENT;
        }

        return 0;
    }

    spin_lock_irqsave(&g_fast_nat.mapping_lock, flags);

    if (entry->state != LINKG_FAST_NAT_ENTRY_ACTIVE)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOENT;
    }

    if (entry->key.protocol != protocol || entry->key.remote_ip != remote_ip)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOENT;
    }

    if (check_remote_id && entry->key.remote_id != remote_id)
    {
        spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);
        return -ENOENT;
    }

    *original_ip = entry->key.original_ip;
    *original_id = entry->key.original_id;
    entry->last_seen = now;

    spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);

    return 0;
}

/**
 * @brief 周期回收超时SNAT映射。
 */
static void _linkg_fast_nat_mapping_gc_timer(struct timer_list *timer)
{
    linkg_fast_nat_entry_t *entry;
    unsigned long           flags;
    unsigned long           now;
    __u32                   index;

    (void)timer;

    if (READ_ONCE(g_fast_nat.state) != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return;
    }

    if (READ_ONCE(g_fast_nat.mapping_count) == 0U)
    {
        goto rearm;
    }

    now = jiffies;

    spin_lock_irqsave(&g_fast_nat.mapping_lock, flags);

    for (index = 0U; index < LINKG_FAST_NAT_SLOT_COUNT; index++)
    {
        entry = &g_fast_nat.mappings[index];

        if (entry->state != LINKG_FAST_NAT_ENTRY_ACTIVE)
        {
            continue;
        }

        if (!time_after_eq(now, READ_ONCE(entry->last_seen) + LINKG_FAST_NAT_MAPPING_TIMEOUT))
        {
            continue;
        }

        smp_store_release(&entry->state, LINKG_FAST_NAT_ENTRY_TOMBSTONE);

        if (g_fast_nat.mapping_count > 0U)
        {
            g_fast_nat.mapping_count--;
        }
    }

    spin_unlock_irqrestore(&g_fast_nat.mapping_lock, flags);

rearm:
    if (READ_ONCE(g_fast_nat.state) == LINKG_FAST_NAT_STATE_RUNNING)
    {
        mod_timer(&g_fast_nat.mapping_gc_timer, jiffies + LINKG_FAST_NAT_MAPPING_GC_INTERVAL);
    }
}


/****************************** Ethernet主动流表 ******************************/

/**
 * @brief 清空全部Ethernet主动Virtual流状态。
 *
 * 仅在Flow老化定时器和Netfilter hooks停止后调用。
 */
static void _linkg_fast_nat_flow_reset(void)
{
    memset(g_fast_nat.flows, 0, sizeof(g_fast_nat.flows));
    memset(g_fast_nat.fast_flow_slots, 0xFF, sizeof(g_fast_nat.fast_flow_slots));

    g_fast_nat.fast_flow_last_hit     = 0U;
    g_fast_nat.fast_flow_next_replace = 0U;
    g_fast_nat.flow_count             = 0U;
}

/**
 * @brief 判断两个Ethernet主动Virtual流Key是否一致。
 */
static bool _linkg_fast_nat_flow_key_equal(const linkg_fast_nat_flow_key_t *left, const linkg_fast_nat_flow_key_t *right)
{
    return left->protocol    == right->protocol &&
           left->ethernet_ip == right->ethernet_ip &&
           left->ethernet_id == right->ethernet_id &&
           left->virtual_ip  == right->virtual_ip &&
           left->virtual_id  == right->virtual_id;
}

/**
 * @brief 计算Ethernet主动Virtual流对应的初始Hash槽位。
 */
static __u32 _linkg_fast_nat_flow_hash(const linkg_fast_nat_flow_key_t *key)
{
    __u32 ids;
    __u32 hash;

    ids = ((__u32)ntohs(key->ethernet_id) << 16U) | (__u32)ntohs(key->virtual_id);

    hash = jhash_3words((__force __u32)key->ethernet_ip,
                        (__force __u32)key->virtual_ip,
                        ids,
                        LINKG_FAST_NAT_FLOW_HASH_SEED ^ (__u32)key->protocol);

    return hash & LINKG_FAST_NAT_FLOW_SLOT_MASK;
}

/**
 * @brief 按最小刷新周期更新Flow最近活动时间。
 *
 * 高频数据包只读取last_seen，达到刷新周期后才进入写锁更新，
 * 从而避免每个数据包都写共享Flow状态。
 */
static int _linkg_fast_nat_flow_refresh(const linkg_fast_nat_flow_key_t *key, __u32 slot)
{
    linkg_fast_nat_flow_entry_t *entry;
    unsigned long                flags;
    unsigned long                last_seen;
    unsigned long                now;

    if (key == NULL || slot >= LINKG_FAST_NAT_FLOW_SLOT_COUNT)
    {
        return -EINVAL;
    }

    entry     = &g_fast_nat.flows[slot];
    now       = jiffies;
    last_seen = READ_ONCE(entry->last_seen);

    if (!time_after_eq(now, last_seen + LINKG_FAST_NAT_FLOW_REFRESH_TIME))
    {
        return 0;
    }

    spin_lock_irqsave(&g_fast_nat.flow_lock, flags);

    if (entry->state != LINKG_FAST_NAT_FLOW_ACTIVE || !_linkg_fast_nat_flow_key_equal(&entry->key, key))
    {
        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
        return -ENOENT;
    }

    entry->last_seen = now;

    spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);

    return 0;
}

/**
 * @brief 从快速缓存查找Ethernet主动Virtual流。
 */
static int _linkg_fast_nat_flow_fast_lookup(const linkg_fast_nat_flow_key_t *key, __u32 *slot)
{
    linkg_fast_nat_flow_entry_t *entry;
    __u32                        cached_slot;
    __u32                        start;
    __u32                        offset;
    __u32                        index;

    if (key == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    start = READ_ONCE(g_fast_nat.fast_flow_last_hit);

    for (offset = 0U; offset < LINKG_FAST_NAT_FLOW_FAST_COUNT; offset++)
    {
        index       = (start + offset) & LINKG_FAST_NAT_FLOW_FAST_MASK;
        cached_slot = READ_ONCE(g_fast_nat.fast_flow_slots[index]);

        if (cached_slot == LINKG_FAST_NAT_FLOW_SLOT_INVALID)
        {
            continue;
        }

        entry = &g_fast_nat.flows[cached_slot];

        if (smp_load_acquire(&entry->state) != LINKG_FAST_NAT_FLOW_ACTIVE)
        {
            continue;
        }

        if (!_linkg_fast_nat_flow_key_equal(&entry->key, key))
        {
            continue;
        }

        if (index != start)
        {
            WRITE_ONCE(g_fast_nat.fast_flow_last_hit, index);
        }

        *slot = cached_slot;
        return 0;
    }

    return -ENOENT;
}

/**
 * @brief 将Flow Hash槽位写入快速缓存。
 *
 * 快速缓存仅作为查询提示，竞争覆盖不会影响Hash主表正确性。
 */
static void _linkg_fast_nat_flow_fast_update(__u32 slot)
{
    __u32 index;

    if (slot >= LINKG_FAST_NAT_FLOW_SLOT_COUNT)
    {
        return;
    }

    index = READ_ONCE(g_fast_nat.fast_flow_next_replace);

    WRITE_ONCE(g_fast_nat.fast_flow_slots[index], slot);
    WRITE_ONCE(g_fast_nat.fast_flow_last_hit, index);
    WRITE_ONCE(g_fast_nat.fast_flow_next_replace, (index + 1U) & LINKG_FAST_NAT_FLOW_FAST_MASK);
}

/**
 * @brief 无锁查找Ethernet主动Virtual流Hash主表。
 */
static int _linkg_fast_nat_flow_hash_lookup(const linkg_fast_nat_flow_key_t *key, __u32 *slot)
{
    linkg_fast_nat_flow_entry_t *entry;
    __u32                        current_slot;
    __u32                        probe;
    __u8                         state;

    if (key == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    current_slot = _linkg_fast_nat_flow_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_FLOW_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.flows[current_slot];
        state = smp_load_acquire(&entry->state);

        if (state == LINKG_FAST_NAT_FLOW_FREE)
        {
            return -ENOENT;
        }

        if (state == LINKG_FAST_NAT_FLOW_ACTIVE && _linkg_fast_nat_flow_key_equal(&entry->key, key))
        {
            *slot = current_slot;
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_FLOW_SLOT_MASK;
    }

    return -ENOENT;
}

/**
 * @brief 查找Ethernet主动Virtual流。
 *
 * 优先查询4项快速缓存，未命中时查询Hash主表并回填快速缓存。
 */
static int _linkg_fast_nat_flow_lookup(const linkg_fast_nat_flow_key_t *key, __u32 *slot)
{
    int ret;

    ret = _linkg_fast_nat_flow_fast_lookup(key, slot);
    if (ret == 0)
    {
        return _linkg_fast_nat_flow_refresh(key, *slot);
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    ret = _linkg_fast_nat_flow_hash_lookup(key, slot);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_flow_refresh(key, *slot);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_fast_nat_flow_fast_update(*slot);

    return 0;
}

/**
 * @brief 获取已有Ethernet主动Virtual流或创建新流。
 */
static int _linkg_fast_nat_flow_get_or_create(const linkg_fast_nat_flow_key_t *key)
{
    linkg_fast_nat_flow_entry_t *entry;
    unsigned long                flags;
    __u32                        insert_slot;
    __u32                        current_slot;
    __u32                        probe;
    __u32                        slot;
    __u8                         state;
    int                          ret;

    if (key == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_fast_nat_flow_lookup(key, &slot);
    if (ret == 0)
    {
        return 0;
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    spin_lock_irqsave(&g_fast_nat.flow_lock, flags);

    ret = _linkg_fast_nat_flow_hash_lookup(key, &slot);
    if (ret == 0)
    {
        g_fast_nat.flows[slot].last_seen = jiffies;

        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);

        _linkg_fast_nat_flow_fast_update(slot);

        return 0;
    }

    if (ret != -ENOENT)
    {
        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
        return ret;
    }

    if (g_fast_nat.flow_count >= LINKG_FAST_NAT_FLOW_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
        return -ENOSPC;
    }

    insert_slot  = LINKG_FAST_NAT_FLOW_SLOT_INVALID;
    current_slot = _linkg_fast_nat_flow_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_FLOW_SLOT_COUNT; probe++)
    {
        entry = &g_fast_nat.flows[current_slot];
        state = entry->state;

        if (state == LINKG_FAST_NAT_FLOW_TOMBSTONE || state == LINKG_FAST_NAT_FLOW_FREE)
        {
            insert_slot = current_slot;
            break;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_FLOW_SLOT_MASK;
    }

    if (insert_slot == LINKG_FAST_NAT_FLOW_SLOT_INVALID)
    {
        spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);
        return -ENOSPC;
    }

    entry = &g_fast_nat.flows[insert_slot];

    entry->key       = *key;
    entry->last_seen = jiffies;

    smp_store_release(&entry->state, LINKG_FAST_NAT_FLOW_ACTIVE);

    g_fast_nat.flow_count++;

    spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);

    _linkg_fast_nat_flow_fast_update(insert_slot);

    return 0;
}

/**
 * @brief 周期回收超时Ethernet主动Virtual流。
 */
static void _linkg_fast_nat_flow_gc_timer(struct timer_list *timer)
{
    linkg_fast_nat_flow_entry_t *entry;
    unsigned long                flags;
    unsigned long                now;
    __u32                        index;

    (void)timer;

    if (READ_ONCE(g_fast_nat.state) != LINKG_FAST_NAT_STATE_RUNNING)
    {
        return;
    }

    if (READ_ONCE(g_fast_nat.flow_count) == 0U)
    {
        goto rearm;
    }

    now = jiffies;

    spin_lock_irqsave(&g_fast_nat.flow_lock, flags);

    for (index = 0U; index < LINKG_FAST_NAT_FLOW_SLOT_COUNT; index++)
    {
        entry = &g_fast_nat.flows[index];

        if (entry->state != LINKG_FAST_NAT_FLOW_ACTIVE)
        {
            continue;
        }

        if (!time_after_eq(now, READ_ONCE(entry->last_seen) + LINKG_FAST_NAT_FLOW_TIMEOUT))
        {
            continue;
        }

        smp_store_release(&entry->state, LINKG_FAST_NAT_FLOW_TOMBSTONE);

        if (g_fast_nat.flow_count > 0U)
        {
            g_fast_nat.flow_count--;
        }
    }

    spin_unlock_irqrestore(&g_fast_nat.flow_lock, flags);

rearm:
    if (READ_ONCE(g_fast_nat.state) == LINKG_FAST_NAT_STATE_RUNNING)
    {
        mod_timer(&g_fast_nat.flow_gc_timer, jiffies + LINKG_FAST_NAT_FLOW_GC_INTERVAL);
    }
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
 * @brief 处理Ethernet入口的Fast NAT SNAT返回报文。
 *
 * 仅处理目的IPv4为本节点Ethernet地址、且目的ID落在SNAT translated ID范围内的报文。
 * translated ID直接映射到Mapping数组槽位，验证远端五元组后恢复原始目的IPv4和目的ID。
 * 未命中有效Mapping时继续正常协议栈处理，不影响本机Ethernet服务。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_prerouting_reverse_snat(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    unsigned int transport_offset;
    __be32       original_ip;
    __be16       source_id;
    __be16       destination_id;
    __be16       original_id;
    __u32        translated_id;
    __u32        slot;
    int          ret;

    if (state->in == NULL || state->in->ifindex != g_fast_nat.config.ethernet_ifindex)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (iph->daddr != g_fast_nat.config.ethernet_ip)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &destination_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    translated_id = (__u32)ntohs(destination_id);

    if (translated_id < (__u32)g_fast_nat.config.snat_port_start)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    slot = translated_id - (__u32)g_fast_nat.config.snat_port_start;
    if (slot >= LINKG_FAST_NAT_SLOT_COUNT)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &source_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = _linkg_fast_nat_mapping_reverse_lookup(slot,
                                                 iph->protocol,
                                                 iph->saddr,
                                                 source_id,
                                                 &original_ip,
                                                 &original_id);
    if (ret == -ENOENT)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_replace_tuple(skb, false, original_ip, original_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_DONE;
}

/**
 * @brief 处理LinkG流量访问本地Ethernet地址的Destination NETMAP规则。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_prerouting_linkg_to_ethernet(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    uint32_t host_mask;
    uint32_t host;
    int      ret;

    if (state->in == NULL || state->in->ifindex != g_fast_nat.config.tun_ifindex)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.local_virtual_subnet))
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    host_mask = ~ntohl(g_fast_nat.config.local_virtual_subnet.netmask);
    host      =  ntohl(iph->daddr) & host_mask;

    if (host == 1U)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    ret = linkg_fast_nat_destination_netmap(skb, &g_fast_nat.config.local_virtual_subnet, &g_fast_nat.config.ethernet_network);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_DONE;
}

/**
 * @brief 记录Ethernet主动访问Virtual网络的原始流。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_prerouting_ethernet_flow_record(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    linkg_fast_nat_flow_key_t key;
    unsigned int              transport_offset;
    int                       ret;

    if (state->in == NULL || state->in->ifindex != g_fast_nat.config.ethernet_ifindex)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network) ||
        !linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.virtual_network))
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    memset(&key, 0, sizeof(key));

    key.ethernet_ip = iph->saddr;
    key.virtual_ip  = iph->daddr;
    key.protocol    = iph->protocol;

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &key.ethernet_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &key.virtual_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = _linkg_fast_nat_flow_get_or_create(&key);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_CONTINUE;
}

/**
 * @brief Netfilter PRE_ROUTING Fast NAT处理。
 */
static unsigned int _linkg_fast_nat_prerouting(void *priv, struct sk_buff *skb, const struct nf_hook_state *state)
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

    for (index = 0U; index < ARRAY_SIZE(g_prerouting_rules); index++)
    {
        result = g_prerouting_rules[index](skb, state, iph);

        if (result == LINKG_FAST_NAT_RULE_DONE)
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
 * @brief 处理Ethernet源流量进入LinkG网络的Source NETMAP规则。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_postrouting_ethernet_to_linkg(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    int ret;

    if (state->out == NULL || state->out->ifindex != g_fast_nat.config.tun_ifindex)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.ethernet_network))
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    ret = linkg_fast_nat_source_netmap(skb, &g_fast_nat.config.ethernet_network, &g_fast_nat.config.local_virtual_subnet);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_DONE;
}

/**
 * @brief POST_ROUTING源地址转换规则。
 *
 * Virtual源需要先查询Ethernet主动流表，命中表示当前报文属于返回方向，
 * 保留Virtual源地址；未命中时与TUN源、Hairpin源统一建立SNAT映射，
 * 最终将源地址转换为本节点Ethernet地址和Mapping槽位对应的translated ID。
 */
static linkg_fast_nat_rule_result_t _linkg_fast_nat_postrouting_source_translate(struct sk_buff *skb, const struct nf_hook_state *state, struct iphdr *iph)
{
    linkg_fast_nat_mapping_key_t mapping_key;
    linkg_fast_nat_flow_key_t    flow_key;
    unsigned int                 transport_offset;
    __be16                       source_id;
    __be16                       destination_id;
    __be16                       translated_id;
    __u32                        mapping_slot;
    __u32                        flow_slot;
    __u8                         snat_type;
    int                          ret;

    if (state->out == NULL || state->out->ifindex != g_fast_nat.config.ethernet_ifindex)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (!linkg_fast_nat_ipv4_in_subnet(iph->daddr, &g_fast_nat.config.ethernet_network))
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    transport_offset = (unsigned int)iph->ihl * 4U;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &source_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &destination_id);
    if (ret == -EOPNOTSUPP)
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    if ((skb->mark & LINKG_FAST_NAT_MARK_HAIRPIN) != 0U)
    {
        snat_type = LINKG_FAST_NAT_SNAT_HAIRPIN;
    }
    else if (linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.virtual_network))
    {
        memset(&flow_key, 0, sizeof(flow_key));

        flow_key.ethernet_ip = iph->daddr;
        flow_key.virtual_ip  = iph->saddr;
        flow_key.ethernet_id = destination_id;
        flow_key.virtual_id  = source_id;
        flow_key.protocol    = iph->protocol;

        ret = _linkg_fast_nat_flow_lookup(&flow_key, &flow_slot);
        if (ret == 0)
        {
            return LINKG_FAST_NAT_RULE_DONE;
        }

        if (ret != -ENOENT)
        {
            return LINKG_FAST_NAT_RULE_DROP;
        }

        snat_type = LINKG_FAST_NAT_SNAT_VIRTUAL;
    }
    else if (linkg_fast_nat_ipv4_in_subnet(iph->saddr, &g_fast_nat.config.tun_network))
    {
        snat_type = LINKG_FAST_NAT_SNAT_TUN;
    }
    else
    {
        return LINKG_FAST_NAT_RULE_CONTINUE;
    }

    memset(&mapping_key, 0, sizeof(mapping_key));

    mapping_key.original_ip = iph->saddr;
    mapping_key.remote_ip   = iph->daddr;
    mapping_key.original_id = source_id;
    mapping_key.remote_id   = destination_id;
    mapping_key.protocol    = iph->protocol;

    ret = _linkg_fast_nat_mapping_get_or_create(&mapping_key, snat_type, &mapping_slot);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    translated_id = htons((__u16)((__u32)g_fast_nat.config.snat_port_start + mapping_slot));

    ret = linkg_fast_nat_replace_tuple(skb, true, g_fast_nat.config.ethernet_ip, translated_id);
    if (ret != 0)
    {
        return LINKG_FAST_NAT_RULE_DROP;
    }

    return LINKG_FAST_NAT_RULE_DONE;
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

        if (result == LINKG_FAST_NAT_RULE_DONE)
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

    mod_timer(&g_fast_nat.flow_gc_timer, jiffies + LINKG_FAST_NAT_FLOW_GC_INTERVAL);
    mod_timer(&g_fast_nat.mapping_gc_timer, jiffies + LINKG_FAST_NAT_MAPPING_GC_INTERVAL);

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

    del_timer_sync(&g_fast_nat.flow_gc_timer);
    del_timer_sync(&g_fast_nat.mapping_gc_timer);

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

    timer_setup(&g_fast_nat.flow_gc_timer, _linkg_fast_nat_flow_gc_timer, 0);
    timer_setup(&g_fast_nat.mapping_gc_timer, _linkg_fast_nat_mapping_gc_timer, 0);

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
