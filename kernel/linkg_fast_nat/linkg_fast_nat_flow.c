/**
 * @file linkg_fast_nat_flow.c
 * @brief Ethernet主动流表
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"

#include <linux/errno.h>
#include <linux/in.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/seqlock.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>

/****************************** 私有常量 ******************************/

#define LINKG_FAST_NAT_FLOW_SLOT_COUNT        4096U                                 // Ethernet主动流Hash槽位数量，必须为2的幂
#define LINKG_FAST_NAT_FLOW_SLOT_MASK         (LINKG_FAST_NAT_FLOW_SLOT_COUNT - 1U) // Ethernet主动流Hash槽位掩码
#define LINKG_FAST_NAT_FLOW_ACTIVE_MAX        2000U                                 // 最大活动Ethernet主动流数量
#define LINKG_FAST_NAT_FLOW_HASH_SEED         0x4C464C57U                           // "LFLW"固定Hash种子
#define LINKG_FAST_NAT_FLOW_FAST_COUNT        4U                                    // Ethernet主动流快速缓存槽位数量，必须为2的幂
#define LINKG_FAST_NAT_FLOW_FAST_MASK         (LINKG_FAST_NAT_FLOW_FAST_COUNT - 1U) // Ethernet主动流快速缓存槽位掩码
#define LINKG_FAST_NAT_FLOW_SLOT_INVALID      0xFFFFFFFFU                           // 无效Flow Hash槽位
#define LINKG_FAST_NAT_FLOW_REFRESH_TIME      (1U  * HZ)                            // 活动Flow最近命中时间最小刷新周期
#define LINKG_FAST_NAT_FLOW_TIMEOUT           (180U * HZ)                           // Flow无流量超时时间
#define LINKG_FAST_NAT_FLOW_GC_INTERVAL       (5U  * HZ)                            // Flow超时扫描周期

#define LINKG_FAST_NAT_FLOW_FREE            0U // Flow槽位从未使用
#define LINKG_FAST_NAT_FLOW_ACTIVE          1U // Flow槽位当前有效
#define LINKG_FAST_NAT_FLOW_TOMBSTONE       2U // Flow已超时，可被新Flow复用

/****************************** 私有类型 ******************************/

/**
 * @brief Ethernet主动流方向项，正向和返回报文使用相同的Key。
 */
typedef struct
{
    linkg_fast_nat_flow_key_t key;       // 标准Ethernet -> Virtual流Key
    unsigned long             last_seen; // 最近一次活动时间
    seqcount_t                seq;       // Flow槽位版本，创建和回收由写锁保护
    __u8                      state;     // LINKG_FAST_NAT_FLOW_*
} linkg_fast_nat_flow_entry_t;

/**
 * @brief Flow表的私有状态。
 */
typedef struct
{
    spinlock_t        lock;     // 状态表写锁
    struct timer_list gc_timer; // 状态表老化定时器
    bool              running;  // GC运行标志

    linkg_fast_nat_flow_entry_t flows[LINKG_FAST_NAT_FLOW_SLOT_COUNT];           // Ethernet主动Virtual流Hash表
    __u32                       fast_flow_slots[LINKG_FAST_NAT_FLOW_FAST_COUNT]; // 高频Flow对应的Hash槽位
    __u32                       fast_flow_last_hit;                              // 最近一次命中的Flow快速缓存槽位
    __u32                       fast_flow_next_replace;                          // 下一次Flow快速缓存替换槽位
    __u32                       flow_count;                                      // 当前活动Ethernet主动流数量
} linkg_fast_nat_flow_context_t;

static linkg_fast_nat_flow_context_t g_flow =
{
    .lock = __SPIN_LOCK_UNLOCKED(g_flow.lock)
};

/****************************** Ethernet主动流表 ******************************/

/**
 * @brief 清空全部Ethernet主动Virtual流状态。
 *
 * 仅在Flow老化定时器和Netfilter hooks停止后调用。
 */
void linkg_fast_nat_flow_reset(void)
{
    __u32 index;

    memset(g_flow.flows, 0, sizeof(g_flow.flows));

    for (index = 0U; index < LINKG_FAST_NAT_FLOW_SLOT_COUNT; index++)
    {
        seqcount_init(&g_flow.flows[index].seq);
    }
    memset(g_flow.fast_flow_slots, 0xFF, sizeof(g_flow.fast_flow_slots));

    g_flow.fast_flow_last_hit     = 0U;
    g_flow.fast_flow_next_replace = 0U;
    g_flow.flow_count             = 0U;
}

/**
 * @brief 判断两个Ethernet主动Virtual流Key是否一致。
 */
static bool _linkg_fast_nat_flow_key_equal(const linkg_fast_nat_flow_key_t *left, const linkg_fast_nat_flow_key_t *right)
{
    return READ_ONCE(left->protocol)    == right->protocol &&
           READ_ONCE(left->ethernet_ip) == right->ethernet_ip &&
           READ_ONCE(left->ethernet_id) == right->ethernet_id &&
           READ_ONCE(left->virtual_ip)  == right->virtual_ip &&
           READ_ONCE(left->virtual_id)  == right->virtual_id;
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
static int _linkg_fast_nat_flow_refresh(const linkg_fast_nat_flow_key_t *key, __u32 slot, __u32 seq)
{
    linkg_fast_nat_flow_entry_t *entry;
    unsigned long                flags;
    unsigned long                last_seen;
    unsigned long                now;

    if (key == NULL || slot >= LINKG_FAST_NAT_FLOW_SLOT_COUNT)
    {
        return -EINVAL;
    }

    entry     = &g_flow.flows[slot];
    now       = jiffies;
    last_seen = READ_ONCE(entry->last_seen);

    if (!time_after_eq(now, last_seen + LINKG_FAST_NAT_FLOW_REFRESH_TIME))
    {
        // 校验Lookup读取时的槽位版本，避免GC回收后复用导致错误命中。
        if (read_seqcount_retry(&entry->seq, seq))
        {
            return -ENOENT;
        }

        return 0;
    }

    spin_lock_irqsave(&g_flow.lock, flags);

    if (raw_read_seqcount(&entry->seq) != seq ||
        entry->state != LINKG_FAST_NAT_FLOW_ACTIVE ||
        !_linkg_fast_nat_flow_key_equal(&entry->key, key))
    {
        spin_unlock_irqrestore(&g_flow.lock, flags);
        return -ENOENT;
    }

    entry->last_seen = now;

    spin_unlock_irqrestore(&g_flow.lock, flags);

    return 0;
}

/**
 * @brief 从快速缓存查找Ethernet主动Virtual流。
 */
static int _linkg_fast_nat_flow_fast_lookup(const linkg_fast_nat_flow_key_t *key, __u32 *slot, __u32 *seq)
{
    linkg_fast_nat_flow_entry_t *entry;
    __u32                        cached_slot;
    __u32                        start;
    __u32                        offset;
    __u32                        index;
    __u32                        version;
    bool                         matched;

    if (key == NULL || slot == NULL || seq == NULL)
    {
        return -EINVAL;
    }

    start = READ_ONCE(g_flow.fast_flow_last_hit);

    for (offset = 0U; offset < LINKG_FAST_NAT_FLOW_FAST_COUNT; offset++)
    {
        index       = (start + offset) & LINKG_FAST_NAT_FLOW_FAST_MASK;
        cached_slot = READ_ONCE(g_flow.fast_flow_slots[index]);

        if (cached_slot == LINKG_FAST_NAT_FLOW_SLOT_INVALID)
        {
            continue;
        }

        entry = &g_flow.flows[cached_slot];

        do
        {
            version = read_seqcount_begin(&entry->seq);

            matched = READ_ONCE(entry->state) == LINKG_FAST_NAT_FLOW_ACTIVE &&
                      _linkg_fast_nat_flow_key_equal(&entry->key, key);
        } while (read_seqcount_retry(&entry->seq, version));

        if (!matched)
        {
            continue;
        }

        if (index != start)
        {
            WRITE_ONCE(g_flow.fast_flow_last_hit, index);
        }

        *slot = cached_slot;
        *seq  = version;
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

    index = READ_ONCE(g_flow.fast_flow_next_replace);

    WRITE_ONCE(g_flow.fast_flow_slots[index], slot);
    WRITE_ONCE(g_flow.fast_flow_last_hit, index);
    WRITE_ONCE(g_flow.fast_flow_next_replace, (index + 1U) & LINKG_FAST_NAT_FLOW_FAST_MASK);
}

/**
 * @brief 无锁查找Ethernet主动Virtual流Hash主表。
 */
static int _linkg_fast_nat_flow_hash_lookup(const linkg_fast_nat_flow_key_t *key, __u32 *slot, __u32 *seq)
{
    linkg_fast_nat_flow_entry_t *entry;
    __u32                        current_slot;
    __u32                        probe;
    __u32                        version;
    __u8                         state;
    bool                         matched;

    if (key == NULL || slot == NULL || seq == NULL)
    {
        return -EINVAL;
    }

    current_slot = _linkg_fast_nat_flow_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_FLOW_SLOT_COUNT; probe++)
    {
        entry = &g_flow.flows[current_slot];

        do
        {
            version = read_seqcount_begin(&entry->seq);
            state   = READ_ONCE(entry->state);
            matched = state == LINKG_FAST_NAT_FLOW_ACTIVE &&
                      _linkg_fast_nat_flow_key_equal(&entry->key, key);
        } while (read_seqcount_retry(&entry->seq, version));

        if (state == LINKG_FAST_NAT_FLOW_FREE)
        {
            return -ENOENT;
        }

        if (matched)
        {
            *slot = current_slot;
            *seq  = version;
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
    __u32 seq;
    int   ret;

    ret = _linkg_fast_nat_flow_fast_lookup(key, slot, &seq);
    if (ret == 0)
    {
        ret = _linkg_fast_nat_flow_refresh(key, *slot, seq);
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

    ret = _linkg_fast_nat_flow_hash_lookup(key, slot, &seq);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_fast_nat_flow_refresh(key, *slot, seq);
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
int linkg_fast_nat_flow_get_or_create(const linkg_fast_nat_flow_key_t *key)
{
    linkg_fast_nat_flow_entry_t *entry;
    unsigned long                flags;
    __u32                        insert_slot;
    __u32                        current_slot;
    __u32                        probe;
    __u32                        slot;
    __u32                        found_seq;
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

    spin_lock_irqsave(&g_flow.lock, flags);

    ret = _linkg_fast_nat_flow_hash_lookup(key, &slot, &found_seq);
    if (ret == 0)
    {
        g_flow.flows[slot].last_seen = jiffies;

        spin_unlock_irqrestore(&g_flow.lock, flags);

        _linkg_fast_nat_flow_fast_update(slot);

        return 0;
    }

    if (ret != -ENOENT)
    {
        spin_unlock_irqrestore(&g_flow.lock, flags);
        return ret;
    }

    if (g_flow.flow_count >= LINKG_FAST_NAT_FLOW_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_flow.lock, flags);
        return -ENOSPC;
    }

    insert_slot  = LINKG_FAST_NAT_FLOW_SLOT_INVALID;
    current_slot = _linkg_fast_nat_flow_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_FLOW_SLOT_COUNT; probe++)
    {
        entry = &g_flow.flows[current_slot];
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
        spin_unlock_irqrestore(&g_flow.lock, flags);
        return -ENOSPC;
    }

    entry = &g_flow.flows[insert_slot];

    write_seqcount_begin(&entry->seq);

    entry->key       = *key;
    entry->last_seen = jiffies;

    smp_store_release(&entry->state, LINKG_FAST_NAT_FLOW_ACTIVE);

    write_seqcount_end(&entry->seq);

    g_flow.flow_count++;

    spin_unlock_irqrestore(&g_flow.lock, flags);

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

    if (!READ_ONCE(g_flow.running))
    {
        return;
    }

    if (READ_ONCE(g_flow.flow_count) == 0U)
    {
        goto rearm;
    }

    now = jiffies;

    spin_lock_irqsave(&g_flow.lock, flags);

    for (index = 0U; index < LINKG_FAST_NAT_FLOW_SLOT_COUNT; index++)
    {
        entry = &g_flow.flows[index];

        if (entry->state != LINKG_FAST_NAT_FLOW_ACTIVE)
        {
            continue;
        }

        if (!time_after_eq(now, READ_ONCE(entry->last_seen) + LINKG_FAST_NAT_FLOW_TIMEOUT))
        {
            continue;
        }

        write_seqcount_begin(&entry->seq);
        smp_store_release(&entry->state, LINKG_FAST_NAT_FLOW_TOMBSTONE);
        write_seqcount_end(&entry->seq);

        if (g_flow.flow_count > 0U)
        {
            g_flow.flow_count--;
        }
    }

    spin_unlock_irqrestore(&g_flow.lock, flags);

rearm:
    if (READ_ONCE(g_flow.running))
    {
        mod_timer(&g_flow.gc_timer, jiffies + LINKG_FAST_NAT_FLOW_GC_INTERVAL);
    }
}

/**
 * @brief 查找并刷新Ethernet主动流。
 */
int linkg_fast_nat_flow_lookup(const linkg_fast_nat_flow_key_t *key)
{
    __u32 slot;

    return _linkg_fast_nat_flow_lookup(key, &slot);
}

/**
 * @brief 初始化状态表的老化定时器。
 */
void linkg_fast_nat_flow_init(void)
{
    timer_setup(&g_flow.gc_timer, _linkg_fast_nat_flow_gc_timer, 0);
}

/**
 * @brief 启动状态表老化回收。
 */
void linkg_fast_nat_flow_gc_start(void)
{
    WRITE_ONCE(g_flow.running, true);
    mod_timer(&g_flow.gc_timer, jiffies + LINKG_FAST_NAT_FLOW_GC_INTERVAL);
}

/**
 * @brief 禁止GC重新安排定时器并等待在途回调退出。
 */
void linkg_fast_nat_flow_gc_stop(void)
{
    WRITE_ONCE(g_flow.running, false);
    del_timer_sync(&g_flow.gc_timer);
}
