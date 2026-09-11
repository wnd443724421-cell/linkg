/**
 * @file transport_reassembly.c
 * @brief LinkG传输层本机分片重组实现
 * @author Dawn
 * @version 1.5.0
 * @date 2026-09-11
 */

#include "transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_FIRST   (1U << 0)                                                                                // 首片接收标志
#define LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_SECOND  (1U << 1)                                                                                // 尾片接收标志
#define LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_ALL     (LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_FIRST | LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_SECOND) // 全部分片接收标志

_Static_assert((LINKG_TRANSPORT_REASSEMBLY_SET_COUNT & (LINKG_TRANSPORT_REASSEMBLY_SET_COUNT - 1U)) == 0U, "reassembly set count must be power of two");

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验直接Peer节点编号。
 */
static bool _linkg_transport_reassembly_peer_valid(uint8_t peer_node_id)
{
    return peer_node_id >= LINKG_RESOURCE_NODE_ID_MIN && peer_node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 计算本机重组缓存组索引。
 */
static uint32_t _linkg_transport_reassembly_set(linkg_transport_class_t traffic_class, uint8_t peer_node_id, uint32_t peer_epoch, const linkg_transport_header_t *header, uint32_t packet_id)
{
    uint32_t hash;

    hash  = packet_id * 0x85EBCA6BU;
    hash ^= (uint32_t)header->source_node_id * 0x9E3779B1U;
    hash ^= (uint32_t)header->destination_node_id * 0xC2B2AE35U;
    hash ^= (uint32_t)header->type * 0x27D4EB2FU;
    hash ^= (uint32_t)peer_node_id * 0x165667B1U;
    hash ^= (uint32_t)traffic_class * 0xD3A2646CU;
    hash ^= peer_epoch * 0xA24BAED5U;
    hash ^= hash >> 16U;

    return hash & (LINKG_TRANSPORT_REASSEMBLY_SET_COUNT - 1U);
}

/**
 * @brief 判断重组项是否匹配当前Peer/Class原始数据包。
 */
static bool _linkg_transport_reassembly_match(const linkg_transport_reassembly_entry_t *entry, linkg_transport_class_t traffic_class, uint8_t peer_node_id, uint32_t peer_epoch, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header)
{
    if (entry == NULL || header == NULL || fragment_header == NULL || !entry->valid)
    {
        return false;
    }

    return entry->traffic_class == traffic_class &&
           entry->peer_node_id == peer_node_id &&
           entry->peer_epoch == peer_epoch &&
           entry->source_node_id == header->source_node_id &&
           entry->destination_node_id == header->destination_node_id &&
           entry->type == (linkg_transport_type_t)header->type &&
           entry->packet_id == fragment_header->packet_id;
}

/**
 * @brief 判断重组项是否已经过期。
 */
static bool _linkg_transport_reassembly_expired(const linkg_transport_reassembly_entry_t *entry, uint64_t now_us)
{
    return entry != NULL && entry->valid && entry->expires_at_us <= now_us;
}

/**
 * @brief 释放并清空重组项。
 */
static void _linkg_transport_reassembly_entry_clear(linkg_transport_reassembly_entry_t *entry)
{
    if (entry == NULL)
    {
        return;
    }

    if (entry->first_packet != NULL)
    {
        linkg_packet_release(entry->first_packet);
    }

    if (entry->tail_packet != NULL)
    {
        linkg_packet_release(entry->tail_packet);
    }

    memset(entry, 0, sizeof(*entry));
}

/**
 * @brief 记录Peer重置或注销时主动释放的本机重组分片数量。
 *
 * @note 调用前不得持有g_transport.reassembly.lock。
 */
static void _linkg_transport_reassembly_record_reset(uint64_t frames)
{
    if (frames == 0U)
    {
        return;
    }

    pthread_mutex_lock(&g_transport.lock);

    g_transport.stats.reassembly_reset_frames += frames;

    pthread_mutex_unlock(&g_transport.lock);
}

/**
 * @brief 清理已经过期的本机重组项。
 *
 * @note 调用方必须持有g_transport.reassembly.lock。
 */
static void _linkg_transport_reassembly_gc_locked(uint64_t now_us)
{
    linkg_transport_reassembly_entry_t *entry;
    uint32_t                            set;
    uint32_t                            way;

    if (g_transport.reassembly.last_gc_us != 0U && now_us - g_transport.reassembly.last_gc_us < LINKG_TRANSPORT_REASSEMBLY_GC_INTERVAL_US)
    {
        return;
    }

    for (set = 0U; set < LINKG_TRANSPORT_REASSEMBLY_SET_COUNT; set++)
    {
        for (way = 0U; way < LINKG_TRANSPORT_REASSEMBLY_WAYS; way++)
        {
            entry = &g_transport.reassembly.entries[set][way];

            if (_linkg_transport_reassembly_expired(entry, now_us))
            {
                _linkg_transport_reassembly_entry_clear(entry);
            }
        }
    }

    g_transport.reassembly.last_gc_us = now_us;
}

/**
 * @brief 获取当前分片对应的接收标志。
 */
static uint8_t _linkg_transport_reassembly_fragment_bit(uint16_t fragment_offset)
{
    return fragment_offset == 0U ? LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_FIRST : LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_SECOND;
}

/**
 * @brief 校验固定两片布局并返回当前分片载荷长度。
 */
static int _linkg_transport_reassembly_fragment_length(uint16_t fragment_offset, uint16_t packet_length, const linkg_packet_t *packet, uint32_t *fragment_length)
{
    uint32_t length;

    if (packet == NULL || fragment_length == NULL || packet->pool == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet_length <= LINKG_TRANSPORT_PAYLOAD_MAX_SIZE || packet_length > LINKG_TRANSPORT_PACKET_MAX_SIZE)
    {
        return -EPROTO;
    }

    if (packet->data_length < LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE + 1U)
    {
        return -EMSGSIZE;
    }

    length = packet->data_length - LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE;

    if (fragment_offset == 0U)
    {
        if (length != LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE)
        {
            return -EPROTO;
        }
    }
    else if (fragment_offset != LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE || length != (uint32_t)packet_length - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE)
    {
        return -EPROTO;
    }

    *fragment_length = length;

    return 0;
}

/**
 * @brief 缓存首片并将其转换为最终完整Packet载体。
 *
 * @note 成功后重组项持有Packet一个引用。
 */
static int _linkg_transport_reassembly_store_first(linkg_transport_reassembly_entry_t *entry, linkg_packet_t *packet)
{
    uint32_t fragment_length;
    uint32_t payload_offset;
    int      ret;

    if (entry == NULL || packet == NULL || packet->pool == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_transport_reassembly_fragment_length(0U, entry->packet_length, packet, &fragment_length);
    if (ret != 0)
    {
        return ret;
    }

    if (packet->data_offset > packet->pool->slot_size || LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE > packet->pool->slot_size - packet->data_offset)
    {
        return -ENOBUFS;
    }

    payload_offset = packet->data_offset + LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE;

    if (entry->packet_length > packet->pool->slot_size - payload_offset)
    {
        return -ENOBUFS;
    }

    linkg_packet_retain(packet);

    if (linkg_packet_pull(packet, LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE) == NULL)
    {
        linkg_packet_release(packet);
        return -EMSGSIZE;
    }

    if (packet->data_length != fragment_length)
    {
        linkg_packet_release(packet);
        return -EPROTO;
    }

    entry->first_packet = packet;
    entry->received_mask |= LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_FIRST;

    return 0;
}

/**
 * @brief 缓存先到达的尾片。
 *
 * @note 成功后重组项持有Packet一个引用。
 */
static int _linkg_transport_reassembly_store_tail(linkg_transport_reassembly_entry_t *entry, linkg_packet_t *packet)
{
    uint32_t fragment_length;
    int      ret;

    if (entry == NULL || packet == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_transport_reassembly_fragment_length((uint16_t)LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE, entry->packet_length, packet, &fragment_length);
    if (ret != 0)
    {
        return ret;
    }

    (void)fragment_length;

    linkg_packet_retain(packet);

    entry->tail_packet = packet;
    entry->received_mask |= LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_SECOND;

    return 0;
}

/**
 * @brief 将尾片载荷追加到首片Packet末尾。
 */
static int _linkg_transport_reassembly_append_tail(linkg_transport_reassembly_entry_t *entry, const linkg_packet_t *tail_packet)
{
    const uint8_t *source_data;
    uint8_t       *destination_data;
    uint32_t       tail_length;
    int            ret;

    if (entry == NULL || entry->first_packet == NULL || tail_packet == NULL || tail_packet->pool == NULL || tail_packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (entry->first_packet->pool == NULL || entry->first_packet->slot == NULL || entry->first_packet->data_length != LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE)
    {
        return -EPROTO;
    }

    ret = _linkg_transport_reassembly_fragment_length((uint16_t)LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE, entry->packet_length, tail_packet, &tail_length);
    if (ret != 0)
    {
        return ret;
    }

    if (entry->first_packet->data_offset > entry->first_packet->pool->slot_size || entry->packet_length > entry->first_packet->pool->slot_size - entry->first_packet->data_offset)
    {
        return -ENOBUFS;
    }

    source_data      = linkg_packet_const_data(tail_packet) + LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE;
    destination_data = linkg_packet_data(entry->first_packet) + LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;

    memcpy(destination_data, source_data, tail_length);

    entry->first_packet->data_length = entry->packet_length;
    entry->received_mask |= LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_SECOND;

    return 0;
}

/**
 * @brief 创建本机重组项并缓存当前分片。
 *
 * @note TTL从当前原始Packet首个到达分片开始固定计算。
 *       调用方必须持有g_transport.reassembly.lock。
 */
static int _linkg_transport_reassembly_create_entry_locked(linkg_transport_reassembly_entry_t *entry, linkg_transport_class_t traffic_class, uint8_t peer_node_id, uint32_t peer_epoch, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint64_t now_us)
{
    uint32_t fragment_length;
    int      ret;

    if (entry == NULL || header == NULL || fragment_header == NULL || packet == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_transport_reassembly_fragment_length(fragment_header->fragment_offset, fragment_header->packet_length, packet, &fragment_length);
    if (ret != 0)
    {
        return ret;
    }

    (void)fragment_length;

    memset(entry, 0, sizeof(*entry));

    entry->expires_at_us       = now_us + LINKG_TRANSPORT_REASSEMBLY_TTL_US;
    entry->packet_id           = fragment_header->packet_id;
    entry->peer_epoch          = peer_epoch;
    entry->type                = (linkg_transport_type_t)header->type;
    entry->traffic_class       = traffic_class;
    entry->packet_length       = fragment_header->packet_length;
    entry->source_node_id      = header->source_node_id;
    entry->destination_node_id = header->destination_node_id;
    entry->peer_node_id        = peer_node_id;
    entry->valid               = true;

    if (fragment_header->fragment_offset == 0U)
    {
        ret = _linkg_transport_reassembly_store_first(entry, packet);
    }
    else
    {
        ret = _linkg_transport_reassembly_store_tail(entry, packet);
    }

    if (ret != 0)
    {
        _linkg_transport_reassembly_entry_clear(entry);
        return ret;
    }

    return 0;
}

/**
 * @brief 完成分片重组并移交完整Packet引用。
 */
static int _linkg_transport_reassembly_complete(linkg_transport_reassembly_entry_t *entry, linkg_packet_t **completed_packet)
{
    linkg_packet_t *result_packet;

    if (entry == NULL || completed_packet == NULL || entry->first_packet == NULL || entry->received_mask != LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_ALL)
    {
        return -EINVAL;
    }

    if (entry->tail_packet != NULL)
    {
        linkg_packet_release(entry->tail_packet);
        entry->tail_packet = NULL;
    }

    result_packet        = entry->first_packet;
    entry->first_packet  = NULL;
    result_packet->data_length = entry->packet_length;

    memset(entry, 0, sizeof(*entry));

    *completed_packet = result_packet;

    return 1;
}

/**
 * @brief 在已持锁状态下提交一个Peer/Class分片。
 *
 * @return 1表示完成重组，0表示继续等待，负值表示处理失败。
 */
static int _linkg_transport_reassembly_submit_locked(linkg_transport_class_t traffic_class, uint8_t peer_node_id, uint32_t peer_epoch, const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, uint64_t now_us, linkg_packet_t **completed_packet)
{
    linkg_transport_reassembly_entry_t *candidate;
    linkg_transport_reassembly_entry_t *entry;
    linkg_transport_reassembly_entry_t *free_entry;
    linkg_transport_reassembly_entry_t *oldest_entry;
    linkg_transport_reassembly_entry_t *new_entry;
    uint32_t                            fragment_length;
    uint32_t                            set;
    uint32_t                            way;
    uint8_t                             expected_mask;
    uint8_t                             fragment_bit;
    int                                 ret;

    if (!linkg_transport_class_valid(traffic_class) || !_linkg_transport_reassembly_peer_valid(peer_node_id) || header == NULL || fragment_header == NULL || packet == NULL || completed_packet == NULL)
    {
        return -EINVAL;
    }

    *completed_packet = NULL;

    if ((header->flags & LINKG_TRANSPORT_FLAG_FRAGMENT) == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_transport_reassembly_fragment_length(fragment_header->fragment_offset, fragment_header->packet_length, packet, &fragment_length);
    if (ret != 0)
    {
        return ret;
    }

    (void)fragment_length;

    fragment_bit = _linkg_transport_reassembly_fragment_bit(fragment_header->fragment_offset);
    set          = _linkg_transport_reassembly_set(traffic_class, peer_node_id, peer_epoch, header, fragment_header->packet_id);
    entry        = NULL;
    free_entry   = NULL;
    oldest_entry = NULL;

    for (way = 0U; way < LINKG_TRANSPORT_REASSEMBLY_WAYS; way++)
    {
        candidate = &g_transport.reassembly.entries[set][way];

        if (_linkg_transport_reassembly_expired(candidate, now_us))
        {
            _linkg_transport_reassembly_entry_clear(candidate);
        }

        if (_linkg_transport_reassembly_match(candidate, traffic_class, peer_node_id, peer_epoch, header, fragment_header))
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
            _linkg_transport_reassembly_entry_clear(new_entry);
        }

        return _linkg_transport_reassembly_create_entry_locked(new_entry, traffic_class, peer_node_id, peer_epoch, header, fragment_header, packet, now_us);
    }

    if (entry->packet_length != fragment_header->packet_length)
    {
        _linkg_transport_reassembly_entry_clear(entry);
        return -EPROTO;
    }

    if ((entry->received_mask & fragment_bit) != 0U)
    {
        return 0;
    }

    expected_mask = (uint8_t)(LINKG_TRANSPORT_REASSEMBLY_FRAGMENT_ALL ^ fragment_bit);

    if (entry->received_mask != expected_mask)
    {
        _linkg_transport_reassembly_entry_clear(entry);
        return -EPROTO;
    }

    if (fragment_header->fragment_offset == 0U)
    {
        // LAST先到，当前FIRST到达，使用FIRST作为最终完整Packet载体。
        if (entry->first_packet != NULL || entry->tail_packet == NULL)
        {
            _linkg_transport_reassembly_entry_clear(entry);
            return -EPROTO;
        }

        ret = _linkg_transport_reassembly_store_first(entry, packet);
        if (ret == 0)
        {
            ret = _linkg_transport_reassembly_append_tail(entry, entry->tail_packet);
        }
    }
    else
    {
        // FIRST先到，当前LAST到达，直接将LAST载荷追加到缓存FIRST。
        if (entry->first_packet == NULL || entry->tail_packet != NULL)
        {
            _linkg_transport_reassembly_entry_clear(entry);
            return -EPROTO;
        }

        ret = _linkg_transport_reassembly_append_tail(entry, packet);
    }

    if (ret != 0)
    {
        _linkg_transport_reassembly_entry_clear(entry);
        return ret;
    }

    return _linkg_transport_reassembly_complete(entry, completed_packet);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化本机分片重组资源。
 */
int linkg_transport_reassembly_runtime_init(void)
{
    int ret;

    if (g_transport.reassembly.initialized)
    {
        return -EALREADY;
    }

    memset(&g_transport.reassembly, 0, sizeof(g_transport.reassembly));

    ret = pthread_mutex_init(&g_transport.reassembly.lock, NULL);
    if (ret != 0)
    {
        memset(&g_transport.reassembly, 0, sizeof(g_transport.reassembly));
        return -ret;
    }

    g_transport.reassembly.initialized = true;

    return 0;
}

/**
 * @brief 释放全部本机分片重组资源。
 */
int linkg_transport_reassembly_runtime_deinit(void)
{
    linkg_transport_reassembly_entry_t *entry;
    uint32_t                            set;
    uint32_t                            way;
    int                                 ret;

    if (!g_transport.reassembly.initialized)
    {
        return 0;
    }

    ret = pthread_mutex_lock(&g_transport.reassembly.lock);
    if (ret != 0)
    {
        return -ret;
    }

    for (set = 0U; set < LINKG_TRANSPORT_REASSEMBLY_SET_COUNT; set++)
    {
        for (way = 0U; way < LINKG_TRANSPORT_REASSEMBLY_WAYS; way++)
        {
            entry = &g_transport.reassembly.entries[set][way];
            _linkg_transport_reassembly_entry_clear(entry);
        }
    }

    g_transport.reassembly.initialized = false;

    ret = pthread_mutex_unlock(&g_transport.reassembly.lock);
    if (ret != 0)
    {
        g_transport.reassembly.initialized = true;
        return -ret;
    }

    ret = pthread_mutex_destroy(&g_transport.reassembly.lock);
    if (ret != 0)
    {
        g_transport.reassembly.initialized = true;
        return -ret;
    }

    memset(&g_transport.reassembly, 0, sizeof(g_transport.reassembly));

    return 0;
}

/**
 * @brief 清理指定直接Peer残留的全部本机重组项。
 */
int linkg_transport_reassembly_reset_peer(uint8_t peer_node_id)
{
    linkg_transport_reassembly_entry_t *entry;
    uint64_t                            reset_frames;
    uint32_t                            set;
    uint32_t                            way;
    int                                 ret;

    if (!_linkg_transport_reassembly_peer_valid(peer_node_id))
    {
        return -EINVAL;
    }

    if (!g_transport.reassembly.initialized)
    {
        return 0;
    }

    reset_frames = 0U;

    ret = pthread_mutex_lock(&g_transport.reassembly.lock);
    if (ret != 0)
    {
        return -ret;
    }

    for (set = 0U; set < LINKG_TRANSPORT_REASSEMBLY_SET_COUNT; set++)
    {
        for (way = 0U; way < LINKG_TRANSPORT_REASSEMBLY_WAYS; way++)
        {
            entry = &g_transport.reassembly.entries[set][way];

            if (entry->valid && entry->peer_node_id == peer_node_id)
            {
                if (entry->first_packet != NULL)
                {
                    reset_frames++;
                }

                if (entry->tail_packet != NULL)
                {
                    reset_frames++;
                }

                _linkg_transport_reassembly_entry_clear(entry);
            }
        }
    }

    ret = pthread_mutex_unlock(&g_transport.reassembly.lock);
    if (ret != 0)
    {
        return -ret;
    }

    _linkg_transport_reassembly_record_reset(reset_frames);

    return 0;
}

/****************************** 分片重组 ******************************/

/**
 * @brief 批量提交本机目标分片并尝试完成重组。
 *
 * @note 整批只获取一次重组锁并读取一次当前时间。
 */
int linkg_transport_reassembly_submit_batch(linkg_transport_reassembly_submit_item_t *items, uint32_t count)
{
    uint64_t now_us;
    uint32_t index;
    int      ret;

    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (!g_transport.reassembly.initialized)
    {
        return -ENODEV;
    }

    for (index = 0U; index < count; index++)
    {
        items[index].completed_packet = NULL;
        items[index].result           = -EINPROGRESS;
    }

    now_us = linkg_time_elapsed_us();

    ret = pthread_mutex_lock(&g_transport.reassembly.lock);
    if (ret != 0)
    {
        return -ret;
    }

    _linkg_transport_reassembly_gc_locked(now_us);

    for (index = 0U; index < count; index++)
    {
        if (items[index].peer == NULL)
        {
            items[index].result = -EINVAL;
            continue;
        }

        if (!linkg_transport_peer_epoch_matches(items[index].peer, items[index].peer_epoch))
        {
            items[index].result = -ESTALE;
            continue;
        }

        items[index].result = _linkg_transport_reassembly_submit_locked(items[index].traffic_class, items[index].peer_node_id, items[index].peer_epoch, items[index].header, items[index].fragment_header, items[index].packet, now_us, &items[index].completed_packet);
    }

    ret = pthread_mutex_unlock(&g_transport.reassembly.lock);

    return ret == 0 ? 0 : -ret;
}
