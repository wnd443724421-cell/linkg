/**
 * @file transport_forward.c
 * @brief LinkG传输层中继分片配对实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#include "transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_time.h"

_Static_assert((LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT & (LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT - 1U)) == 0U, "forward pair set count must be power of two");

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验直接Peer节点编号。
 */
static bool _linkg_transport_forward_peer_valid(uint8_t peer_node_id)
{
    return peer_node_id >= LINKG_RESOURCE_NODE_ID_MIN && peer_node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 计算中继分片配对缓存组索引。
 */
static uint32_t _linkg_transport_forward_pair_set(linkg_transport_class_t traffic_class, uint8_t peer_node_id, const linkg_transport_header_t *header, uint32_t packet_id)
{
    uint32_t hash;

    hash  = packet_id * 0x85EBCA6BU;
    hash ^= (uint32_t)header->source_node_id * 0x9E3779B1U;
    hash ^= (uint32_t)header->destination_node_id * 0xC2B2AE35U;
    hash ^= (uint32_t)header->type * 0x27D4EB2FU;
    hash ^= (uint32_t)peer_node_id * 0x165667B1U;
    hash ^= (uint32_t)traffic_class * 0xD3A2646CU;
    hash ^= hash >> 16U;

    return hash & (LINKG_TRANSPORT_FORWARD_PAIR_SET_COUNT - 1U);
}

/**
 * @brief 判断中继分片配对项是否匹配当前Peer/Class原始数据包。
 */
static bool _linkg_transport_forward_pair_match(const linkg_transport_forward_pair_entry_t *entry, linkg_transport_class_t traffic_class, uint8_t peer_node_id, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header)
{
    if (entry == NULL || header == NULL || fragment_header == NULL || !entry->valid)
    {
        return false;
    }

    return entry->traffic_class == traffic_class &&
           entry->peer_node_id == peer_node_id &&
           entry->source_node_id == header->source_node_id &&
           entry->destination_node_id == header->destination_node_id &&
           entry->type == (linkg_transport_type_t)header->type &&
           entry->packet_id == fragment_header->packet_id;
}

/**
 * @brief 判断中继分片配对项是否已经过期。
 */
static bool _linkg_transport_forward_pair_expired(const linkg_transport_forward_pair_entry_t *entry, uint64_t now_us)
{
    return entry != NULL && entry->valid && entry->expires_at_us <= now_us;
}

/**
 * @brief 释放并清空中继分片配对项。
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
 * @brief 释放不完整中继分片配对项并累计丢弃量。
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
 * @brief 记录中继不完整分片组丢弃统计。
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
 * @brief 清理已经过期的中继分片配对项。
 *
 * @note 调用方必须持有g_transport.forward_pairs.lock。
 */
static void _linkg_transport_forward_pair_gc_locked(uint64_t now_us, uint64_t *dropped_bytes, uint64_t *dropped_frames)
{
    linkg_transport_forward_pair_entry_t *entry;
    uint32_t                              set;
    uint32_t                              way;

    if (g_transport.forward_pairs.last_gc_us != 0U && now_us - g_transport.forward_pairs.last_gc_us < LINKG_TRANSPORT_FORWARD_PAIR_GC_INTERVAL_US)
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
 * @brief 创建新的中继不完整分片配对项。
 *
 * @note Pair缓存额外持有当前Packet一个引用，Link RX原始引用保持不变。
 */
static int _linkg_transport_forward_pair_create_locked(linkg_transport_forward_pair_entry_t *entry, linkg_transport_class_t traffic_class, uint8_t peer_node_id, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, uint64_t now_us)
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
    entry->type                = (linkg_transport_type_t)header->type;
    entry->traffic_class       = traffic_class;
    entry->packet_length       = fragment_header->packet_length;
    entry->fragment_offset     = fragment_header->fragment_offset;
    entry->source_node_id      = header->source_node_id;
    entry->destination_node_id = header->destination_node_id;
    entry->peer_node_id        = peer_node_id;
    entry->valid               = true;

    return 0;
}

/**
 * @brief 完成中继两片配对并移交两个完整Wire Frame引用。
 *
 * @note 无论实际到达顺序如何，输出始终严格按照FIRST、LAST排列。
 */
static int _linkg_transport_forward_pair_complete_locked(linkg_transport_forward_pair_entry_t *entry, uint8_t peer_node_id, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, linkg_transport_forward_item_t *output_items, uint32_t *output_count)
{
    linkg_packet_t *first_packet;
    linkg_packet_t *tail_packet;
    uint32_t        first_payload_length;
    uint32_t        tail_payload_length;

    if (entry == NULL || header == NULL || fragment_header == NULL || packet == NULL || output_items == NULL || output_count == NULL)
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

    first_packet->flags = (first_packet->flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) | LINKG_PACKET_FLAG_TX_GROUP_FIRST;
    tail_packet->flags  = (tail_packet->flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) | LINKG_PACKET_FLAG_TX_GROUP_LAST;

    output_items[0].packet              = first_packet;
    output_items[0].payload_length      = first_payload_length;
    output_items[0].destination_node_id = header->destination_node_id;
    output_items[0].peer_node_id        = peer_node_id;

    output_items[1].packet              = tail_packet;
    output_items[1].payload_length      = tail_payload_length;
    output_items[1].destination_node_id = header->destination_node_id;
    output_items[1].peer_node_id        = peer_node_id;

    entry->packet = NULL;
    memset(entry, 0, sizeof(*entry));

    *output_count = LINKG_TRANSPORT_FRAGMENT_COUNT_MAX;

    return 0;
}

/**
 * @brief 在已持锁状态下提交一个中继分片。
 *
 * @return 返回0且output_count为0表示继续等待，返回0且output_count为2表示已经完成FIRST/LAST配对。
 */
static int _linkg_transport_forward_pair_submit_locked(linkg_transport_class_t traffic_class, uint8_t peer_node_id, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, uint64_t now_us, linkg_transport_forward_item_t *output_items, uint32_t *output_count, uint64_t *dropped_bytes, uint64_t *dropped_frames)
{
    linkg_transport_forward_pair_entry_t *candidate;
    linkg_transport_forward_pair_entry_t *entry;
    linkg_transport_forward_pair_entry_t *free_entry;
    linkg_transport_forward_pair_entry_t *oldest_entry;
    linkg_transport_forward_pair_entry_t *new_entry;
    uint32_t                              set;
    uint32_t                              way;

    if (header == NULL || fragment_header == NULL || packet == NULL || output_items == NULL || output_count == NULL)
    {
        return -EINVAL;
    }

    *output_count = 0U;

    set          = _linkg_transport_forward_pair_set(traffic_class, peer_node_id, header, fragment_header->packet_id);
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

        if (_linkg_transport_forward_pair_match(candidate, traffic_class, peer_node_id, header, fragment_header))
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

        return _linkg_transport_forward_pair_create_locked(new_entry, traffic_class, peer_node_id, header, fragment_header, packet, payload_length, now_us);
    }

    if (entry->packet_length != fragment_header->packet_length)
    {
        _linkg_transport_forward_pair_entry_drop(entry, dropped_bytes, dropped_frames);
        return -EPROTO;
    }

    if (entry->fragment_offset == fragment_header->fragment_offset)
    {
        return 0;
    }

    return _linkg_transport_forward_pair_complete_locked(entry, peer_node_id, header, fragment_header, packet, payload_length, output_items, output_count);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化中继分片配对资源。
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
 * @brief 释放全部中继分片配对资源。
 *
 * @note 调用前Link RX数据面必须已经停止，不得再有新的Pair Submit进入。
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

/**
 * @brief 清理指定直接Peer残留的全部中继分片配对项。
 */
int linkg_transport_forward_pair_reset_peer(uint8_t peer_node_id)
{
    linkg_transport_forward_pair_entry_t *entry;
    uint64_t                              dropped_bytes;
    uint64_t                              dropped_frames;
    uint32_t                              set;
    uint32_t                              way;
    int                                   ret;

    if (!_linkg_transport_forward_peer_valid(peer_node_id))
    {
        return -EINVAL;
    }

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

            if (entry->valid && entry->peer_node_id == peer_node_id)
            {
                _linkg_transport_forward_pair_entry_drop(entry, &dropped_bytes, &dropped_frames);
            }
        }
    }

    ret = pthread_mutex_unlock(&g_transport.forward_pairs.lock);
    if (ret != 0)
    {
        return -ret;
    }

    _linkg_transport_forward_record_incomplete(dropped_bytes, dropped_frames);

    return 0;
}

/****************************** 分片配对 ******************************/

/**
 * @brief 提交一个AP中继分片并尝试完成Peer/Class两片配对。
 *
 * @note 当前Packet进入Pair Cache时由缓存额外retain；output_count为2时两个输出Packet引用均移交给调用方负责最终release。
 */
int linkg_transport_forward_pair_submit(linkg_transport_class_t traffic_class, uint8_t peer_node_id, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint32_t payload_length, linkg_transport_forward_item_t *output_items, uint32_t *output_count)
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

    if (!linkg_transport_class_valid(traffic_class) || !_linkg_transport_forward_peer_valid(peer_node_id) || header == NULL || fragment_header == NULL || packet == NULL || output_items == NULL || output_count == NULL || payload_length == 0U)
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

    ret = _linkg_transport_forward_pair_submit_locked(traffic_class, peer_node_id, header, fragment_header, packet, payload_length, now_us, output_items, output_count, &dropped_bytes, &dropped_frames);

    unlock_ret = pthread_mutex_unlock(&g_transport.forward_pairs.lock);

    _linkg_transport_forward_record_incomplete(dropped_bytes, dropped_frames);

    if (ret != 0)
    {
        return ret;
    }

    return unlock_ret == 0 ? 0 : -unlock_ret;
}
