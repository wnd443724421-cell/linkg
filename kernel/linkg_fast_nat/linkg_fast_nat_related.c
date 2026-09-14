/**
 * @file linkg_fast_nat_related.c
 * @brief LinkG Fast NAT派生数据流管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-14
 */

#include "linkg_fast_nat_internal.h"

#include <linux/errno.h>
#include <linux/in.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>

/****************************** 模块常量 ******************************/

#define LINKG_FAST_NAT_RELATED_SLOT_COUNT       256U                                         // Related Hash槽位数量，必须为2的幂
#define LINKG_FAST_NAT_RELATED_SLOT_MASK        (LINKG_FAST_NAT_RELATED_SLOT_COUNT - 1U)      // Related Hash槽位掩码
#define LINKG_FAST_NAT_RELATED_ACTIVE_MAX       128U                                         // 最大活动Related数量
#define LINKG_FAST_NAT_RELATED_HASH_SEED        0x4C524C44U                                   // "LRLD"固定Hash种子
#define LINKG_FAST_NAT_RELATED_REFRESH_TIME     (1U * HZ)                                     // 最近命中时间最小刷新周期
#define LINKG_FAST_NAT_RELATED_TIMEOUT          (120U * HZ)                                   // Related无流量超时时间
#define LINKG_FAST_NAT_RELATED_GC_INTERVAL      (5U * HZ)                                     // Related超时扫描周期
#define LINKG_FAST_NAT_RELATED_FREE             0U                                            // Related槽位从未使用
#define LINKG_FAST_NAT_RELATED_ACTIVE           1U                                            // Related槽位当前有效
#define LINKG_FAST_NAT_RELATED_TOMBSTONE        2U                                            // Related槽位已删除但保持Hash探测链
#define LINKG_FAST_NAT_RELATED_SLOT_INVALID     0xFFFFFFFFU                                   // 无效Related Hash槽位

/****************************** 内部类型 ******************************/

typedef struct
{
    __be32 local_real_ip; // 本地Ethernet业务设备真实IPv4
    __be16 local_real_id; // 本地业务设备源TCP/UDP端口
    __be16 gateway_id;    // 业务设备发送到本节点Ethernet地址的目的端口
    __u8   protocol;      // IPv4上层协议
} linkg_fast_nat_related_key_t;

typedef struct
{
    linkg_fast_nat_related_key_t key;               // 本地真实数据流匹配Key
    __be32                        remote_virtual_ip; // 远端客户端Virtual IPv4
    __be16                        remote_virtual_id; // 远端客户端目标TCP/UDP端口
    unsigned long                 last_seen;         // 最近一次活动时间
    __u8                          state;             // LINKG_FAST_NAT_RELATED_*
} linkg_fast_nat_related_entry_t;

typedef struct
{
    spinlock_t                     lock;                                        // Related表写锁
    struct timer_list              gc_timer;                                    // Related超时扫描定时器
    linkg_fast_nat_related_entry_t entries[LINKG_FAST_NAT_RELATED_SLOT_COUNT];   // Related Hash表
    __u32                          active_count;                                // 当前活动Related数量
    bool                           running;                                     // Related数据面是否运行
} linkg_fast_nat_related_context_t;

/****************************** 全局上下文 ******************************/

static linkg_fast_nat_related_context_t g_related =
{
    .lock         = __SPIN_LOCK_UNLOCKED(g_related.lock), // Related表写锁
    .active_count = 0U,                                   // 当前无活动Related
    .running      = false                                 // 数据面尚未运行
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 清空全部Related状态。
 *
 * 调用方必须确保Related GC已经停止且Fast NAT数据面不再访问该表。
 */
static void _linkg_fast_nat_related_reset(void)
{
    memset(g_related.entries, 0, sizeof(g_related.entries));
    g_related.active_count = 0U;
}

/**
 * @brief 判断两个Related Key是否一致。
 */
static bool _linkg_fast_nat_related_key_equal(const linkg_fast_nat_related_key_t *left, const linkg_fast_nat_related_key_t *right)
{
    return left->protocol      == right->protocol &&
           left->local_real_ip == right->local_real_ip &&
           left->local_real_id == right->local_real_id &&
           left->gateway_id    == right->gateway_id;
}

/**
 * @brief 计算Related Key对应的初始Hash槽位。
 */
static __u32 _linkg_fast_nat_related_hash(const linkg_fast_nat_related_key_t *key)
{
    __u32 ids;
    __u32 hash;

    ids = ((__u32)ntohs(key->local_real_id) << 16U) | (__u32)ntohs(key->gateway_id);
    hash = jhash_3words((__force __u32)key->local_real_ip,
                        ids,
                        (__u32)key->protocol,
                        LINKG_FAST_NAT_RELATED_HASH_SEED);

    return hash & LINKG_FAST_NAT_RELATED_SLOT_MASK;
}

/**
 * @brief 无锁查询Related Hash表。
 *
 * entry只在state发布为ACTIVE前写入完整内容，读取侧通过acquire语义读取state。
 */
static int _linkg_fast_nat_related_lookup(const linkg_fast_nat_related_key_t *key, __be32 *remote_virtual_ip, __be16 *remote_virtual_id, __u32 *slot)
{
    linkg_fast_nat_related_entry_t *entry;
    __u32                           current_slot;
    __u32                           probe;
    __u8                            state;

    if (key == NULL || remote_virtual_ip == NULL || remote_virtual_id == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    current_slot = _linkg_fast_nat_related_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_RELATED_SLOT_COUNT; probe++)
    {
        entry = &g_related.entries[current_slot];
        state = smp_load_acquire(&entry->state);

        if (state == LINKG_FAST_NAT_RELATED_FREE)
        {
            return -ENOENT;
        }

        if (state == LINKG_FAST_NAT_RELATED_ACTIVE && _linkg_fast_nat_related_key_equal(&entry->key, key))
        {
            *remote_virtual_ip = READ_ONCE(entry->remote_virtual_ip);
            *remote_virtual_id = READ_ONCE(entry->remote_virtual_id);
            *slot              = current_slot;
            return 0;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_RELATED_SLOT_MASK;
    }

    return -ENOENT;
}

/**
 * @brief 按最小刷新周期更新Related最近活动时间。
 */
static void _linkg_fast_nat_related_refresh(const linkg_fast_nat_related_key_t *key, __u32 slot)
{
    linkg_fast_nat_related_entry_t *entry;
    unsigned long                   flags;
    unsigned long                   last_seen;
    unsigned long                   now;

    if (key == NULL || slot >= LINKG_FAST_NAT_RELATED_SLOT_COUNT)
    {
        return;
    }

    entry     = &g_related.entries[slot];
    now       = jiffies;
    last_seen = READ_ONCE(entry->last_seen);

    if (!time_after_eq(now, last_seen + LINKG_FAST_NAT_RELATED_REFRESH_TIME))
    {
        return;
    }

    spin_lock_irqsave(&g_related.lock, flags);

    if (entry->state == LINKG_FAST_NAT_RELATED_ACTIVE && _linkg_fast_nat_related_key_equal(&entry->key, key))
    {
        entry->last_seen = now;
    }

    spin_unlock_irqrestore(&g_related.lock, flags);
}

/**
 * @brief 周期回收超时Related状态。
 */
static void _linkg_fast_nat_related_gc_timer(struct timer_list *timer)
{
    linkg_fast_nat_related_entry_t *entry;
    unsigned long                   flags;
    unsigned long                   now;
    __u32                           index;

    (void)timer;

    if (!READ_ONCE(g_related.running))
    {
        return;
    }

    now = jiffies;

    spin_lock_irqsave(&g_related.lock, flags);

    for (index = 0U; index < LINKG_FAST_NAT_RELATED_SLOT_COUNT; index++)
    {
        entry = &g_related.entries[index];

        if (entry->state != LINKG_FAST_NAT_RELATED_ACTIVE)
        {
            continue;
        }

        if (!time_after_eq(now, READ_ONCE(entry->last_seen) + LINKG_FAST_NAT_RELATED_TIMEOUT))
        {
            continue;
        }

        smp_store_release(&entry->state, LINKG_FAST_NAT_RELATED_TOMBSTONE);

        if (g_related.active_count > 0U)
        {
            g_related.active_count--;
        }
    }

    spin_unlock_irqrestore(&g_related.lock, flags);

    if (READ_ONCE(g_related.running))
    {
        mod_timer(&g_related.gc_timer, jiffies + LINKG_FAST_NAT_RELATED_GC_INTERVAL);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Related状态表和GC定时器。
 */
void linkg_fast_nat_related_init(void)
{
    timer_setup(&g_related.gc_timer, _linkg_fast_nat_related_gc_timer, 0);
    _linkg_fast_nat_related_reset();
}

/**
 * @brief 启动Related数据面。
 */
void linkg_fast_nat_related_start(void)
{
    _linkg_fast_nat_related_reset();
    WRITE_ONCE(g_related.running, true);
    mod_timer(&g_related.gc_timer, jiffies + LINKG_FAST_NAT_RELATED_GC_INTERVAL);
}

/**
 * @brief 停止Related数据面并清空全部派生流状态。
 */
void linkg_fast_nat_related_stop(void)
{
    WRITE_ONCE(g_related.running, false);
    del_timer_sync(&g_related.gc_timer);
    _linkg_fast_nat_related_reset();
}

/**
 * @brief 反初始化Related状态表。
 */
void linkg_fast_nat_related_deinit(void)
{
    linkg_fast_nat_related_stop();
}

/****************************** Related接口 ******************************/

/**
 * @brief 创建或更新一条协议派生的数据流映射。
 *
 * local_real_ip/local_real_id/gateway_id描述本节点Ethernet侧未来会收到的数据流，
 * remote_virtual_ip/remote_virtual_id描述命中后需要恢复的远端Virtual目标。
 */
int linkg_fast_nat_related_add(__be32 local_real_ip, __be16 local_real_id, __be16 gateway_id, __be32 remote_virtual_ip, __be16 remote_virtual_id, __u8 protocol)
{
    linkg_fast_nat_related_key_t key;
    linkg_fast_nat_related_entry_t *entry;
    unsigned long flags;
    __u32 current_slot;
    __u32 insert_slot;
    __u32 existing_slot;
    __u32 probe;
    __u8 state;
    __be32 existing_virtual_ip;
    __be16 existing_virtual_id;
    int ret;

    if (local_real_ip == 0 || remote_virtual_ip == 0 || local_real_id == 0 || gateway_id == 0 || remote_virtual_id == 0)
    {
        return -EINVAL;
    }

    if (protocol != IPPROTO_TCP && protocol != IPPROTO_UDP)
    {
        return -EPROTONOSUPPORT;
    }

    memset(&key, 0, sizeof(key));
    key.local_real_ip = local_real_ip;
    key.local_real_id = local_real_id;
    key.gateway_id    = gateway_id;
    key.protocol      = protocol;

    ret = _linkg_fast_nat_related_lookup(&key, &existing_virtual_ip, &existing_virtual_id, &existing_slot);
    if (ret == 0)
    {
        spin_lock_irqsave(&g_related.lock, flags);

        entry = &g_related.entries[existing_slot];
        if (entry->state == LINKG_FAST_NAT_RELATED_ACTIVE && _linkg_fast_nat_related_key_equal(&entry->key, &key))
        {
            WRITE_ONCE(entry->remote_virtual_ip, remote_virtual_ip);
            WRITE_ONCE(entry->remote_virtual_id, remote_virtual_id);
            entry->last_seen = jiffies;
        }

        spin_unlock_irqrestore(&g_related.lock, flags);
        return 0;
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    spin_lock_irqsave(&g_related.lock, flags);

    ret = _linkg_fast_nat_related_lookup(&key, &existing_virtual_ip, &existing_virtual_id, &existing_slot);
    if (ret == 0)
    {
        entry = &g_related.entries[existing_slot];
        WRITE_ONCE(entry->remote_virtual_ip, remote_virtual_ip);
        WRITE_ONCE(entry->remote_virtual_id, remote_virtual_id);
        entry->last_seen = jiffies;

        spin_unlock_irqrestore(&g_related.lock, flags);
        return 0;
    }

    if (ret != -ENOENT)
    {
        spin_unlock_irqrestore(&g_related.lock, flags);
        return ret;
    }

    if (g_related.active_count >= LINKG_FAST_NAT_RELATED_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_related.lock, flags);
        return -ENOSPC;
    }

    insert_slot  = LINKG_FAST_NAT_RELATED_SLOT_INVALID;
    current_slot = _linkg_fast_nat_related_hash(&key);

    for (probe = 0U; probe < LINKG_FAST_NAT_RELATED_SLOT_COUNT; probe++)
    {
        entry = &g_related.entries[current_slot];
        state = entry->state;

        if (state == LINKG_FAST_NAT_RELATED_TOMBSTONE || state == LINKG_FAST_NAT_RELATED_FREE)
        {
            insert_slot = current_slot;
            break;
        }

        current_slot = (current_slot + 1U) & LINKG_FAST_NAT_RELATED_SLOT_MASK;
    }

    if (insert_slot == LINKG_FAST_NAT_RELATED_SLOT_INVALID)
    {
        spin_unlock_irqrestore(&g_related.lock, flags);
        return -ENOSPC;
    }

    entry = &g_related.entries[insert_slot];

    entry->key               = key;
    entry->remote_virtual_ip = remote_virtual_ip;
    entry->remote_virtual_id = remote_virtual_id;
    entry->last_seen         = jiffies;

    smp_store_release(&entry->state, LINKG_FAST_NAT_RELATED_ACTIVE);

    g_related.active_count++;

    spin_unlock_irqrestore(&g_related.lock, flags);

    return 0;
}

/**
 * @brief 匹配Ethernet入口的派生数据流并恢复远端Virtual目标。
 *
 * 命中后只执行Destination转换，Source保持真实Ethernet地址，
 * 后续TUN TX Source NETMAP会将其恢复为本节点对应的Virtual地址。
 */
int linkg_fast_nat_related_translate_ethernet_rx(struct sk_buff *skb, __be32 gateway_ip)
{
    linkg_fast_nat_related_key_t key;
    struct iphdr *iph;
    unsigned int transport_offset;
    __be32 remote_virtual_ip;
    __be16 remote_virtual_id;
    __u32 slot;
    int ret;

    if (skb == NULL || gateway_ip == 0)
    {
        return -EINVAL;
    }

    ret = linkg_fast_nat_get_ipv4(skb, &iph, &transport_offset);
    if (ret != 0)
    {
        return ret;
    }

    if ((iph->protocol != IPPROTO_TCP && iph->protocol != IPPROTO_UDP) || iph->daddr != gateway_ip)
    {
        return -ENOENT;
    }

    memset(&key, 0, sizeof(key));
    key.local_real_ip = iph->saddr;
    key.protocol      = iph->protocol;

    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, true, &key.local_real_id);
    if (ret != 0)
    {
        return ret;
    }

    iph = ip_hdr(skb);
    ret = linkg_fast_nat_get_transport_id(skb, iph, transport_offset, false, &key.gateway_id);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_related_lookup(&key, &remote_virtual_ip, &remote_virtual_id, &slot);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_fast_nat_related_refresh(&key, slot);

    return linkg_fast_nat_replace_tuple(skb, false, remote_virtual_ip, remote_virtual_id);
}
