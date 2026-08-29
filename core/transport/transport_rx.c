/**
 * @file transport_rx.c
 * @brief LinkG逻辑传输层批量接收实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-08-29
 */

#include "transport_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_packet_pool.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE      32U        // 单次Transport内部处理最大物理帧数量
#define LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX    16U        // 单次最大本机交付数量
#define LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID  UINT32_MAX // 无效物理来源分组索引

/****************************** 本机交付 ******************************/

typedef struct
{
    linkg_transport_type_t     type;                                         // 当前批次Transport类型
    linkg_transport_delivery_t items[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX]; // 本机交付元素
    bool                       owned[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX]; // Transport是否持有Packet引用
    uint32_t                   count;                                        // 当前元素数量
} linkg_transport_delivery_batch_t;

/****************************** AP转发 ******************************/

typedef struct
{
    linkg_transport_forward_item_t items[LINKG_TRANSPORT_FORWARD_BATCH_MAX]; // AP转发元素
    bool                           owned[LINKG_TRANSPORT_FORWARD_BATCH_MAX]; // 当前Batch是否持有Packet引用
    uint32_t                       count;                                    // 当前元素数量
} linkg_transport_forward_batch_t;

/****************************** 批量接收状态 ******************************/

typedef struct
{
    linkg_path_endpoint_t source;       // 当前物理来源端点
    uint64_t              bytes;        // 当前来源本批次物理接收字节数
    uint64_t              packets;      // 当前来源本批次物理接收包数
    uint8_t               peer_node_id; // 当前来源对应的直接Peer节点编号
    bool                  resolved;     // 是否已经成功归属Node Path
} linkg_transport_rx_source_group_t;

typedef struct
{
    linkg_transport_header_t          header;             // Transport基础头
    linkg_transport_fragment_header_t fragment_header;    // 分片扩展头
    linkg_packet_t                   *completed_packet;   // 已完成重组的完整Packet
    uint32_t                          source_group_index; // 当前物理来源分组索引
    uint32_t                          payload_length;     // 当前物理Transport帧业务载荷长度
    linkg_transport_window_result_t   window_result;      // Peer接收窗口结果
    int                               reassembly_result;  // 1完成，0等待，负值表示失败
    bool                              decoded;            // Transport协议头是否成功解析
    bool                              fragmented;         // 是否为LinkG分片
} linkg_transport_rx_state_t;

typedef struct
{
    uint64_t invalid_frames;      // 当前内部批次非法Transport帧数量
    uint64_t unattributed_frames; // 当前内部批次无法归属直接Peer帧数量
} linkg_transport_rx_batch_stats_t;

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断两个物理路径端点是否相同。
 */
static bool _linkg_transport_rx_endpoint_equal(const linkg_path_endpoint_t *left, const linkg_path_endpoint_t *right)
{
    const struct sockaddr_in6 *left6;
    const struct sockaddr_in6 *right6;
    const struct sockaddr_in  *left4;
    const struct sockaddr_in  *right4;

    if (left == NULL || right == NULL)
    {
        return false;
    }

    if (left->length != right->length || left->address.ss_family != right->address.ss_family)
    {
        return false;
    }

    if (left->address.ss_family == AF_INET)
    {
        left4  = (const struct sockaddr_in *)&left->address;
        right4 = (const struct sockaddr_in *)&right->address;

        return left4->sin_addr.s_addr == right4->sin_addr.s_addr &&
               left4->sin_port == right4->sin_port;
    }

    if (left->address.ss_family == AF_INET6)
    {
        left6  = (const struct sockaddr_in6 *)&left->address;
        right6 = (const struct sockaddr_in6 *)&right->address;

        return memcmp(&left6->sin6_addr, &right6->sin6_addr, sizeof(left6->sin6_addr)) == 0 &&
               left6->sin6_port == right6->sin6_port &&
               left6->sin6_scope_id == right6->sin6_scope_id;
    }

    return false;
}

/**
 * @brief 批量归并Transport全局异常统计。
 *
 * 正常高速数据路径两个计数均为0时不获取g_transport.lock。
 */
static void _linkg_transport_rx_record_batch_stats(const linkg_transport_rx_batch_stats_t *stats)
{
    if (stats == NULL)
    {
        return;
    }

    if (stats->invalid_frames == 0U && stats->unattributed_frames == 0U)
    {
        return;
    }

    pthread_mutex_lock(&g_transport.lock);

    g_transport.stats.rx_invalid_frames      += stats->invalid_frames;
    g_transport.stats.rx_unattributed_frames += stats->unattributed_frames;

    pthread_mutex_unlock(&g_transport.lock);
}

/****************************** 本机交付 ******************************/

/**
 * @brief 立即同步交付当前本机数据批次。
 *
 * 普通Packet借用Link RX引用，重组Packet由Transport持有并在Handler返回后释放。
 */
static void _linkg_transport_rx_flush_delivery(linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_handler_func_t handler;
    void                          *handler_user_data;
    uint32_t                       index;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    handler           = g_transport.handlers[batch->type].handler;
    handler_user_data = g_transport.handlers[batch->type].user_data;

    if (handler != NULL)
    {
        (void)handler(batch->items, batch->count, handler_user_data);
    }

    for (index = 0U; index < batch->count; index++)
    {
        if (batch->owned[index] && batch->items[index].packet != NULL)
        {
            linkg_packet_release(batch->items[index].packet);
        }
    }

    memset(batch, 0, sizeof(*batch));

    batch->type = LINKG_TRANSPORT_TYPE_NONE;
}

/**
 * @brief 将Transport载荷Packet加入本机交付批次。
 */
static void _linkg_transport_rx_queue_payload(const linkg_transport_header_t *header, linkg_packet_t *packet, bool owned, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_delivery_t *delivery;
    linkg_transport_type_t      type;

    if (header == NULL || packet == NULL || batch == NULL)
    {
        return;
    }

    type = (linkg_transport_type_t)header->type;

    if (batch->count > 0U && batch->type != type)
    {
        _linkg_transport_rx_flush_delivery(batch);
    }

    if (batch->count >= LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX)
    {
        _linkg_transport_rx_flush_delivery(batch);
    }

    batch->type = type;

    delivery         = &batch->items[batch->count];
    delivery->header = *header;
    delivery->packet = packet;

    batch->owned[batch->count] = owned;
    batch->count++;

    if (batch->count >= LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX)
    {
        _linkg_transport_rx_flush_delivery(batch);
    }
}

/**
 * @brief 去除普通Transport帧基础头并加入本机交付批次。
 */
static int _linkg_transport_rx_queue_normal_delivery(const linkg_transport_header_t *header, linkg_link_rx_item_t *item, linkg_transport_delivery_batch_t *batch)
{
    if (header == NULL || item == NULL || item->packet == NULL || batch == NULL)
    {
        return -EINVAL;
    }

    if (linkg_packet_pull(item->packet, LINKG_TRANSPORT_WIRE_HEADER_SIZE) == NULL)
    {
        return -EPROTO;
    }

    _linkg_transport_rx_queue_payload(header, item->packet, false, batch);

    return 0;
}

/**
 * @brief 将完整重组Packet加入本机交付批次。
 */
static void _linkg_transport_rx_queue_reassembled_delivery(const linkg_transport_header_t *header, linkg_packet_t *packet, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_header_t delivery_header;

    if (header == NULL || packet == NULL || batch == NULL)
    {
        return;
    }

    delivery_header = *header;
    delivery_header.flags &= (uint16_t)~LINKG_TRANSPORT_FLAG_FRAGMENT;

    _linkg_transport_rx_queue_payload(&delivery_header, packet, true, batch);
}

/****************************** AP转发 ******************************/

/**
 * @brief 同步提交当前AP转发批次。
 *
 * Pair Cache输出Packet由当前Batch持有引用，Forward返回后统一释放。
 */
static void _linkg_transport_rx_flush_forward(linkg_transport_forward_batch_t *batch)
{
    uint32_t index;
    int      ret;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    ret = linkg_transport_forward_batch(batch->items, batch->count);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("submit transport forward batch failed, count=%u, error=%d", batch->count, ret);
    }

    for (index = 0U; index < batch->count; index++)
    {
        if (batch->owned[index] && batch->items[index].packet != NULL)
        {
            linkg_packet_release(batch->items[index].packet);
        }
    }

    memset(batch, 0, sizeof(*batch));
}

/**
 * @brief 将非分片Transport帧加入当前AP同步转发批次。
 *
 * 当前Packet仅借用Link RX基础引用，不由Forward Batch释放。
 */
static void _linkg_transport_rx_append_forward(const linkg_transport_header_t *header, linkg_link_rx_item_t *item, uint32_t payload_length, linkg_transport_forward_batch_t *batch)
{
    linkg_transport_forward_item_t *forward_item;

    if (header == NULL || item == NULL || item->packet == NULL || batch == NULL)
    {
        return;
    }

    if (batch->count >= LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        _linkg_transport_rx_flush_forward(batch);
    }

    forward_item = &batch->items[batch->count];

    forward_item->packet              = item->packet;
    forward_item->destination_node_id = header->destination_node_id;
    forward_item->type                = (linkg_transport_type_t)header->type;
    forward_item->payload_length      = payload_length;

    batch->owned[batch->count] = false;
    batch->count++;

    if (batch->count >= LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        _linkg_transport_rx_flush_forward(batch);
    }
}

/**
 * @brief 将完整配对的FIRST和LAST连续加入AP Forward Batch。
 *
 * Pair两个Packet引用均由当前Batch接管。
 * 如果当前Batch只剩一个位置，则先Flush旧Batch，保证Pair不被人为拆开。
 */
static void _linkg_transport_rx_append_forward_pair(const linkg_transport_forward_item_t *items, linkg_transport_forward_batch_t *batch)
{
    uint32_t index;

    if (items == NULL || batch == NULL)
    {
        return;
    }

    if (batch->count + LINKG_TRANSPORT_FRAGMENT_COUNT_MAX > LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        _linkg_transport_rx_flush_forward(batch);
    }

    for (index = 0U; index < LINKG_TRANSPORT_FRAGMENT_COUNT_MAX; index++)
    {
        batch->items[batch->count] = items[index];
        batch->owned[batch->count] = true;
        batch->count++;
    }

    if (batch->count >= LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        _linkg_transport_rx_flush_forward(batch);
    }
}

/****************************** Path归属 ******************************/

/**
 * @brief 根据当前Link RX批次建立物理来源分组。
 *
 * 同一Link下相同来源端点只建立一个Group，后续Path查询和统计按Group聚合。
 */
static void _linkg_transport_rx_build_source_groups(const linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_rx_source_group_t *groups, uint32_t *group_count)
{
    uint32_t current_group_count;
    uint32_t group_index;
    uint32_t index;

    current_group_count = 0U;

    for (index = 0U; index < count; index++)
    {
        states[index].source_group_index = LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID;
        states[index].window_result      = LINKG_TRANSPORT_WINDOW_INVALID;
        states[index].reassembly_result  = -EINPROGRESS;

        if (items[index].packet == NULL)
        {
            continue;
        }

        for (group_index = 0U; group_index < current_group_count; group_index++)
        {
            if (_linkg_transport_rx_endpoint_equal(&groups[group_index].source, &items[index].source))
            {
                break;
            }
        }

        if (group_index == current_group_count)
        {
            groups[group_index].source = items[index].source;
            current_group_count++;
        }

        groups[group_index].bytes += items[index].packet->data_length;
        groups[group_index].packets++;

        states[index].source_group_index = group_index;
    }

    *group_count = current_group_count;
}

/**
 * @brief 批量解析物理来源对应的Node Path和直接Peer。
 *
 * 整个Transport Chunk一次提交全部Source Group，Node内部统一完成Path归属和接收统计。
 */
static void _linkg_transport_rx_resolve_source_groups(uint32_t link_id, linkg_transport_rx_source_group_t *groups, uint32_t group_count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_node_path_rx_item_t node_items[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    uint32_t                  group_index;
    int                       ret;

    if (group_count == 0U)
    {
        return;
    }

    memset(node_items, 0, sizeof(node_items));

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        node_items[group_index].source  = groups[group_index].source;
        node_items[group_index].bytes   = groups[group_index].bytes;
        node_items[group_index].packets = groups[group_index].packets;
    }

    ret = linkg_node_account_path_rx_batch(link_id, node_items, group_count);
    if (ret != 0)
    {
        for (group_index = 0U; group_index < group_count; group_index++)
        {
            stats->unattributed_frames += groups[group_index].packets;
        }

        return;
    }

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        if (node_items[group_index].result != 0)
        {
            stats->unattributed_frames += groups[group_index].packets;
            continue;
        }

        groups[group_index].peer_node_id = node_items[group_index].peer_node_id;
        groups[group_index].resolved     = true;
    }
}

/****************************** 协议解析 ******************************/

/**
 * @brief 批量解析Transport基础头、分片头和业务载荷长度。
 *
 * 本阶段完全无锁，只处理已经成功归属物理Path的帧。
 */
static void _linkg_transport_rx_decode_batch(const linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, const linkg_transport_rx_source_group_t *groups, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_packet_t *packet;
    uint32_t        group_index;
    uint32_t        index;
    int             ret;

    for (index = 0U; index < count; index++)
    {
        packet      = items[index].packet;
        group_index = states[index].source_group_index;

        if (packet == NULL || group_index == LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID)
        {
            continue;
        }

        if (!groups[group_index].resolved)
        {
            continue;
        }

        ret = linkg_transport_wire_decode(packet, &states[index].header);
        if (ret != 0)
        {
            stats->invalid_frames++;
            continue;
        }

        states[index].fragmented = (states[index].header.flags & LINKG_TRANSPORT_FLAG_FRAGMENT) != 0U;

        if (states[index].fragmented)
        {
            ret = linkg_transport_wire_fragment_decode(packet, &states[index].fragment_header);
            if (ret != 0)
            {
                stats->invalid_frames++;
                continue;
            }

            states[index].payload_length = packet->data_length - LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE;
        }
        else
        {
            states[index].payload_length = packet->data_length - LINKG_TRANSPORT_WIRE_HEADER_SIZE;
        }

        states[index].decoded = true;
    }
}

/****************************** 接收窗口 ******************************/

/**
 * @brief 整批执行直接Peer接收窗口去重和Transport物理帧统计。
 *
 * 整个Chunk只获取一次g_transport.lock，每个物理Source Group只查询一次Transport Peer。
 */
static int _linkg_transport_rx_accept_batch(linkg_transport_rx_state_t *states, uint32_t count, const linkg_transport_rx_source_group_t *groups, uint32_t group_count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_peer_t       *group_peers[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_type_stats_t *type_stats;
    linkg_transport_peer_t       *peer;
    uint64_t                      now_ms;
    uint32_t                      group_index;
    uint32_t                      index;
    bool                          have_decoded;
    bool                          now_valid;
    int                           ret;

    have_decoded = false;

    for (index = 0U; index < count; index++)
    {
        if (states[index].decoded)
        {
            have_decoded = true;
            break;
        }
    }

    if (!have_decoded)
    {
        return 0;
    }

    memset(group_peers, 0, sizeof(group_peers));

    now_ms    = 0U;
    now_valid = false;

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        if (groups[group_index].resolved)
        {
            group_peers[group_index] = linkg_transport_find_peer_locked(groups[group_index].peer_node_id);
        }
    }

    for (index = 0U; index < count; index++)
    {
        if (!states[index].decoded)
        {
            continue;
        }

        group_index = states[index].source_group_index;
        peer        = group_peers[group_index];

        if (peer == NULL)
        {
            stats->unattributed_frames++;
            continue;
        }

        states[index].window_result = linkg_transport_window_accept(&peer->rx_window, states[index].header.sequence);

        type_stats = &peer->stats.types[states[index].header.type];

        if (states[index].window_result == LINKG_TRANSPORT_WINDOW_ACCEPT)
        {
            type_stats->rx_packets++;
            type_stats->rx_bytes += states[index].payload_length;

            if (!now_valid)
            {
                now_ms    = linkg_time_elapsed_ms();
                now_valid = true;
            }

            peer->stats.last_rx_ms = now_ms;
        }
        else if (states[index].window_result == LINKG_TRANSPORT_WINDOW_DUPLICATE)
        {
            type_stats->rx_duplicate_packets++;
        }
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 分片重组 ******************************/

/**
 * @brief 批量处理目标为本机的已接受分片。
 *
 * 所有本机分片一次进入Reassembly，整个分片子批次只获取一次重组锁。
 */
static void _linkg_transport_rx_reassemble_batch(linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_reassembly_submit_item_t reassembly_items[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    uint32_t                                 logical_indices[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    uint32_t                                 reassembly_count;
    uint32_t                                 logical_index;
    uint32_t                                 index;
    int                                      ret;

    reassembly_count = 0U;

    for (index = 0U; index < count; index++)
    {
        if (states[index].window_result != LINKG_TRANSPORT_WINDOW_ACCEPT)
        {
            continue;
        }

        if (!states[index].fragmented)
        {
            continue;
        }

        if (states[index].header.destination_node_id != g_transport.local_node_id)
        {
            continue;
        }

        reassembly_items[reassembly_count].header           = &states[index].header;
        reassembly_items[reassembly_count].fragment_header  = &states[index].fragment_header;
        reassembly_items[reassembly_count].packet           = items[index].packet;
        reassembly_items[reassembly_count].completed_packet = NULL;
        reassembly_items[reassembly_count].result           = -EINPROGRESS;

        logical_indices[reassembly_count] = index;

        reassembly_count++;
    }

    if (reassembly_count == 0U)
    {
        return;
    }

    ret = linkg_transport_reassembly_submit_batch(reassembly_items, reassembly_count);
    if (ret != 0)
    {
        for (index = 0U; index < reassembly_count; index++)
        {
            if (reassembly_items[index].completed_packet != NULL)
            {
                linkg_packet_release(reassembly_items[index].completed_packet);
            }

            logical_index = logical_indices[index];

            states[logical_index].reassembly_result = ret;
            stats->invalid_frames++;
        }

        return;
    }

    for (index = 0U; index < reassembly_count; index++)
    {
        logical_index = logical_indices[index];

        states[logical_index].reassembly_result = reassembly_items[index].result;
        states[logical_index].completed_packet  = reassembly_items[index].completed_packet;

        if (states[logical_index].reassembly_result < 0)
        {
            if (states[logical_index].completed_packet != NULL)
            {
                linkg_packet_release(states[logical_index].completed_packet);
                states[logical_index].completed_packet = NULL;
            }

            stats->invalid_frames++;
            continue;
        }

        if (states[logical_index].reassembly_result == 1 && states[logical_index].completed_packet == NULL)
        {
            stats->invalid_frames++;
        }
    }
}

/****************************** 数据分发 ******************************/

/**
 * @brief 按物理帧原始接收顺序分发已经通过接收窗口的数据。
 *
 * 本机普通帧直接交付，本机分片执行Reassembly；
 * AP非本机普通帧直接转发，分片必须完成Pair后才向下一跳提交。
 */
static void _linkg_transport_rx_dispatch_batch(linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_delivery_batch_t *delivery_batch, linkg_transport_forward_batch_t *forward_batch, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_forward_item_t pair_items[LINKG_TRANSPORT_FRAGMENT_COUNT_MAX];
    uint32_t                       pair_count;
    uint32_t                       pair_index;
    uint32_t                       index;
    int                            ret;

    for (index = 0U; index < count; index++)
    {
        if (states[index].window_result != LINKG_TRANSPORT_WINDOW_ACCEPT)
        {
            continue;
        }

        if (states[index].header.destination_node_id == g_transport.local_node_id)
        {
            if (states[index].fragmented)
            {
                if (states[index].reassembly_result == 1 && states[index].completed_packet != NULL)
                {
                    _linkg_transport_rx_queue_reassembled_delivery(&states[index].header, states[index].completed_packet, delivery_batch);
                    states[index].completed_packet = NULL;
                }

                continue;
            }

            ret = _linkg_transport_rx_queue_normal_delivery(&states[index].header, &items[index], delivery_batch);
            if (ret != 0)
            {
                stats->invalid_frames++;
            }

            continue;
        }

        if (g_transport.local_role != LINKG_DEVICE_ROLE_AP)
        {
            continue;
        }

        if (!states[index].fragmented)
        {
            _linkg_transport_rx_append_forward(&states[index].header, &items[index], states[index].payload_length, forward_batch);
            continue;
        }

        memset(pair_items, 0, sizeof(pair_items));

        pair_count = 0U;

        ret = linkg_transport_forward_pair_submit(&states[index].header,
                                                   &states[index].fragment_header,
                                                   items[index].packet,
                                                   states[index].payload_length,
                                                   pair_items,
                                                   &pair_count);
        if (ret != 0)
        {
            for (pair_index = 0U; pair_index < pair_count; pair_index++)
            {
                if (pair_items[pair_index].packet != NULL)
                {
                    linkg_packet_release(pair_items[pair_index].packet);
                }
            }

            if (ret == -EPROTO || ret == -EINVAL)
            {
                stats->invalid_frames++;
            }
            else
            {
                LINKG_LOG_ERROR("transport forward fragment pair failed, error=%d", ret);
            }

            continue;
        }

        if (pair_count == 0U)
        {
            continue;
        }

        if (pair_count != LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)
        {
            for (pair_index = 0U; pair_index < pair_count; pair_index++)
            {
                if (pair_items[pair_index].packet != NULL)
                {
                    linkg_packet_release(pair_items[pair_index].packet);
                }
            }

            stats->invalid_frames++;
            continue;
        }

        _linkg_transport_rx_append_forward_pair(pair_items, forward_batch);
    }
}

/****************************** 内部批次 ******************************/

/**
 * @brief 处理一个Transport内部接收Chunk。
 *
 * 流水线为来源分组、Path归属、协议解析、Peer窗口、分片重组和数据分发。
 */
static void _linkg_transport_rx_process_chunk(uint32_t link_id, linkg_link_rx_item_t *items, uint32_t count, linkg_transport_delivery_batch_t *delivery_batch, linkg_transport_forward_batch_t *forward_batch)
{
    linkg_transport_rx_source_group_t groups[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_rx_state_t        states[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_rx_batch_stats_t  stats;
    uint32_t                          group_count;
    int                               ret;

    memset(groups, 0, sizeof(groups));
    memset(states, 0, sizeof(states));
    memset(&stats, 0, sizeof(stats));

    group_count = 0U;

    _linkg_transport_rx_build_source_groups(items, states, count, groups, &group_count);
    _linkg_transport_rx_resolve_source_groups(link_id, groups, group_count, &stats);
    _linkg_transport_rx_decode_batch(items, states, count, groups, &stats);

    ret = _linkg_transport_rx_accept_batch(states, count, groups, group_count, &stats);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("transport RX accept batch failed, count=%u, error=%d", count, ret);
        _linkg_transport_rx_record_batch_stats(&stats);
        return;
    }

    _linkg_transport_rx_reassemble_batch(items, states, count, &stats);
    _linkg_transport_rx_dispatch_batch(items, states, count, delivery_batch, forward_batch, &stats);
    _linkg_transport_rx_record_batch_stats(&stats);
}

/****************************** 数据接收 ******************************/

/**
 * @brief Link Base统一批量接收回调。
 *
 * Link Base负责提供物理RX Batch，Transport内部按Chunk执行完整接收流水线。
 * AP转发保持原始source_node_id、destination_node_id、packet_id和分片边界，转发阶段只重新分配逐跳sequence。
 */
void linkg_transport_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t count, void *user_data)
{
    linkg_transport_delivery_batch_t delivery_batch;
    linkg_transport_forward_batch_t  forward_batch;
    uint32_t                         chunk_count;
    uint32_t                         link_id;
    uint32_t                         offset;

    (void)user_data;

    if (link == NULL || items == NULL || count == 0U)
    {
        return;
    }

    link_id = linkg_link_get_id(link);

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    memset(&delivery_batch, 0, sizeof(delivery_batch));
    memset(&forward_batch, 0, sizeof(forward_batch));

    delivery_batch.type = LINKG_TRANSPORT_TYPE_NONE;
    offset              = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE)
        {
            chunk_count = LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE;
        }

        _linkg_transport_rx_process_chunk(link_id, &items[offset], chunk_count, &delivery_batch, &forward_batch);

        offset += chunk_count;
    }

    _linkg_transport_rx_flush_forward(&forward_batch);
    _linkg_transport_rx_flush_delivery(&delivery_batch);
}
