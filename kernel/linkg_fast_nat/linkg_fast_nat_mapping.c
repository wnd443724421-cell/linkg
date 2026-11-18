/**
 * @file linkg_fast_nat_mapping.c
 * @brief LinkG Fast NAT SNAT映射管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"

#include <linux/errno.h>
#include <linux/in.h>
#include <linux/jhash.h>
#include <linux/jiffies.h>
#include <linux/kernel.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/timer.h>
#include <linux/types.h>

/****************************** 模块常量 ******************************/

#define LINKG_FAST_NAT_SLOT_MASK             (LINKG_FAST_NAT_SLOT_COUNT - 1U)         // SNAT Hash槽位掩码
#define LINKG_FAST_NAT_ACTIVE_MAX            2000U                                    // 最大活动SNAT映射数量
#define LINKG_FAST_NAT_HASH_SEED             0x4C4E4154U                              // SNAT固定Hash种子
#define LINKG_FAST_NAT_MAPPING_FAST_COUNT    4U                                       // SNAT映射快速缓存槽位数量
#define LINKG_FAST_NAT_MAPPING_FAST_MASK     (LINKG_FAST_NAT_MAPPING_FAST_COUNT - 1U) // SNAT映射快速缓存槽位掩码
#define LINKG_FAST_NAT_MAPPING_SLOT_INVALID  0xFFFFFFFFU                              // 无效SNAT映射Hash槽位
#define LINKG_FAST_NAT_MAPPING_REFRESH_TIME  (1U * HZ)                                // Mapping最近命中时间最小刷新周期
#define LINKG_FAST_NAT_MAPPING_TIMEOUT       (180U * HZ)                              // Mapping无流量超时时间
#define LINKG_FAST_NAT_MAPPING_GC_INTERVAL   (6U * HZ)                                // Mapping超时扫描周期
#define LINKG_FAST_NAT_ENTRY_FREE            0U                                       // SNAT映射槽从未使用
#define LINKG_FAST_NAT_ENTRY_ACTIVE          1U                                       // SNAT映射槽当前有效
#define LINKG_FAST_NAT_ENTRY_TOMBSTONE       2U                                       // SNAT映射槽已删除但保留Hash探测链

/****************************** 内部类型 ******************************/

/**
 * @brief Fast NAT SNAT映射项。
 *
 * translated ID固定为配置的SNAT起始端口加当前Hash槽位下标。
 * 槽位进入TOMBSTONE后仍保留Hash探测链。
 */
typedef struct
{
    linkg_fast_nat_mapping_key_t key;       // SNAT前原始五元组
    unsigned long                last_seen; // 最近一次活动时间
    __u8                         snat_type; // SNAT来源类型
    __u8                         state;     // LINKG_FAST_NAT_ENTRY_*
} linkg_fast_nat_entry_t;

/**
 * @brief Fast NAT SNAT映射模块上下文。
 */
typedef struct
{
    spinlock_t             lock;                                               // Mapping表写锁
    struct timer_list      gc_timer;                                           // Mapping超时回收定时器
    bool                   running;                                            // 是否允许GC继续运行
    linkg_fast_nat_entry_t entries[LINKG_FAST_NAT_SLOT_COUNT];                 // SNAT映射Hash表，槽位对应translated ID
    __u32                  fast_slots[LINKG_FAST_NAT_MAPPING_FAST_COUNT];      // 高频Mapping对应Hash槽位
    __u32                  fast_last_hit;                                      // 最近一次命中的快速缓存槽位
    __u32                  fast_next_replace;                                  // 下一次快速缓存替换槽位
    __u32                  active_count;                                       // 当前活动SNAT映射数量
} linkg_fast_nat_mapping_context_t;

/****************************** 全局上下文 ******************************/

static linkg_fast_nat_mapping_context_t g_mapping =
{
    .lock = __SPIN_LOCK_UNLOCKED(g_mapping.lock) // Mapping表写锁
};

/****************************** SNAT映射 ******************************/

/**
 * @brief 清空全部SNAT映射状态。
 *
 * 调用方必须确保Netfilter hooks和Mapping老化定时器已停止。
 */
void linkg_fast_nat_mapping_reset(void)
{
    memset(g_mapping.entries, 0, sizeof(g_mapping.entries));
    memset(g_mapping.fast_slots, 0xFF, sizeof(g_mapping.fast_slots));

    g_mapping.fast_last_hit     = 0U;
    g_mapping.fast_next_replace = 0U;
    g_mapping.active_count      = 0U;
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

    entry     = &g_mapping.entries[slot];
    now       = jiffies;
    last_seen = READ_ONCE(entry->last_seen);

    if (!time_after_eq(now, last_seen + LINKG_FAST_NAT_MAPPING_REFRESH_TIME))
    {
        return 0;
    }

    spin_lock_irqsave(&g_mapping.lock, flags);

    if (entry->state != LINKG_FAST_NAT_ENTRY_ACTIVE ||
        entry->snat_type != snat_type ||
        !_linkg_fast_nat_mapping_key_equal(&entry->key, key))
    {
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return -ENOENT;
    }

    entry->last_seen = now;

    spin_unlock_irqrestore(&g_mapping.lock, flags);

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

    start = READ_ONCE(g_mapping.fast_last_hit);

    for (offset = 0U; offset < LINKG_FAST_NAT_MAPPING_FAST_COUNT; offset++)
    {
        index       = (start + offset) & LINKG_FAST_NAT_MAPPING_FAST_MASK;
        cached_slot = READ_ONCE(g_mapping.fast_slots[index]);

        if (cached_slot == LINKG_FAST_NAT_MAPPING_SLOT_INVALID || cached_slot >= LINKG_FAST_NAT_SLOT_COUNT)
        {
            continue;
        }

        entry = &g_mapping.entries[cached_slot];

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
            WRITE_ONCE(g_mapping.fast_last_hit, index);
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

    index = READ_ONCE(g_mapping.fast_next_replace);

    WRITE_ONCE(g_mapping.fast_slots[index], slot);
    WRITE_ONCE(g_mapping.fast_last_hit, index);
    WRITE_ONCE(g_mapping.fast_next_replace, (index + 1U) & LINKG_FAST_NAT_MAPPING_FAST_MASK);
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
        entry = &g_mapping.entries[current_slot];
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
        snat_type != LINKG_FAST_NAT_SNAT_HAIRPIN &&
        snat_type != LINKG_FAST_NAT_SNAT_LOCAL &&
        snat_type != LINKG_FAST_NAT_SNAT_UPLINK)
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

    spin_lock_irqsave(&g_mapping.lock, flags);

    ret = _linkg_fast_nat_mapping_hash_lookup(key, snat_type, &found_slot);
    if (ret == 0)
    {
        g_mapping.entries[found_slot].last_seen = jiffies;

        spin_unlock_irqrestore(&g_mapping.lock, flags);

        _linkg_fast_nat_mapping_fast_update(found_slot);

        *slot = found_slot;
        return 0;
    }

    if (ret != -ENOENT)
    {
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return ret;
    }

    if (g_mapping.active_count >= LINKG_FAST_NAT_ACTIVE_MAX)
    {
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return -ENOSPC;
    }

    insert_slot  = LINKG_FAST_NAT_MAPPING_SLOT_INVALID;
    current_slot = _linkg_fast_nat_mapping_hash(key);

    for (probe = 0U; probe < LINKG_FAST_NAT_SLOT_COUNT; probe++)
    {
        entry = &g_mapping.entries[current_slot];
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
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return -ENOSPC;
    }

    entry = &g_mapping.entries[insert_slot];

    entry->key       = *key;
    entry->last_seen = jiffies;
    entry->snat_type = snat_type;

    smp_store_release(&entry->state, LINKG_FAST_NAT_ENTRY_ACTIVE);

    g_mapping.active_count++;

    spin_unlock_irqrestore(&g_mapping.lock, flags);

    _linkg_fast_nat_mapping_fast_update(insert_slot);

    *slot = insert_slot;
    return 0;
}

/**
 * @brief 根据translated ID对应槽位反查SNAT映射。
 *
 * 回包通过translated ID直接定位Mapping槽位，不执行Hash查询。
 * TCP/UDP额外校验远端源端口；ICMP Echo只校验协议和远端IPv4地址。
 */
static int _linkg_fast_nat_mapping_reverse_lookup(__u32 slot, __u8 protocol, __be32 remote_ip, __be16 remote_id, __be32 *original_ip, __be16 *original_id, __u8 *snat_type)
{
    linkg_fast_nat_entry_t *entry;
    unsigned long           flags;
    unsigned long           last_seen;
    unsigned long           now;
    bool                    check_remote_id;

    if (slot >= LINKG_FAST_NAT_SLOT_COUNT || original_ip == NULL || original_id == NULL || snat_type == NULL)
    {
        return -EINVAL;
    }

    entry = &g_mapping.entries[slot];

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
        *snat_type   = READ_ONCE(entry->snat_type);

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

    spin_lock_irqsave(&g_mapping.lock, flags);

    if (entry->state != LINKG_FAST_NAT_ENTRY_ACTIVE)
    {
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return -ENOENT;
    }

    if (entry->key.protocol != protocol || entry->key.remote_ip != remote_ip)
    {
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return -ENOENT;
    }

    if (check_remote_id && entry->key.remote_id != remote_id)
    {
        spin_unlock_irqrestore(&g_mapping.lock, flags);
        return -ENOENT;
    }

    *original_ip     = entry->key.original_ip;
    *original_id     = entry->key.original_id;
    *snat_type       = entry->snat_type;
    entry->last_seen = now;

    spin_unlock_irqrestore(&g_mapping.lock, flags);

    return 0;
}

/****************************** translated ID接口 ******************************/

/**
 * @brief 将网络字节序的translated ID转换为内部槽位。
 */
static __u32 _linkg_fast_nat_mapping_id_to_slot(__u16 port_start, __be16 translated_id)
{
    return (__u32)ntohs(translated_id) - (__u32)port_start;
}

/**
 * @brief 判断translated ID是否属于SNAT端口池。
 */
bool linkg_fast_nat_mapping_id_in_range(__u16 port_start, __be16 translated_id)
{
    return _linkg_fast_nat_mapping_id_to_slot(port_start, translated_id) < LINKG_FAST_NAT_SLOT_COUNT;
}

/**
 * @brief 查找或创建SNAT映射，返回网络字节序的translated ID。
 */
int linkg_fast_nat_mapping_get_or_create(const linkg_fast_nat_mapping_key_t *key, __u8 snat_type, __u16 port_start, __be16 *translated_id)
{
    __u32 slot;
    int   ret;

    if (translated_id == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_fast_nat_mapping_get_or_create(key, snat_type, &slot);
    if (ret == 0)
    {
        *translated_id = htons((__u16)((__u32)port_start + slot));
    }

    return ret;
}

/**
 * @brief 使用translated ID和远端端点恢复原始端点及SNAT来源。
 */
int linkg_fast_nat_mapping_reverse_lookup(__u16 port_start, __be16 translated_id, __u8 protocol, __be32 remote_ip, __be16 remote_id, __be32 *original_ip, __be16 *original_id, __u8 *snat_type)
{
    __u32 slot;

    slot = _linkg_fast_nat_mapping_id_to_slot(port_start, translated_id);
    if (slot >= LINKG_FAST_NAT_SLOT_COUNT)
    {
        return -ENOENT;
    }

    return _linkg_fast_nat_mapping_reverse_lookup(slot, protocol, remote_ip, remote_id, original_ip, original_id, snat_type);
}

/****************************** 超时回收 ******************************/

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

    if (!READ_ONCE(g_mapping.running))
    {
        return;
    }

    if (READ_ONCE(g_mapping.active_count) == 0U)
    {
        goto rearm;
    }

    now = jiffies;

    spin_lock_irqsave(&g_mapping.lock, flags);

    for (index = 0U; index < LINKG_FAST_NAT_SLOT_COUNT; index++)
    {
        entry = &g_mapping.entries[index];

        if (entry->state != LINKG_FAST_NAT_ENTRY_ACTIVE)
        {
            continue;
        }

        if (!time_after_eq(now, READ_ONCE(entry->last_seen) + LINKG_FAST_NAT_MAPPING_TIMEOUT))
        {
            continue;
        }

        smp_store_release(&entry->state, LINKG_FAST_NAT_ENTRY_TOMBSTONE);

        if (g_mapping.active_count > 0U)
        {
            g_mapping.active_count--;
        }
    }

    spin_unlock_irqrestore(&g_mapping.lock, flags);

rearm:
    if (READ_ONCE(g_mapping.running))
    {
        mod_timer(&g_mapping.gc_timer, jiffies + LINKG_FAST_NAT_MAPPING_GC_INTERVAL);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Mapping超时回收定时器。
 */
void linkg_fast_nat_mapping_init(void)
{
    timer_setup(&g_mapping.gc_timer, _linkg_fast_nat_mapping_gc_timer, 0);
}

/**
 * @brief 启动Mapping超时回收定时器。
 */
void linkg_fast_nat_mapping_gc_start(void)
{
    WRITE_ONCE(g_mapping.running, true);
    mod_timer(&g_mapping.gc_timer, jiffies + LINKG_FAST_NAT_MAPPING_GC_INTERVAL);
}

/**
 * @brief 同步停止Mapping超时回收定时器。
 */
void linkg_fast_nat_mapping_gc_stop(void)
{
    WRITE_ONCE(g_mapping.running, false);
    del_timer_sync(&g_mapping.gc_timer);
}
