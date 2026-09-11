/**
 * @file transport_tx.c
 * @brief LinkG逻辑传输层发送实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#include "transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_TX_BATCH_MAX        32U
#define LINKG_TRANSPORT_TX_WIRE_BATCH_MAX  (LINKG_TRANSPORT_TX_BATCH_MAX * LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)

/****************************** 内部类型 ******************************/

/**
 * @brief 单个逻辑Packet的Transport发送状态。
 */
typedef struct
{
    linkg_packet_t *tail_packet;          // 分片尾包，Transport持有基础引用
    uint32_t        original_data_offset; // Transport编码前数据偏移
    uint32_t        original_data_length; // Transport编码前数据长度
    uint32_t        original_flags;       // Transport编码前Packet标志
    uint32_t        packet_id;            // 分片原始Packet编号
    uint32_t        frame_offset;         // 当前逻辑Packet在Wire批次中的起始位置
    uint8_t         frame_count;          // 当前逻辑Packet生成Wire Frame数量
} linkg_transport_tx_state_t;

/**
 * @brief Transport编码后的Wire Frame批次。
 */
typedef struct
{
    linkg_packet_t *packets[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];         // 已编码Transport Wire Frame
    uint32_t        payload_lengths[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX]; // 各Wire Frame实际Transport载荷长度
    uint32_t        count;                                              // 当前Wire Frame数量
} linkg_transport_tx_frame_batch_t;

/****************************** 参数校验 ******************************/

/**
 * @brief 校验Transport节点编号。
 */
static bool _linkg_transport_tx_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN && node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 校验Transport发送上下文。
 */
static int _linkg_transport_tx_validate_context(const linkg_transport_tx_context_t *context)
{
    uint32_t index;

    if (context == NULL || context->targets == NULL)
    {
        return -EINVAL;
    }

    if (context->target_count == 0U || context->target_count > LINKG_TRANSPORT_TX_TARGET_MAX)
    {
        return -EINVAL;
    }

    if (!linkg_transport_class_valid(context->traffic_class))
    {
        return -EINVAL;
    }

    if (!_linkg_transport_tx_node_id_valid(context->peer_node_id))
    {
        return -EINVAL;
    }

    for (index = 0U; index < context->target_count; index++)
    {
        if (context->targets[index].link == NULL || context->targets[index].path == NULL)
        {
            return -EINVAL;
        }
    }

    return 0;
}

/**
 * @brief 校验Transport编码所需Packet。
 */
static int _linkg_transport_tx_validate_packet(const linkg_packet_t *packet)
{
    uint32_t required_headroom;

    if (packet == NULL || packet->pool == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length == 0U)
    {
        return -EINVAL;
    }

    if (packet->data_length > LINKG_TRANSPORT_PACKET_MAX_SIZE)
    {
        return -EMSGSIZE;
    }

    required_headroom = packet->data_length > LINKG_TRANSPORT_PAYLOAD_MAX_SIZE ?
                        LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE :
                        LINKG_TRANSPORT_WIRE_HEADER_SIZE;

    if (linkg_packet_headroom(packet) < required_headroom)
    {
        return -ENOSPC;
    }

    return 0;
}

/**
 * @brief 校验Transport正常发送批次。
 */
static int _linkg_transport_tx_validate_batch(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *items, uint32_t count)
{
    uint32_t index;
    int      ret;

    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_transport_tx_validate_context(context);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (!linkg_transport_type_valid(items[index].type))
        {
            return -EINVAL;
        }

        if (!_linkg_transport_tx_node_id_valid(items[index].destination_node_id))
        {
            return -EINVAL;
        }

        ret = _linkg_transport_tx_validate_packet(items[index].packet);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 分片准备 ******************************/

/**
 * @brief 初始化逻辑Packet状态并准备全部分片尾包。
 *
 * @note 本阶段不修改调用方原始Packet。
 */
static int _linkg_transport_tx_prepare_states(const linkg_transport_tx_item_t *items, linkg_transport_tx_state_t *states, uint32_t count)
{
    const uint8_t  *data;
    linkg_packet_t *packet;
    linkg_packet_t *tail_packet;
    uint32_t        frame_offset;
    uint32_t        tail_length;
    uint32_t        index;

    memset(states, 0, sizeof(*states) * count);

    frame_offset = 0U;

    for (index = 0U; index < count; index++)
    {
        packet = items[index].packet;

        states[index].original_data_offset = packet->data_offset;
        states[index].original_data_length = packet->data_length;
        states[index].original_flags       = packet->flags;
        states[index].frame_offset         = frame_offset;
        states[index].frame_count          = packet->data_length > LINKG_TRANSPORT_PAYLOAD_MAX_SIZE ?
                                             LINKG_TRANSPORT_FRAGMENT_COUNT_MAX :
                                             1U;

        frame_offset += states[index].frame_count;

        if (states[index].frame_count == 1U)
        {
            continue;
        }

        tail_packet = linkg_packet_pool_alloc(packet->pool);
        if (tail_packet == NULL)
        {
            return -ENOMEM;
        }

        tail_length = packet->data_length - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;

        if (linkg_packet_headroom(tail_packet) < LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE || linkg_packet_capacity(tail_packet) < tail_length)
        {
            linkg_packet_release(tail_packet);
            return -ENOBUFS;
        }

        data = linkg_packet_const_data(packet);

        memcpy(linkg_packet_data(tail_packet), data + LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE, tail_length);

        tail_packet->data_length = tail_length;
        tail_packet->flags       = packet->flags;

        states[index].tail_packet = tail_packet;
    }

    return 0;
}

/**
 * @brief 释放Transport临时申请的分片尾包。
 */
static void _linkg_transport_tx_cleanup_states(linkg_transport_tx_state_t *states, uint32_t count)
{
    uint32_t index;

    for (index = 0U; index < count; index++)
    {
        if (states[index].tail_packet != NULL)
        {
            linkg_packet_release(states[index].tail_packet);
            states[index].tail_packet = NULL;
        }
    }
}

/**
 * @brief 恢复调用Transport之前的原始Packet布局。
 */
static void _linkg_transport_tx_restore_packets(const linkg_transport_tx_item_t *items, const linkg_transport_tx_state_t *states, uint32_t count)
{
    linkg_packet_t *packet;
    uint32_t        index;

    for (index = 0U; index < count; index++)
    {
        packet = items[index].packet;

        packet->data_offset = states[index].original_data_offset;
        packet->data_length = states[index].original_data_length;
        packet->flags       = states[index].original_flags;
    }
}

/****************************** 协议状态 ******************************/

/**
 * @brief 分配下一个Transport分片Packet编号。
 *
 * @note 调用方必须持有g_transport.lock。
 */
static uint32_t _linkg_transport_tx_next_packet_id_locked(void)
{
    uint32_t packet_id;

    packet_id = g_transport.next_packet_id;

    if (packet_id == LINKG_TRANSPORT_PACKET_ID_INVALID)
    {
        packet_id = 1U;
    }

    g_transport.next_packet_id = packet_id + 1U;

    if (g_transport.next_packet_id == LINKG_TRANSPORT_PACKET_ID_INVALID)
    {
        g_transport.next_packet_id = 1U;
    }

    return packet_id;
}

/**
 * @brief 获取当前Peer/Class发送顺序锁并准备分片Packet编号。
 *
 * @note 返回成功后仅持有peer_class->tx_order_lock，
 *       g_transport.lock已经释放。
 */
static int _linkg_transport_tx_acquire_state(const linkg_transport_tx_context_t *context, linkg_transport_tx_state_t *states, uint32_t count, linkg_transport_peer_class_t **peer_class)
{
    linkg_transport_peer_class_t *current;
    linkg_transport_peer_t       *peer;
    uint32_t                      index;
    int                           ret;

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (!g_transport.initialized)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENODEV;
    }

    peer = linkg_transport_find_peer_locked(context->peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    if (!linkg_transport_peer_epoch_read_active(peer, NULL))
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -EAGAIN;
    }

    current = &peer->classes[context->traffic_class];

    ret = pthread_mutex_lock(&current->tx_order_lock);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (states[index].frame_count == LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)
        {
            states[index].packet_id = _linkg_transport_tx_next_packet_id_locked();
        }
    }

    pthread_mutex_unlock(&g_transport.lock);

    *peer_class = current;

    return 0;
}

/****************************** Wire编码 ******************************/

/**
 * @brief 构造Transport基础头。
 */
static void _linkg_transport_tx_build_header(linkg_transport_header_t *header, const linkg_transport_tx_item_t *item, uint16_t flags, uint32_t sequence)
{
    memset(header, 0, sizeof(*header));

    header->type                = (uint8_t)item->type;
    header->flags               = flags;
    header->sequence            = sequence;
    header->source_node_id      = g_transport.local_node_id;
    header->destination_node_id = item->destination_node_id;
}

/**
 * @brief 编码单帧Transport逻辑Packet。
 */
static int _linkg_transport_tx_encode_normal(const linkg_transport_tx_item_t *item, const linkg_transport_tx_state_t *state, linkg_transport_peer_class_t *peer_class, linkg_transport_tx_frame_batch_t *batch)
{
    linkg_transport_header_t header;
    linkg_packet_t          *packet;
    uint32_t                 sequence;
    int                      ret;

    packet = item->packet;

    peer_class->tx_sequence++;
    sequence = peer_class->tx_sequence;

    packet->flags &= ~LINKG_PACKET_FLAG_TX_GROUP_MASK;

    _linkg_transport_tx_build_header(&header, item, LINKG_TRANSPORT_FLAG_NONE, sequence);

    ret = linkg_transport_wire_encode(packet, &header);
    if (ret != 0)
    {
        return ret;
    }

    batch->packets[batch->count]         = packet;
    batch->payload_lengths[batch->count] = state->original_data_length;
    batch->count++;

    return 0;
}

/**
 * @brief 编码固定两片Transport逻辑Packet。
 */
static int _linkg_transport_tx_encode_fragmented(const linkg_transport_tx_item_t *item, const linkg_transport_tx_state_t *state, linkg_transport_peer_class_t *peer_class, linkg_transport_tx_frame_batch_t *batch)
{
    linkg_transport_fragment_header_t fragment_header;
    linkg_transport_header_t          header;
    linkg_packet_t                   *first_packet;
    linkg_packet_t                   *tail_packet;
    uint32_t                          first_sequence;
    uint32_t                          tail_sequence;
    uint32_t                          tail_length;
    int                               ret;

    first_packet = item->packet;
    tail_packet  = state->tail_packet;
    tail_length  = state->original_data_length - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;

    peer_class->tx_sequence++;
    first_sequence = peer_class->tx_sequence;

    peer_class->tx_sequence++;
    tail_sequence = peer_class->tx_sequence;

    first_packet->data_length = LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
    first_packet->flags       = (state->original_flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) | LINKG_PACKET_FLAG_TX_GROUP_FIRST;

    memset(&fragment_header, 0, sizeof(fragment_header));

    fragment_header.packet_id       = state->packet_id;
    fragment_header.fragment_offset = 0U;
    fragment_header.packet_length   = (uint16_t)state->original_data_length;

    ret = linkg_transport_wire_fragment_encode(first_packet, &fragment_header);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_transport_tx_build_header(&header, item, LINKG_TRANSPORT_FLAG_FRAGMENT, first_sequence);

    ret = linkg_transport_wire_encode(first_packet, &header);
    if (ret != 0)
    {
        return ret;
    }

    tail_packet->flags = (state->original_flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) | LINKG_PACKET_FLAG_TX_GROUP_LAST;

    memset(&fragment_header, 0, sizeof(fragment_header));

    fragment_header.packet_id       = state->packet_id;
    fragment_header.fragment_offset = (uint16_t)LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
    fragment_header.packet_length   = (uint16_t)state->original_data_length;

    ret = linkg_transport_wire_fragment_encode(tail_packet, &fragment_header);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_transport_tx_build_header(&header, item, LINKG_TRANSPORT_FLAG_FRAGMENT, tail_sequence);

    ret = linkg_transport_wire_encode(tail_packet, &header);
    if (ret != 0)
    {
        return ret;
    }

    batch->packets[batch->count]         = first_packet;
    batch->payload_lengths[batch->count] = LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
    batch->count++;

    batch->packets[batch->count]         = tail_packet;
    batch->payload_lengths[batch->count] = tail_length;
    batch->count++;

    return 0;
}

/**
 * @brief 按逻辑Packet顺序编码整个Transport批次。
 *
 * @note 分片Packet的FIRST和LAST始终连续写入Wire批次。
 */
static int _linkg_transport_tx_encode_batch(const linkg_transport_tx_item_t *items, const linkg_transport_tx_state_t *states, uint32_t count, linkg_transport_peer_class_t *peer_class, linkg_transport_tx_frame_batch_t *batch)
{
    uint32_t index;
    int      ret;

    memset(batch, 0, sizeof(*batch));

    for (index = 0U; index < count; index++)
    {
        if (states[index].frame_count == LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)
        {
            ret = _linkg_transport_tx_encode_fragmented(&items[index], &states[index], peer_class, batch);
        }
        else
        {
            ret = _linkg_transport_tx_encode_normal(&items[index], &states[index], peer_class, batch);
        }

        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** Link提交 ******************************/

/**
 * @brief 将Transport业务类别转换为Link业务类别。
 */
static linkg_link_tx_class_t _linkg_transport_tx_link_class(linkg_transport_class_t traffic_class)
{
    switch (traffic_class)
    {
        case LINKG_TRANSPORT_CLASS_REALTIME:
            return LINKG_LINK_TX_CLASS_REALTIME;

        case LINKG_TRANSPORT_CLASS_VIDEO:
            return LINKG_LINK_TX_CLASS_VIDEO;

        case LINKG_TRANSPORT_CLASS_DATA:
        default:
            return LINKG_LINK_TX_CLASS_DATA;
    }
}

/**
 * @brief 将同一Wire Frame批次提交全部Scheduler Target。
 *
 * @note Transport允许一次生成最多64个Wire Frame，
 *       具体Link负责按照自身tx_batch_size继续拆分物理批次。
 *
 * @note 单个Wire Frame只要至少被一个Target接管即视为成功。
 */
static int _linkg_transport_tx_submit_wire_batch(const linkg_transport_tx_context_t *context, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    int                   target_results[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];
    linkg_link_tx_class_t tx_class;
    uint32_t              accepted_count;
    uint32_t              target_index;
    uint32_t              index;
    int                   ret;

    tx_class = _linkg_transport_tx_link_class(context->traffic_class);

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;
    }

    for (target_index = 0U; target_index < context->target_count; target_index++)
    {
        for (index = 0U; index < count; index++)
        {
            target_results[index] = -EINPROGRESS;
        }

        ret = linkg_link_submit_batch(context->targets[target_index].link, context->targets[target_index].path, tx_class, &context->targets[target_index].destination, packets, count, target_results);

        if (ret < 0)
        {
            for (index = 0U; index < count; index++)
            {
                target_results[index] = ret;
            }
        }

        for (index = 0U; index < count; index++)
        {
            if (target_results[index] == 0)
            {
                results[index] = 0;
            }
            else if (results[index] == -EINPROGRESS)
            {
                results[index] = target_results[index];
            }
        }
    }

    accepted_count = 0U;

    for (index = 0U; index < count; index++)
    {
        if (results[index] == -EINPROGRESS)
        {
            results[index] = -EIO;
        }

        if (results[index] == 0)
        {
            accepted_count++;
        }
    }

    return (int)accepted_count;
}

/****************************** 结果处理 ******************************/

/**
 * @brief 将Wire Frame结果汇总为原始逻辑Packet结果。
 */
static uint32_t _linkg_transport_tx_finalize_results(const linkg_transport_tx_item_t *items, const linkg_transport_tx_state_t *states, uint32_t count, const int *frame_results, int *results)
{
    uint32_t success_count;
    uint32_t frame_index;
    uint32_t index;
    int      result;

    success_count = 0U;

    for (index = 0U; index < count; index++)
    {
        result = 0;

        for (frame_index = 0U; frame_index < states[index].frame_count; frame_index++)
        {
            if (frame_results[states[index].frame_offset + frame_index] != 0)
            {
                result = frame_results[states[index].frame_offset + frame_index];
                break;
            }
        }

        results[index] = result;

        if (result == 0)
        {
            success_count++;
            continue;
        }

        if (frame_results[states[index].frame_offset] != 0)
        {
            items[index].packet->data_offset = states[index].original_data_offset;
            items[index].packet->data_length = states[index].original_data_length;
            items[index].packet->flags       = states[index].original_flags;
        }
    }

    return success_count;
}

/**
 * @brief 记录当前Peer/Class Transport发送统计。
 */
static void _linkg_transport_tx_record_stats(linkg_transport_peer_class_t *peer_class, const linkg_transport_tx_frame_batch_t *batch, const int *frame_results)
{
    uint64_t now_ms;
    uint32_t index;
    bool     accepted;

    accepted = false;

    for (index = 0U; index < batch->count; index++)
    {
        if (frame_results[index] == 0)
        {
            peer_class->stats.tx_packets++;
            peer_class->stats.tx_bytes += batch->payload_lengths[index];
            accepted = true;
        }
        else
        {
            peer_class->stats.tx_failed_packets++;
            peer_class->stats.tx_failed_bytes += batch->payload_lengths[index];
        }
    }

    if (accepted)
    {
        now_ms = linkg_time_elapsed_ms();
        peer_class->stats.last_tx_ms = now_ms;
    }
}

/**
 * @brief 获取当前Peer/Class中继发送状态并锁定发送顺序。
 *
 * @note 返回成功后仅持有peer_class->tx_order_lock，
 *       g_transport.lock已经释放。
 */
static int _linkg_transport_tx_acquire_forward_state(const linkg_transport_tx_context_t *context, linkg_transport_peer_class_t **peer_class)
{
    linkg_transport_peer_class_t *current;
    linkg_transport_peer_t       *peer;
    int                           ret;

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (!g_transport.initialized)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENODEV;
    }

    peer = linkg_transport_find_peer_locked(context->peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    if (!linkg_transport_peer_epoch_read_active(peer, NULL))
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -EAGAIN;
    }

    current = &peer->classes[context->traffic_class];

    ret = pthread_mutex_lock(&current->tx_order_lock);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ret;
    }

    pthread_mutex_unlock(&g_transport.lock);

    *peer_class = current;

    return 0;
}

/**
 * @brief 记录当前Peer/Class中继发送统计。
 */
static void _linkg_transport_tx_record_forward_stats(linkg_transport_peer_class_t *peer_class, const linkg_transport_forward_item_t *items, uint32_t count, const int *results)
{
    uint64_t now_ms;
    uint32_t index;
    bool     accepted;

    accepted = false;

    for (index = 0U; index < count; index++)
    {
        if (results[index] == 0)
        {
            peer_class->stats.tx_packets++;
            peer_class->stats.tx_bytes += items[index].payload_length;
            accepted = true;
        }
        else
        {
            peer_class->stats.tx_failed_packets++;
            peer_class->stats.tx_failed_bytes += items[index].payload_length;
        }
    }

    if (accepted)
    {
        now_ms = linkg_time_elapsed_ms();
        peer_class->stats.last_tx_ms = now_ms;
    }
}

/****************************** Chunk发送 ******************************/

/**
 * @brief 发送一个最多32个逻辑Packet的Transport内部Chunk。
 *
 * @note 当前Chunk最多展开为64个Transport Wire Frame，
 *       Link层负责继续按照自身tx_batch_size拆分物理发送批次。
 */
static int _linkg_transport_tx_send_chunk(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *items, uint32_t count, int *results)
{
    int                              frame_results[LINKG_TRANSPORT_TX_WIRE_BATCH_MAX];
    linkg_transport_tx_frame_batch_t frame_batch;
    linkg_transport_tx_state_t       states[LINKG_TRANSPORT_TX_BATCH_MAX];
    linkg_transport_peer_class_t    *peer_class;
    uint32_t                         sequence_start;
    uint32_t                         success_count;
    int                              accepted_frames;
    int                              ret;

    peer_class = NULL;

    if (count == 0U)
    {
        return -EINVAL;
    }

    if (count > LINKG_TRANSPORT_TX_BATCH_MAX)
    {
        return -EOVERFLOW;
    }

    ret = _linkg_transport_tx_prepare_states(items, states, count);
    if (ret != 0)
    {
        _linkg_transport_tx_cleanup_states(states, count);
        return ret;
    }

    ret = _linkg_transport_tx_acquire_state(context, states, count, &peer_class);
    if (ret != 0)
    {
        _linkg_transport_tx_cleanup_states(states, count);
        return ret;
    }

    sequence_start = peer_class->tx_sequence;

    ret = _linkg_transport_tx_encode_batch(items, states, count, peer_class, &frame_batch);
    if (ret != 0)
    {
        peer_class->tx_sequence = sequence_start;

        _linkg_transport_tx_restore_packets(items, states, count);
        pthread_mutex_unlock(&peer_class->tx_order_lock);
        _linkg_transport_tx_cleanup_states(states, count);

        return ret;
    }

    accepted_frames = _linkg_transport_tx_submit_wire_batch(context, frame_batch.packets, frame_batch.count, frame_results);

    /**
     * 当前Chunk没有任何Wire Frame被任一Target接管时，
     * 对端不可能观察到本Chunk分配的sequence，可以安全回退。
     */
    if (accepted_frames == 0)
    {
        peer_class->tx_sequence = sequence_start;
    }

    _linkg_transport_tx_record_stats(peer_class, &frame_batch, frame_results);

    success_count = _linkg_transport_tx_finalize_results(items, states, count, frame_results, results);

    pthread_mutex_unlock(&peer_class->tx_order_lock);
    _linkg_transport_tx_cleanup_states(states, count);

    return (int)success_count;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 批量发送Scheduler已经完成调度的Transport逻辑Packet。
 *
 * @note 调用方可以提交任意数量逻辑Packet，Transport固定按最多32个逻辑Packet拆分内部Chunk。
 *       每个Chunk最多展开为64个Wire Frame，不按分片结果额外遍历或动态切分Chunk。
 *
 * @return 小于0表示全部Chunk均未正常执行；
 *         大于等于0表示成功完整提交的原始逻辑Packet总数量。
 */
int linkg_transport_send_batch(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *items, uint32_t count, int *results)
{
    uint32_t success_count;
    uint32_t chunk_count;
    uint32_t offset;
    uint32_t index;
    int      first_error;
    int      ret;
    bool     processed;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (context == NULL || items == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_transport_tx_validate_batch(context, items, count);
    if (ret != 0)
    {
        return ret;
    }

    success_count = 0U;
    first_error   = 0;
    processed     = false;
    offset        = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_TRANSPORT_TX_BATCH_MAX)
        {
            chunk_count = LINKG_TRANSPORT_TX_BATCH_MAX;
        }

        ret = _linkg_transport_tx_send_chunk(context, &items[offset], chunk_count, &results[offset]);
        if (ret < 0)
        {
            for (index = 0U; index < chunk_count; index++)
            {
                results[offset + index] = ret;
            }

            if (first_error == 0)
            {
                first_error = ret;
            }
        }
        else
        {
            processed = true;
            success_count += (uint32_t)ret;
        }

        offset += chunk_count;
    }

    if (!processed)
    {
        return first_error;
    }

    return (int)success_count;
}

/**
 * @brief 发送单个Scheduler已经完成调度的Transport逻辑Packet。
 */
int linkg_transport_send(const linkg_transport_tx_context_t *context, const linkg_transport_tx_item_t *item)
{
    int results[1];
    int ret;

    if (item == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_transport_send_batch(context, item, 1U, results);
    if (ret < 0)
    {
        return ret;
    }

    return results[0];
}

/****************************** 中继发送 ******************************/

/**
 * @brief 批量转发Scheduler已经完成下一跳调度的Transport Wire Frame。
 *
 * @note items中的Packet必须保持完整Transport Wire格式。
 *       本函数不重新编码、不重新分片，也不修改packet_id、source_node_id和destination_node_id，
 *       仅按照当前下一跳Peer/Class重新分配逐跳sequence。
 *
 * @note 一个调用只对应一个直接Peer、一个业务Class和一个发送计划。
 *       当前中继批次最大32帧，分片FIRST和LAST必须保持连续。
 *
 * @return 小于0表示整个batch未进入正常Link提交流程；
 *         大于等于0表示至少被一个Target接管的Wire Frame数量。
 */
int linkg_transport_forward_batch(const linkg_transport_tx_context_t *context, const linkg_transport_forward_item_t *items, uint32_t count, int *results)
{
    linkg_packet_t               *packets[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    linkg_transport_peer_class_t *peer_class;
    uint32_t                      sequence_start;
    uint32_t                      index;
    int                           accepted_count;
    int                           ret;

    peer_class = NULL;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (context == NULL || items == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        return -EOVERFLOW;
    }

    ret = _linkg_transport_tx_validate_context(context);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (items[index].packet == NULL || items[index].payload_length == 0U)
        {
            return -EINVAL;
        }
    }

    ret = _linkg_transport_tx_acquire_forward_state(context, &peer_class);
    if (ret != 0)
    {
        return ret;
    }

    sequence_start = peer_class->tx_sequence;

    for (index = 0U; index < count; index++)
    {
        packets[index] = items[index].packet;

        peer_class->tx_sequence++;

        ret = linkg_transport_wire_update_sequence(packets[index], peer_class->tx_sequence);
        if (ret != 0)
        {
            peer_class->tx_sequence = sequence_start;
            pthread_mutex_unlock(&peer_class->tx_order_lock);
            return ret;
        }
    }

    accepted_count = _linkg_transport_tx_submit_wire_batch(context, packets, count, results);

    if (accepted_count == 0)
    {
        peer_class->tx_sequence = sequence_start;
    }

    _linkg_transport_tx_record_forward_stats(peer_class, items, count, results);

    pthread_mutex_unlock(&peer_class->tx_order_lock);

    return accepted_count;
}
