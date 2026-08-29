/**
 * @file linkg_transport_tx.c
 * @brief LinkG逻辑传输层发送实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-29
 */

#include "linkg_transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_scheduler.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_TX_BATCH_CHUNK_SIZE 16U // 单次处理最大逻辑数据包数量
#define LINKG_TRANSPORT_TX_FRAME_BATCH_SIZE 32U // 单次Scheduler提交最大Transport帧数量

/****************************** 内部类型 ******************************/

typedef struct
{
    uint32_t sequences[LINKG_TRANSPORT_FRAGMENT_COUNT_MAX];             // 当前逻辑包各物理帧逐跳序列号
    uint32_t frame_payload_lengths[LINKG_TRANSPORT_FRAGMENT_COUNT_MAX]; // 当前逻辑包各Transport帧载荷长度
    int      frame_results[LINKG_TRANSPORT_FRAGMENT_COUNT_MAX];         // 当前逻辑包各Transport帧发送结果
    uint32_t packet_id;                                                 // 分片原始完整数据包编号
    uint32_t original_data_offset;                                      // Transport处理前数据偏移
    uint32_t original_data_length;                                      // Transport处理前数据长度
    uint32_t original_flags;                                            // Transport处理前Packet标志
    uint32_t completed_frames;                                          // 已完成调度的Transport帧数量
    uint32_t successful_frames;                                         // 成功发送的Transport帧数量
    int      result;                                                    // 当前逻辑包最终发送结果
    uint8_t  peer_node_id;                                              // 当前直接Peer节点编号
    uint8_t  frame_count;                                               // 当前逻辑包产生的Transport帧数量
    bool     prepared;                                                  // Peer和发送编号是否准备完成
    bool     fragmented;                                                // 是否进行LinkG内部分片
} linkg_transport_tx_state_t;

typedef struct
{
    linkg_packet_t *owned_packet;  // Transport临时申请的数据包，NULL表示借用原Packet
    uint32_t        logical_index; // 所属逻辑数据包索引
    uint8_t         frame_index;   // 所属逻辑包内部物理帧索引
} linkg_transport_tx_frame_state_t;

typedef struct
{
    linkg_scheduler_tx_item_t        scheduler_items[LINKG_TRANSPORT_TX_FRAME_BATCH_SIZE]; // Scheduler物理帧批次
    linkg_transport_tx_frame_state_t frame_states[LINKG_TRANSPORT_TX_FRAME_BATCH_SIZE];    // 物理帧归属和引用状态
    uint32_t                         count;                                                // 当前物理帧数量
} linkg_transport_tx_frame_batch_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验节点编号。
 */
static bool _linkg_transport_tx_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN && node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 获取STA当前唯一直接Peer。
 *
 * 调用方必须持有g_transport.lock，返回指针不得在解锁后继续使用。
 */
static linkg_transport_peer_t *_linkg_transport_tx_sta_peer_locked(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_PEER_MAX; index++)
    {
        if (g_transport.peers[index].valid)
        {
            return &g_transport.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 分配下一个原始完整数据包编号。
 *
 * 调用方必须持有g_transport.lock。
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
 * @brief 校验Transport批量发送公共参数。
 */
static int _linkg_transport_tx_validate_batch(const linkg_transport_tx_item_t *items, uint32_t count, linkg_transport_type_t type, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    linkg_packet_t *packet;
    uint32_t        required_headroom;
    uint32_t        index;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (!linkg_transport_type_valid(type))
    {
        return -EINVAL;
    }

    if (policy != LINKG_SCHEDULER_POLICY_DEFAULT &&
        policy != LINKG_SCHEDULER_POLICY_REDUNDANT &&
        policy != LINKG_SCHEDULER_POLICY_SPECIFIED)
    {
        return -EINVAL;
    }

    if (policy == LINKG_SCHEDULER_POLICY_SPECIFIED && specified_link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    for (index = 0U; index < count; index++)
    {
        if (!_linkg_transport_tx_node_id_valid(items[index].destination_node_id))
        {
            return -EINVAL;
        }

        if (items[index].destination_node_id == g_transport.local_node_id)
        {
            return -EINVAL;
        }

        packet = items[index].packet;

        if (packet == NULL || packet->pool == NULL || packet->slot == NULL)
        {
            return -EINVAL;
        }

        if (!packet->pool->initialized || packet->data_length == 0U)
        {
            return -EINVAL;
        }

        if (packet->data_offset > packet->pool->slot_size ||
            packet->data_length > packet->pool->slot_size - packet->data_offset)
        {
            return -EMSGSIZE;
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
    }

    return 0;
}

/**
 * @brief 初始化当前逻辑批次发送状态。
 */
static void _linkg_transport_tx_init_states(const linkg_transport_tx_item_t *items, linkg_transport_tx_state_t *states, uint32_t count)
{
    linkg_packet_t *packet;
    uint32_t        frame_index;
    uint32_t        index;

    memset(states, 0, (size_t)count * sizeof(*states));

    for (index = 0U; index < count; index++)
    {
        packet = items[index].packet;

        states[index].original_data_offset = packet->data_offset;
        states[index].original_data_length = packet->data_length;
        states[index].original_flags       = packet->flags;
        states[index].fragmented           = packet->data_length > LINKG_TRANSPORT_PAYLOAD_MAX_SIZE;
        states[index].frame_count          = states[index].fragmented ? LINKG_TRANSPORT_FRAGMENT_COUNT_MAX : 1U;
        states[index].result               = -EINPROGRESS;

        if (states[index].fragmented)
        {
            states[index].frame_payload_lengths[0] = LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
            states[index].frame_payload_lengths[1] = packet->data_length - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
        }
        else
        {
            states[index].frame_payload_lengths[0] = packet->data_length;
        }

        for (frame_index = 0U; frame_index < states[index].frame_count; frame_index++)
        {
            states[index].frame_results[frame_index] = -EINPROGRESS;
        }
    }
}

/**
 * @brief 为逻辑批次确定直接Peer并预留逐跳序列号。
 *
 * 分片逻辑包同时分配一个跨链路共享的packet_id。
 * 整个逻辑批次只获取一次g_transport.lock。
 */
static int _linkg_transport_tx_prepare_batch(const linkg_transport_tx_item_t *items, linkg_transport_tx_state_t *states, uint32_t count)
{
    linkg_transport_peer_t *cached_peer;
    linkg_transport_peer_t *peer;
    uint32_t                frame_index;
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
    peer                       = NULL;
    cached_destination_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    cache_valid                = false;

    if (g_transport.local_role == LINKG_DEVICE_ROLE_STA)
    {
        peer = _linkg_transport_tx_sta_peer_locked();
    }

    for (index = 0U; index < count; index++)
    {
        if (g_transport.local_role == LINKG_DEVICE_ROLE_AP)
        {
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
        }
        else if (g_transport.local_role != LINKG_DEVICE_ROLE_STA)
        {
            peer = NULL;
        }

        if (peer == NULL)
        {
            states[index].result = -ENOENT;
            continue;
        }

        states[index].peer_node_id = peer->peer_node_id;

        if (states[index].fragmented)
        {
            states[index].packet_id = _linkg_transport_tx_next_packet_id_locked();
        }

        for (frame_index = 0U; frame_index < states[index].frame_count; frame_index++)
        {
            peer->tx_sequence++;
            states[index].sequences[frame_index] = peer->tx_sequence;
        }

        states[index].prepared = true;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 恢复调用Transport之前的原始Packet布局。
 */
static void _linkg_transport_tx_restore_packet(const linkg_transport_tx_item_t *items, const linkg_transport_tx_state_t *states, uint32_t logical_index)
{
    linkg_packet_t *packet;

    packet = items[logical_index].packet;

    packet->data_offset = states[logical_index].original_data_offset;
    packet->data_length = states[logical_index].original_data_length;
    packet->flags       = states[logical_index].original_flags;
}

/**
 * @brief 初始化Transport基础头。
 */
static void _linkg_transport_tx_build_header(linkg_transport_header_t *header, const linkg_transport_tx_item_t *item, linkg_transport_type_t type, uint16_t flags, uint32_t sequence)
{
    memset(header, 0, sizeof(*header));

    header->type                = (uint8_t)type;
    header->flags               = flags;
    header->sequence            = sequence;
    header->source_node_id      = g_transport.local_node_id;
    header->destination_node_id = item->destination_node_id;
}

/**
 * @brief 向物理帧批次加入一个Transport帧。
 */
static int _linkg_transport_tx_frame_batch_push(linkg_transport_tx_frame_batch_t *batch, uint8_t peer_node_id, linkg_packet_t *packet, uint32_t logical_index, uint8_t frame_index, linkg_packet_t *owned_packet)
{
    uint32_t index;

    if (batch == NULL || packet == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_transport_tx_node_id_valid(peer_node_id))
    {
        return -EINVAL;
    }

    if (batch->count >= LINKG_TRANSPORT_TX_FRAME_BATCH_SIZE)
    {
        return -ENOSPC;
    }

    index = batch->count;

    batch->scheduler_items[index].packet           = packet;
    batch->scheduler_items[index].next_hop_node_id = peer_node_id;
    batch->scheduler_items[index].result           = -EINPROGRESS;

    batch->frame_states[index].owned_packet = owned_packet;
    batch->frame_states[index].logical_index = logical_index;
    batch->frame_states[index].frame_index = frame_index;

    batch->count++;

    return 0;
}

/**
 * @brief 同步提交当前Transport帧批次并归并逻辑包发送结果。
 */
static void _linkg_transport_tx_flush_frames(linkg_transport_tx_frame_batch_t *batch, const linkg_transport_tx_item_t *items, linkg_transport_tx_state_t *states, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    linkg_transport_tx_state_t *state;
    uint32_t                    logical_index;
    uint32_t                    frame_index;
    uint32_t                    index;
    int                         frame_result;
    int                         ret;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    ret = linkg_scheduler_submit_batch(batch->scheduler_items, batch->count, policy, specified_link_id);

    for (index = 0U; index < batch->count; index++)
    {
        logical_index = batch->frame_states[index].logical_index;
        frame_index   = batch->frame_states[index].frame_index;
        state         = &states[logical_index];

        frame_result = ret < 0 ? ret : batch->scheduler_items[index].result;

        if (frame_result == -EINPROGRESS)
        {
            frame_result = -EIO;
        }

        state->frame_results[frame_index] = frame_result;
        state->completed_frames++;

        if (frame_result == 0)
        {
            state->successful_frames++;
        }
        else if (state->result == -EINPROGRESS)
        {
            state->result = frame_result;
        }

        if (state->completed_frames == state->frame_count)
        {
            if (state->successful_frames == state->frame_count)
            {
                state->result = 0;
            }
            else
            {
                if (state->result == -EINPROGRESS)
                {
                    state->result = -EIO;
                }

                _linkg_transport_tx_restore_packet(items, states, logical_index);
            }
        }

        if (batch->frame_states[index].owned_packet != NULL)
        {
            linkg_packet_release(batch->frame_states[index].owned_packet);
        }
    }

    memset(batch, 0, sizeof(*batch));
}

/**
 * @brief 编码并加入一个不需要分片的逻辑数据包。
 */
static int _linkg_transport_tx_queue_normal(const linkg_transport_tx_item_t *items, linkg_transport_tx_state_t *states, uint32_t logical_index, linkg_transport_type_t type, linkg_transport_tx_frame_batch_t *batch)
{
    linkg_transport_header_t header;
    linkg_packet_t          *packet;
    int                      ret;

    packet = items[logical_index].packet;

    _linkg_transport_tx_build_header(&header, &items[logical_index], type, LINKG_TRANSPORT_FLAG_NONE, states[logical_index].sequences[0]);

    ret = linkg_transport_wire_encode(packet, &header);
    if (ret != 0)
    {
        states[logical_index].result = ret;
        _linkg_transport_tx_restore_packet(items, states, logical_index);
        return ret;
    }

    ret = _linkg_transport_tx_frame_batch_push(batch, states[logical_index].peer_node_id, packet, logical_index, 0U, NULL);
    if (ret != 0)
    {
        states[logical_index].result = ret;
        _linkg_transport_tx_restore_packet(items, states, logical_index);
        return ret;
    }

    return 0;
}

/**
 * @brief 编码并加入一个需要LinkG内部分片的逻辑数据包。
 *
 * 第一片复用调用者原Packet，尾片申请额外Packet并仅复制尾部载荷。
 */
static int _linkg_transport_tx_queue_fragmented(const linkg_transport_tx_item_t *items, linkg_transport_tx_state_t *states, uint32_t logical_index, linkg_transport_type_t type, linkg_transport_tx_frame_batch_t *batch)
{
    linkg_transport_fragment_header_t fragment_header;
    linkg_transport_header_t          header;
    const uint8_t                    *original_data;
    linkg_packet_t                   *tail_packet;
    linkg_packet_t                   *packet;
    uint32_t                          original_length;
    uint32_t                          tail_length;
    int                               ret;

    packet          = items[logical_index].packet;
    original_data   = linkg_packet_const_data(packet);
    original_length = states[logical_index].original_data_length;
    tail_length     = original_length - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;

    tail_packet = linkg_packet_pool_alloc(packet->pool);
    if (tail_packet == NULL)
    {
        states[logical_index].result = -ENOMEM;
        return -ENOMEM;
    }

    if (linkg_packet_headroom(tail_packet) < LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE ||
        linkg_packet_capacity(tail_packet) < tail_length)
    {
        linkg_packet_release(tail_packet);
        states[logical_index].result = -ENOBUFS;
        return -ENOBUFS;
    }

    memcpy(linkg_packet_data(tail_packet), original_data + LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE, tail_length);

    tail_packet->data_length = tail_length;
    tail_packet->flags       = (packet->flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) | LINKG_PACKET_FLAG_TX_GROUP_LAST;

    memset(&fragment_header, 0, sizeof(fragment_header));

    fragment_header.packet_id       = states[logical_index].packet_id;
    fragment_header.fragment_offset = (uint16_t)LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
    fragment_header.packet_length   = (uint16_t)original_length;

    ret = linkg_transport_wire_fragment_encode(tail_packet, &fragment_header);
    if (ret != 0)
    {
        linkg_packet_release(tail_packet);
        states[logical_index].result = ret;
        return ret;
    }

    _linkg_transport_tx_build_header(&header, &items[logical_index], type, LINKG_TRANSPORT_FLAG_FRAGMENT, states[logical_index].sequences[1]);

    ret = linkg_transport_wire_encode(tail_packet, &header);
    if (ret != 0)
    {
        linkg_packet_release(tail_packet);
        states[logical_index].result = ret;
        return ret;
    }

    packet->data_length = LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
    packet->flags       = (packet->flags & ~LINKG_PACKET_FLAG_TX_GROUP_MASK) | LINKG_PACKET_FLAG_TX_GROUP_FIRST;

    memset(&fragment_header, 0, sizeof(fragment_header));

    fragment_header.packet_id       = states[logical_index].packet_id;
    fragment_header.fragment_offset = 0U;
    fragment_header.packet_length   = (uint16_t)original_length;

    ret = linkg_transport_wire_fragment_encode(packet, &fragment_header);
    if (ret != 0)
    {
        linkg_packet_release(tail_packet);
        states[logical_index].result = ret;
        _linkg_transport_tx_restore_packet(items, states, logical_index);
        return ret;
    }

    _linkg_transport_tx_build_header(&header, &items[logical_index], type, LINKG_TRANSPORT_FLAG_FRAGMENT, states[logical_index].sequences[0]);

    ret = linkg_transport_wire_encode(packet, &header);
    if (ret != 0)
    {
        linkg_packet_release(tail_packet);
        states[logical_index].result = ret;
        _linkg_transport_tx_restore_packet(items, states, logical_index);
        return ret;
    }

    ret = _linkg_transport_tx_frame_batch_push(batch, states[logical_index].peer_node_id, packet, logical_index, 0U, NULL);
    if (ret != 0)
    {
        linkg_packet_release(tail_packet);
        states[logical_index].result = ret;
        _linkg_transport_tx_restore_packet(items, states, logical_index);
        return ret;
    }

    ret = _linkg_transport_tx_frame_batch_push(batch, states[logical_index].peer_node_id, tail_packet, logical_index, 1U, tail_packet);
    if (ret != 0)
    {
        batch->count--;

        memset(&batch->scheduler_items[batch->count], 0, sizeof(batch->scheduler_items[batch->count]));
        memset(&batch->frame_states[batch->count], 0, sizeof(batch->frame_states[batch->count]));

        linkg_packet_release(tail_packet);

        states[logical_index].result = ret;

        _linkg_transport_tx_restore_packet(items, states, logical_index);

        return ret;
    }

    return 0;
}

/**
 * @brief 批量记录Transport帧发送统计。
 *
 * 每个Transport Frame独立记录成功、失败和实际载荷字节数。
 */
static void _linkg_transport_tx_record_batch(const linkg_transport_tx_state_t *states, uint32_t count, linkg_transport_type_t type)
{
    linkg_transport_type_stats_t *type_stats;
    linkg_transport_peer_t       *cached_peer;
    linkg_transport_peer_t       *peer;
    uint64_t                      now_ms;
    uint32_t                      frame_index;
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

        type_stats = &peer->stats.types[type];

        for (frame_index = 0U; frame_index < states[index].frame_count; frame_index++)
        {
            if (states[index].frame_results[frame_index] == 0)
            {
                type_stats->tx_packets++;
                type_stats->tx_bytes += states[index].frame_payload_lengths[frame_index];

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
                type_stats->tx_failed_bytes += states[index].frame_payload_lengths[frame_index];
            }
        }
    }

    pthread_mutex_unlock(&g_transport.lock);
}

/**
 * @brief 发送一个内部逻辑批次。
 */
static int _linkg_transport_send_chunk(const linkg_transport_tx_item_t *items, uint32_t count, linkg_transport_type_t type, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    linkg_transport_tx_frame_batch_t frame_batch;
    linkg_transport_tx_state_t       states[LINKG_TRANSPORT_TX_BATCH_CHUNK_SIZE];
    uint32_t                         required_frames;
    uint32_t                         index;
    int                              first_error;
    int                              ret;
	uint32_t 						 frame_index;

    memset(&frame_batch, 0, sizeof(frame_batch));

    _linkg_transport_tx_init_states(items, states, count);

    ret = _linkg_transport_tx_prepare_batch(items, states, count);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (!states[index].prepared)
        {
            continue;
        }

        required_frames = states[index].frame_count;

        if (frame_batch.count + required_frames > LINKG_TRANSPORT_TX_FRAME_BATCH_SIZE)
        {
            _linkg_transport_tx_flush_frames(&frame_batch, items, states, policy, specified_link_id);
        }

        if (states[index].fragmented)
        {
            ret = _linkg_transport_tx_queue_fragmented(items, states, index, type, &frame_batch);
        }
        else
        {
            ret = _linkg_transport_tx_queue_normal(items, states, index, type, &frame_batch);
        }

        if (ret != 0)
        {
            continue;
        }

        if (frame_batch.count == LINKG_TRANSPORT_TX_FRAME_BATCH_SIZE)
        {
            _linkg_transport_tx_flush_frames(&frame_batch, items, states, policy, specified_link_id);
        }
    }

    _linkg_transport_tx_flush_frames(&frame_batch, items, states, policy, specified_link_id);

    first_error = 0;

    for (index = 0U; index < count; index++)
    {
        if (states[index].prepared && states[index].result == -EINPROGRESS)
        {
            states[index].result = -EIO;

            _linkg_transport_tx_restore_packet(items, states, index);
        }

        if (states[index].prepared)
        {
            for (frame_index = 0U; frame_index < states[index].frame_count; frame_index++)
            {
                if (states[index].frame_results[frame_index] == -EINPROGRESS)
                {
                    states[index].frame_results[frame_index] = states[index].result != 0 ?
                                                               states[index].result :
                                                               -EIO;
                }
            }
        }

        if (states[index].result != 0 && first_error == 0)
        {
            first_error = states[index].result;
        }
    }

    _linkg_transport_tx_record_batch(states, count, type);

    return first_error;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 批量向最终逻辑目标节点发送Transport数据包。
 *
 * Transport不接管调用者持有的原始Packet引用。
 * 超过1436字节的逻辑包统一切为两片，物理Transport帧最多32个一批提交Scheduler。
 */
int linkg_transport_send_batch(const linkg_transport_tx_item_t *items, uint32_t count, linkg_transport_type_t type, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    uint32_t chunk_count;
    uint32_t offset;
    int      first_error;
    int      ret;

    ret = _linkg_transport_tx_validate_batch(items, count, type, policy, specified_link_id);
    if (ret != 0)
    {
        return ret;
    }

    first_error = 0;
    offset      = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_TRANSPORT_TX_BATCH_CHUNK_SIZE)
        {
            chunk_count = LINKG_TRANSPORT_TX_BATCH_CHUNK_SIZE;
        }

        ret = _linkg_transport_send_chunk(&items[offset], chunk_count, type, policy, specified_link_id);

        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }

        offset += chunk_count;
    }

    return first_error;
}

/**
 * @brief 向最终逻辑目标节点发送Transport数据包。
 *
 * Transport不接管调用者持有的原始Packet引用。
 */
int linkg_transport_send(uint8_t destination_node_id, linkg_transport_type_t type, linkg_packet_t *packet, linkg_scheduler_policy_t policy, uint32_t specified_link_id)
{
    linkg_transport_tx_item_t item;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_transport_tx_node_id_valid(destination_node_id) || packet == NULL)
    {
        return -EINVAL;
    }

    if (destination_node_id == g_transport.local_node_id)
    {
        return -EINVAL;
    }

    item.packet              = packet;
    item.destination_node_id = destination_node_id;

    return linkg_transport_send_batch(&item, 1U, type, policy, specified_link_id);
}
