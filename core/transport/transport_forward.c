/**
 * @file transport_forward.c
 * @brief LinkG AP同步批量转发实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-29
 */

#include "transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "linkg_scheduler.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t sequence;     // 新一跳Transport序列号
    int      result;       // 当前转发结果
    uint8_t  peer_node_id; // 当前直接Peer节点编号
    bool     prepared;     // Peer和Sequence是否准备完成
} linkg_transport_forward_state_t;

_Static_assert((LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT &
                (LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT - 1U)) == 0U,
               "forward pair set count must be power of two");

/****************************** 分片配对 ******************************/

/**
 * @brief 计算AP分片配对缓存组索引。
 */
static uint32_t _linkg_transport_forward_pair_set(const linkg_transport_header_t *header, uint32_t packet_id)
{
    uint32_t hash;

    hash  = packet_id * 0x85EBCA6BU;
    hash ^= (uint32_t)header->source_node_id * 0x9E3779B1U;
    hash ^= (uint32_t)header->destination_node_id * 0xC2B2AE35U;
    hash ^= (uint32_t)header->type * 0x27D4EB2FU;
    hash ^= hash >> 16U;

    return hash & (LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT - 1U);
}

/**
 * @brief 判断AP分片配对项是否匹配。
 */
static bool _linkg_transport_forward_pair_match(const linkg_transport_forward_pair_entry_t *entry, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header)
{
    if (entry == NULL || header == NULL || fragment_header == NULL || !entry->valid)
    {
        return false;
    }

    return entry->source_node_id == header->source_node_id &&
           entry->destination_node_id == header->destination_node_id &&
           entry->type == (linkg_transport_type_t)header->type &&
           entry->packet_id == fragment_header->packet_id;
}

/**
 * @brief 判断AP分片配对项是否已经过期。
 */
static bool _linkg_transport_forward_pair_expired(const linkg_transport_forward_pair_entry_t *entry, uint64_t now_us)
{
    if (entry == NULL || !entry->valid)
    {
        return false;
    }

    return entry->expires_at_us <= now_us;
}

/**
 * @brief 释放并清空AP分片配对项。
 */
static void _linkg_transport_forward_pair_entry_clear(linkg_transport_forward_pair_entry_t *entry)
{
    if (entry == NULL)
    {
        return;
    }

    if (entry->packet != NULL)
    {
        linkg_packet_release(entry->packet);
    }

    memset(entry, 0, sizeof(*entry));
}

/**
 * @brief 释放不完整配对项并累计本次丢弃统计。
 */
static void _linkg_transport_forward_pair_entry_drop(linkg_transport_forward_pair_entry_t *entry, uint64_t *dropped_bytes, uint64_t *dropped_frames)
{
    if (entry == NULL || !entry->valid)
    {
        return;
    }

    if (entry->packet != NULL)
    {
        if (dropped_bytes != NULL)
        {
            *dropped_bytes += entry->payload_length;
        }

        if (dropped_frames != NULL)
        {
            (*dropped_frames)++;
        }
    }

    _linkg_transport_forward_pair_entry_clear(entry);
}

/**
 * @brief 记录AP不完整分片组丢弃统计。
 */
static void _linkg_transport_forward_record_incomplete(uint64_t bytes, uint64_t frames)
{
    if (bytes == 0U && frames == 0U)
    {
        return;
    }

    pthread_mutex_lock(&g_transport.lock);

    g_transport.stats.forward_incomplete_bytes += bytes;
    g_transport.stats.forward_incomplete_frames += frames;

    pthread_mutex_unlock(&g_transport.lock);
}

/**
 * @brief 清理已经过期的AP分片配对项。
 *
 * 调用方必须持有g_transport.forward_pairs.lock。
 */
static void _linkg_transport_forward_pair_gc_locked(uint64_t now_us, uint64_t *dropped_bytes, uint64_t *dropped_frames)
{
    linkg_transport_forward_pair_entry_t *entry;
    uint32_t                              set;
    uint32_t                              way;

    if (g_transport.forward_pairs.last_gc_us != 0U &&
        now_us - g_transport.forward_pairs.last_gc_us < LINKG_TRANSPORT_FORWARD_PAIR_GC_INTERVAL_US)
    {
        return;
    }

    for (set = 0U; set < LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT; set++)
    {
        for (way = 0U; way < LINKG_TRANSPORT_FORWARD_PAIR_WAYS; way++)
        {
            entry = &g_transport.forward_pairs.entries[set][way];

            if (_linkg_transport_forward_pair_expired(entry, now_us))
            {
                _linkg_transport_forward_pair_entry_drop(entry, dropped_bytes, dropped_frames);
            }
        }
    }

    g_transport.forward_pairs.last_gc_us = now_us;
}

/**
 * @brief 创建新的AP不完整分片配对项。
 *
 * Pair缓存额外持有当前Packet一个引用，Link RX原始引用保持不变。
 */
static int _linkg_transport_forward_pair_create_locked(linkg_transport_forward_pair_entry_t *entry, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, uint64_t now_us)
{
    if (entry == NULL || header == NULL || fragment_header == NULL || packet == NULL)
    {
        return -EINVAL;
    }

    linkg_packet_retain(packet);

    memset(entry, 0, sizeof(*entry));

    entry->packet              = packet;
    entry->expires_at_us       = now_us + LINKG_TRANSPORT_FORWARD_PAIR_TTL_US;
    entry->packet_id           = fragment_header->packet_id;
    entry->payload_length      = payload_length;
    entry->packet_length       = fragment_header->packet_length;
    entry->fragment_offset     = fragment_header->fragment_offset;
    entry->type                = (linkg_transport_type_t)header->type;
    entry->source_node_id      = header->source_node_id;
    entry->destination_node_id = header->destination_node_id;
    entry->valid               = true;

    return 0;
}

/**
 * @brief 完成AP两片配对并移交两个Packet引用。
 *
 * 无论实际到达顺序如何，输出始终严格按照FIRST、LAST顺序排列。
 * entry原有引用和当前Packet新增引用均移交给output_items调用方。
 */
static int _linkg_transport_forward_pair_complete_locked(linkg_transport_forward_pair_entry_t *entry, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, linkg_transport_forward_item_t *output_items, uint32_t *output_count)
{
    linkg_packet_t *first_packet;
    linkg_packet_t *tail_packet;
    uint32_t        first_payload_length;
    uint32_t        tail_payload_length;

    if (entry == NULL ||
        header == NULL ||
        fragment_header == NULL ||
        packet == NULL ||
        output_items == NULL ||
        output_count == NULL)
    {
        return -EINVAL;
    }

    linkg_packet_retain(packet);

    if (entry->fragment_offset == 0U)
    {
        first_packet         = entry->packet;
        first_payload_length = entry->payload_length;
        tail_packet          = packet;
        tail_payload_length  = payload_length;
    }
    else
    {
        first_packet         = packet;
        first_payload_length = payload_length;
        tail_packet          = entry->packet;
        tail_payload_length  = entry->payload_length;
    }

    first_packet->flags = (first_packet->flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) |
                          LINKG_PACKET_FLAG_TX_GROUP_FIRST;

    tail_packet->flags = (tail_packet->flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) |
                         LINKG_PACKET_FLAG_TX_GROUP_LAST;

    output_items[0].packet              = first_packet;
    output_items[0].payload_length      = first_payload_length;
    output_items[0].type                = (linkg_transport_type_t)header->type;
    output_items[0].destination_node_id = header->destination_node_id;

    output_items[1].packet              = tail_packet;
    output_items[1].payload_length      = tail_payload_length;
    output_items[1].type                = (linkg_transport_type_t)header->type;
    output_items[1].destination_node_id = header->destination_node_id;

    // entry持有的引用已经移交给output_items，清空时不得再次release。
    entry->packet = NULL;

    memset(entry, 0, sizeof(*entry));

    *output_count = LINKG_TRANSPORT_FRAGMENT_COUNT_MAX;

    return 0;
}

/**
 * @brief 在已持锁状态下提交一个AP转发分片。
 *
 * 返回0且output_count为0表示继续等待另一片；
 * 返回0且output_count为2表示已经完成FIRST/LAST配对。
 */
static int _linkg_transport_forward_pair_submit_locked(const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, uint64_t now_us, linkg_transport_forward_item_t *output_items, uint32_t *output_count, uint64_t *dropped_bytes, uint64_t *dropped_frames)
{
    linkg_transport_forward_pair_entry_t *candidate;
    linkg_transport_forward_pair_entry_t *entry;
    linkg_transport_forward_pair_entry_t *free_entry;
    linkg_transport_forward_pair_entry_t *oldest_entry;
    linkg_transport_forward_pair_entry_t *new_entry;
    uint32_t                              set;
    uint32_t                              way;

    if (header == NULL ||
        fragment_header == NULL ||
        packet == NULL ||
        output_items == NULL ||
        output_count == NULL)
    {
        return -EINVAL;
    }

    *output_count = 0U;

    set          = _linkg_transport_forward_pair_set(header, fragment_header->packet_id);
    entry        = NULL;
    free_entry   = NULL;
    oldest_entry = NULL;

    for (way = 0U; way < LINKG_TRANSPORT_FORWARD_PAIR_WAYS; way++)
    {
        candidate = &g_transport.forward_pairs.entries[set][way];

        if (_linkg_transport_forward_pair_expired(candidate, now_us))
        {
            _linkg_transport_forward_pair_entry_drop(candidate, dropped_bytes, dropped_frames);
        }

        if (_linkg_transport_forward_pair_match(candidate, header, fragment_header))
        {
            entry = candidate;
            break;
        }

        if (!candidate->valid)
        {
            if (free_entry == NULL)
            {
                free_entry = candidate;
            }

            continue;
        }

        if (oldest_entry == NULL || candidate->expires_at_us < oldest_entry->expires_at_us)
        {
            oldest_entry = candidate;
        }
    }

    if (entry == NULL)
    {
        new_entry = free_entry != NULL ? free_entry : oldest_entry;

        if (new_entry == NULL)
        {
            return -ENOSPC;
        }

        if (new_entry->valid)
        {
            _linkg_transport_forward_pair_entry_drop(new_entry, dropped_bytes, dropped_frames);
        }

        return _linkg_transport_forward_pair_create_locked(new_entry, header, fragment_header, packet, payload_length, now_us);
    }

    if (entry->packet_length != fragment_header->packet_length)
    {
        _linkg_transport_forward_pair_entry_drop(entry, dropped_bytes, dropped_frames);
        return -EPROTO;
    }

    // 同一片再次出现直接忽略，不刷新Pair固定TTL。
    if (entry->fragment_offset == fragment_header->fragment_offset)
    {
        return 0;
    }

    return _linkg_transport_forward_pair_complete_locked(entry,
                                                          header,
                                                          fragment_header,
                                                          packet,
                                                          payload_length,
                                                          output_items,
                                                          output_count);
}

/****************************** Pair生命周期 ******************************/

/**
 * @brief 初始化AP转发分片配对资源。
 */
int linkg_transport_forward_pair_runtime_init(void)
{
    int ret;

    if (g_transport.forward_pairs.initialized)
    {
        return -EALREADY;
    }

    memset(&g_transport.forward_pairs, 0, sizeof(g_transport.forward_pairs));

    ret = pthread_mutex_init(&g_transport.forward_pairs.lock, NULL);
    if (ret != 0)
    {
        memset(&g_transport.forward_pairs, 0, sizeof(g_transport.forward_pairs));
        return -ret;
    }

    g_transport.forward_pairs.initialized = true;

    return 0;
}

/**
 * @brief 释放全部AP转发分片配对资源。
 *
 * 调用前Link RX数据面必须已经停止，不得再有新的Pair Submit进入。
 */
int linkg_transport_forward_pair_runtime_deinit(void)
{
    linkg_transport_forward_pair_entry_t *entry;
    uint64_t                              dropped_bytes;
    uint64_t                              dropped_frames;
    uint32_t                              set;
    uint32_t                              way;
    int                                   ret;

    if (!g_transport.forward_pairs.initialized)
    {
        return 0;
    }

    dropped_bytes  = 0U;
    dropped_frames = 0U;

    ret = pthread_mutex_lock(&g_transport.forward_pairs.lock);
    if (ret != 0)
    {
        return -ret;
    }

    for (set = 0U; set < LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT; set++)
    {
        for (way = 0U; way < LINKG_TRANSPORT_FORWARD_PAIR_WAYS; way++)
        {
            entry = &g_transport.forward_pairs.entries[set][way];

            _linkg_transport_forward_pair_entry_drop(entry, &dropped_bytes, &dropped_frames);
        }
    }

    g_transport.forward_pairs.initialized = false;

    ret = pthread_mutex_unlock(&g_transport.forward_pairs.lock);
    if (ret != 0)
    {
        g_transport.forward_pairs.initialized = true;
        return -ret;
    }

    _linkg_transport_forward_record_incomplete(dropped_bytes, dropped_frames);

    ret = pthread_mutex_destroy(&g_transport.forward_pairs.lock);
    if (ret != 0)
    {
        g_transport.forward_pairs.initialized = true;
        return -ret;
    }

    memset(&g_transport.forward_pairs, 0, sizeof(g_transport.forward_pairs));

    return 0;
}

/****************************** Pair提交 ******************************/

/**
 * @brief 提交一个AP转发分片并尝试完成两片配对。
 *
 * 当前Packet在进入Pair Cache时由缓存额外retain；
 * output_count为2时两个输出Packet引用均移交给调用方负责最终release。
 */
int linkg_transport_forward_pair_submit(const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, linkg_transport_forward_item_t *output_items, uint32_t *output_count)
{
    uint64_t dropped_bytes;
    uint64_t dropped_frames;
    uint64_t now_us;
    int      unlock_ret;
    int      ret;

    if (!g_transport.initialized || !g_transport.forward_pairs.initialized)
    {
        return -ENODEV;
    }

    if (g_transport.local_role != LINKG_DEVICE_ROLE_AP)
    {
        return -EPERM;
    }

    if (header == NULL ||
        fragment_header == NULL ||
        packet == NULL ||
        output_items == NULL ||
        output_count == NULL ||
        payload_length == 0U)
    {
        return -EINVAL;
    }

    if ((header->flags & LINKG_TRANSPORT_FLAG_FRAGMENT) == 0U)
    {
        return -EINVAL;
    }

    *output_count  = 0U;
    dropped_bytes  = 0U;
    dropped_frames = 0U;
    now_us         = linkg_time_elapsed_us();

    ret = pthread_mutex_lock(&g_transport.forward_pairs.lock);
    if (ret != 0)
    {
        return -ret;
    }

    _linkg_transport_forward_pair_gc_locked(now_us, &dropped_bytes, &dropped_frames);

    ret = _linkg_transport_forward_pair_submit_locked(header,
                                                       fragment_header,
                                                       packet,
                                                       payload_length,
                                                       now_us,
                                                       output_items,
                                                       output_count,
                                                       &dropped_bytes,
                                                       &dropped_frames);

    unlock_ret = pthread_mutex_unlock(&g_transport.forward_pairs.lock);

    _linkg_transport_forward_record_incomplete(dropped_bytes, dropped_frames);

    if (ret != 0)
    {
        return ret;
    }

    return unlock_ret == 0 ? 0 : -unlock_ret;
}

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验节点编号。
 */
static bool _linkg_transport_forward_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN &&
           node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 校验同步转发批次参数。
 */
static int _linkg_transport_forward_validate_batch(const linkg_transport_forward_item_t *items, uint32_t count)
{
    uint32_t index;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (items == NULL || count == 0U || count > LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        return -EINVAL;
    }

    if (g_transport.local_role != LINKG_DEVICE_ROLE_AP)
    {
        return -EPERM;
    }

    for (index = 0U; index < count; index++)
    {
        if (items[index].packet == NULL)
        {
            return -EINVAL;
        }

        if (!_linkg_transport_forward_node_id_valid(items[index].destination_node_id))
        {
            return -EINVAL;
        }

        if (items[index].destination_node_id == g_transport.local_node_id)
        {
            return -EINVAL;
        }

        if (!linkg_transport_type_valid(items[index].type))
        {
            return -EINVAL;
        }

        if (items[index].payload_length == 0U)
        {
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief 为AP转发批次确定直接Peer并分配新的逐跳序列号。
 *
 * 整个批次只获取一次g_transport.lock，释放状态锁后再进入Scheduler发送路径。
 */
static int _linkg_transport_forward_prepare_batch(const linkg_transport_forward_item_t *items, linkg_transport_forward_state_t *states, uint32_t count)
{
    linkg_transport_peer_t *cached_peer;
    linkg_transport_peer_t *peer;
    uint32_t                index;
    uint8_t                 cached_destination_node_id;
    bool                    cache_valid;
    int                     ret;

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    cached_peer                = NULL;
    cached_destination_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    cache_valid                = false;

    for (index = 0U; index < count; index++)
    {
        states[index].result   = -ENOENT;
        states[index].prepared = false;

        if (cache_valid && items[index].destination_node_id == cached_destination_node_id)
        {
            peer = cached_peer;
        }
        else
        {
            peer                       = linkg_transport_find_peer_locked(items[index].destination_node_id);
            cached_destination_node_id = items[index].destination_node_id;
            cached_peer                = peer;
            cache_valid                = true;
        }

        if (peer == NULL)
        {
            continue;
        }

        peer->tx_sequence++;

        states[index].sequence     = peer->tx_sequence;
        states[index].peer_node_id = peer->peer_node_id;
        states[index].result       = -EINPROGRESS;
        states[index].prepared     = true;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 批量记录AP同步转发Transport帧TX统计。
 *
 * RX统计已经在Transport接收窗口阶段完成，此处只记录转发产生的新一跳TX结果。
 */
static void _linkg_transport_forward_record_batch(const linkg_transport_forward_item_t *items, const linkg_transport_forward_state_t *states, uint32_t count)
{
    linkg_transport_type_stats_t *type_stats;
    linkg_transport_peer_t       *cached_peer;
    linkg_transport_peer_t       *peer;
    uint64_t                      now_ms;
    uint32_t                      index;
    uint8_t                       cached_peer_node_id;
    bool                          cache_valid;
    bool                          now_valid;

    cached_peer         = NULL;
    cached_peer_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    cache_valid         = false;
    now_ms              = 0U;
    now_valid           = false;

    pthread_mutex_lock(&g_transport.lock);

    for (index = 0U; index < count; index++)
    {
        if (!states[index].prepared)
        {
            continue;
        }

        if (cache_valid && states[index].peer_node_id == cached_peer_node_id)
        {
            peer = cached_peer;
        }
        else
        {
            peer                = linkg_transport_find_peer_locked(states[index].peer_node_id);
            cached_peer_node_id = states[index].peer_node_id;
            cached_peer         = peer;
            cache_valid         = true;
        }

        if (peer == NULL)
        {
            continue;
        }

        type_stats = &peer->stats.types[items[index].type];

        if (states[index].result == 0)
        {
            type_stats->tx_packets++;
            type_stats->tx_bytes += items[index].payload_length;

            if (!now_valid)
            {
                now_ms    = linkg_time_elapsed_ms();
                now_valid = true;
            }

            peer->stats.last_tx_ms = now_ms;
        }
        else
        {
            type_stats->tx_failed_packets++;
            type_stats->tx_failed_bytes += items[index].payload_length;
        }
    }

    pthread_mutex_unlock(&g_transport.lock);
}

/****************************** 转发处理 ******************************/

/**
 * @brief 同步批量转发AP收到的非本机目标Transport帧。
 *
 * 当前函数只在调用期间借用Packet引用，不修改Packet引用所有权。
 * 转发保持原始source_node_id、destination_node_id、packet_id和分片边界，
 * 仅重新分配当前下一跳Transport sequence。
 */
int linkg_transport_forward_batch(const linkg_transport_forward_item_t *items, uint32_t count)
{
    linkg_scheduler_tx_item_t        scheduler_items[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    linkg_transport_forward_state_t states[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    uint32_t                        scheduler_indices[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    uint32_t                        scheduler_count;
    uint32_t                        item_index;
    uint32_t                        index;
    int                             ret;

    ret = _linkg_transport_forward_validate_batch(items, count);
    if (ret != 0)
    {
        return ret;
    }

    memset(states, 0, sizeof(states));
    memset(scheduler_items, 0, sizeof(scheduler_items));

    ret = _linkg_transport_forward_prepare_batch(items, states, count);
    if (ret != 0)
    {
        return ret;
    }

    scheduler_count = 0U;

    for (index = 0U; index < count; index++)
    {
        if (!states[index].prepared)
        {
            continue;
        }

        ret = linkg_transport_wire_update_sequence(items[index].packet, states[index].sequence);
        if (ret != 0)
        {
            states[index].result = ret;
            continue;
        }

        scheduler_items[scheduler_count].packet           = items[index].packet;
        scheduler_items[scheduler_count].next_hop_node_id = states[index].peer_node_id;
        scheduler_items[scheduler_count].result           = -EINPROGRESS;

        scheduler_indices[scheduler_count] = index;
        scheduler_count++;
    }

    if (scheduler_count > 0U)
    {
        ret = linkg_scheduler_submit_batch(scheduler_items,
                                           scheduler_count,
                                           LINKG_SCHEDULER_POLICY_DEFAULT,
                                           LINKG_LINK_ID_INVALID);

        if (ret < 0)
        {
            for (index = 0U; index < scheduler_count; index++)
            {
                item_index = scheduler_indices[index];
                states[item_index].result = ret;
            }
        }
        else
        {
            for (index = 0U; index < scheduler_count; index++)
            {
                item_index = scheduler_indices[index];

                states[item_index].result = scheduler_items[index].result;

                if (states[item_index].result == -EINPROGRESS)
                {
                    LINKG_LOG_ERROR("transport forward scheduler result not completed, index=%u", index);
                    states[item_index].result = -EIO;
                }
            }
        }
    }

    _linkg_transport_forward_record_batch(items, states, count);

    return 0;
}
