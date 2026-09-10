/**
 * @file transport_rx.c
 * @brief LinkG逻辑传输层批量接收实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-10
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
#define LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX    32U        // 单个Transport类型最大本机交付数量
#define LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID  UINT32_MAX // 无效物理来源分组索引

/****************************** 本机交付 ******************************/

/**
 * @brief 单个Transport类型的本机交付批次。
 */
typedef struct
{
    linkg_transport_delivery_t items[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX]; // 本机交付元素，可混合业务Class
    bool                       owned[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX]; // Transport是否持有对应Packet引用
    uint32_t                   count;                                        // 当前元素数量
} linkg_transport_delivery_batch_t;

/****************************** 中继交付 ******************************/

/**
 * @brief 单个业务Class的中继调度批次。
 */
typedef struct
{
    linkg_transport_forward_item_t items[LINKG_TRANSPORT_FORWARD_BATCH_MAX]; // 同一业务Class中继元素
    bool                           owned[LINKG_TRANSPORT_FORWARD_BATCH_MAX]; // Transport是否持有对应Packet引用
    uint32_t                       count;                                    // 当前元素数量
} linkg_transport_forward_batch_t;

/****************************** 批量接收状态 ******************************/

/**
 * @brief 当前Link批次中的物理来源分组。
 */
typedef struct
{
    linkg_path_endpoint_t source;       // 当前物理来源端点
    uint64_t              bytes;        // 当前来源本批次物理接收字节数
    uint64_t              packets;      // 当前来源本批次物理接收包数
    uint8_t               peer_node_id; // 当前来源对应的直接Peer节点编号
    bool                  resolved;     // 是否已经成功归属Node Path
} linkg_transport_rx_source_group_t;

/**
 * @brief 单个Transport Wire Frame接收处理状态。
 */
typedef struct
{
    linkg_transport_header_t          header;             // Transport基础头
    linkg_transport_fragment_header_t fragment_header;    // Transport分片扩展头
    linkg_packet_t                   *completed_packet;   // 已完成重组的完整Payload Packet
    uint32_t                          source_group_index; // 当前物理来源分组索引
    uint32_t                          payload_length;     // 当前Wire Frame实际Transport载荷长度
    linkg_transport_class_t           traffic_class;      // 当前Wire Frame业务类别
    linkg_transport_window_result_t   window_result;      // Peer/Class接收窗口结果
    int                               reassembly_result;  // 1完成，0等待，负值表示失败
    uint8_t                           peer_node_id;       // 当前物理上一跳直接Peer节点编号
    bool                              decoded;            // Transport协议是否成功解析
    bool                              fragmented;         // 是否为LinkG内部分片
} linkg_transport_rx_state_t;

/**
 * @brief 当前内部接收Chunk全局异常累计。
 */
typedef struct
{
    uint64_t invalid_frames;      // 非法Transport帧数量
    uint64_t unattributed_frames; // 无法归属直接Peer帧数量
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

        return left4->sin_addr.s_addr == right4->sin_addr.s_addr && left4->sin_port == right4->sin_port;
    }

    if (left->address.ss_family == AF_INET6)
    {
        left6  = (const struct sockaddr_in6 *)&left->address;
        right6 = (const struct sockaddr_in6 *)&right->address;

        return memcmp(&left6->sin6_addr, &right6->sin6_addr, sizeof(left6->sin6_addr)) == 0 && left6->sin6_port == right6->sin6_port && left6->sin6_scope_id == right6->sin6_scope_id;
    }

    return false;
}

/**
 * @brief 从Link Packet业务标志恢复Transport业务类别。
 */
static linkg_transport_class_t _linkg_transport_rx_packet_class(const linkg_packet_t *packet)
{
    if (linkg_packet_is_realtime(packet))
    {
        return LINKG_TRANSPORT_CLASS_REALTIME;
    }

    if (linkg_packet_is_video(packet))
    {
        return LINKG_TRANSPORT_CLASS_VIDEO;
    }

    return LINKG_TRANSPORT_CLASS_DATA;
}

/**
 * @brief 判断sequence是否位于当前最高序列号之前。
 */
static bool _linkg_transport_rx_sequence_before(uint32_t sequence, uint32_t highest_sequence)
{
    return (int32_t)(sequence - highest_sequence) < 0;
}

/**
 * @brief 批量归并Transport全局异常统计。
 *
 * @note 正常高速路径两个计数均为0时不获取g_transport.lock。
 */
static void _linkg_transport_rx_record_batch_stats(const linkg_transport_rx_batch_stats_t *stats)
{
    if (stats == NULL || (stats->invalid_frames == 0U && stats->unattributed_frames == 0U))
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
 * @brief 同步交付指定Transport类型的本机Payload批次。
 *
 * @note Handler调用期间Packet均为借用引用；重组Packet由Transport在Handler返回后释放。
 */
static void _linkg_transport_rx_flush_delivery(linkg_transport_type_t type, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_handler_func_t handler;
    void                          *handler_user_data;
    uint32_t                       index;
    int                            ret;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    handler           = NULL;
    handler_user_data = NULL;

    pthread_mutex_lock(&g_transport.lock);

    if (g_transport.initialized && linkg_transport_type_valid(type))
    {
        handler           = g_transport.handlers[type].handler;
        handler_user_data = g_transport.handlers[type].user_data;
    }

    pthread_mutex_unlock(&g_transport.lock);

    if (handler != NULL)
    {
        ret = handler(batch->items, batch->count, handler_user_data);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("transport local delivery failed, type=%d, count=%u, error=%d", (int)type, batch->count, ret);
        }
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
 * @brief 将一个本机Payload加入指定Transport类型批次。
 */
static void _linkg_transport_rx_append_delivery(linkg_transport_type_t type, linkg_transport_class_t traffic_class, uint8_t source_node_id, uint8_t peer_node_id, linkg_packet_t *packet, bool owned, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_delivery_t *delivery;

    if (packet == NULL || batch == NULL)
    {
        return;
    }

    if (batch->count >= LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX)
    {
        _linkg_transport_rx_flush_delivery(type, batch);
    }

    delivery = &batch->items[batch->count];

    delivery->packet         = packet;
    delivery->traffic_class  = traffic_class;
    delivery->source_node_id = source_node_id;
    delivery->peer_node_id   = peer_node_id;

    batch->owned[batch->count] = owned;
    batch->count++;
}

/**
 * @brief 去除普通Transport基础头并加入本机类型批次。
 */
static int _linkg_transport_rx_append_normal_delivery(const linkg_transport_rx_state_t *state, linkg_link_rx_item_t *item, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_type_t type;

    if (state == NULL || item == NULL || item->packet == NULL || batch == NULL)
    {
        return -EINVAL;
    }

    if (linkg_packet_pull(item->packet, LINKG_TRANSPORT_WIRE_HEADER_SIZE) == NULL)
    {
        return -EPROTO;
    }

    type = (linkg_transport_type_t)state->header.type;

    _linkg_transport_rx_append_delivery(type, state->traffic_class, state->header.source_node_id, state->peer_node_id, item->packet, false, batch);

    return 0;
}

/**
 * @brief 将完整重组Payload加入本机类型批次。
 */
static void _linkg_transport_rx_append_reassembled_delivery(const linkg_transport_rx_state_t *state, linkg_packet_t *packet, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_type_t type;

    if (state == NULL || packet == NULL || batch == NULL)
    {
        return;
    }

    type = (linkg_transport_type_t)state->header.type;

    _linkg_transport_rx_append_delivery(type, state->traffic_class, state->header.source_node_id, state->peer_node_id, packet, true, batch);
}

/**
 * @brief 依次Flush全部本机Transport类型批次。
 */
static void _linkg_transport_rx_flush_deliveries(linkg_transport_delivery_batch_t *batches)
{
    uint32_t type_index;

    for (type_index = (uint32_t)LINKG_TRANSPORT_TYPE_NONE + 1U; type_index < LINKG_TRANSPORT_TYPE_COUNT; type_index++)
    {
        _linkg_transport_rx_flush_delivery((linkg_transport_type_t)type_index, &batches[type_index]);
    }
}

/****************************** 中继交付 ******************************/

/**
 * @brief 同步提交指定业务Class的中继批次给Scheduler回调。
 *
 * @note 一个回调批次只包含一个traffic_class，Scheduler不需要再次执行业务分类。
 */
static void _linkg_transport_rx_flush_forward(linkg_transport_class_t traffic_class, linkg_transport_forward_batch_t *batch)
{
    int                                    results[LINKG_TRANSPORT_FORWARD_BATCH_MAX];
    linkg_transport_forward_handler_func_t handler;
    void                                  *handler_user_data;
    uint32_t                               index;
    int                                    ret;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    handler           = NULL;
    handler_user_data = NULL;

    pthread_mutex_lock(&g_transport.lock);

    if (g_transport.initialized)
    {
        handler           = g_transport.forward_handler.handler;
        handler_user_data = g_transport.forward_handler.user_data;
    }

    pthread_mutex_unlock(&g_transport.lock);

    if (handler != NULL)
    {
        for (index = 0U; index < batch->count; index++)
        {
            results[index] = -EINPROGRESS;
        }

        ret = handler(traffic_class, batch->items, batch->count, results, handler_user_data);
        if (ret < 0)
        {
            LINKG_LOG_ERROR("transport forward delivery failed, class=%d, count=%u, error=%d", (int)traffic_class, batch->count, ret);
        }
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
 * @brief 将一个非分片Wire Frame加入指定业务Class中继批次。
 *
 * @note 当前Packet仅借用Link RX基础引用，不由Forward Batch释放。
 */
static void _linkg_transport_rx_append_forward(const linkg_transport_rx_state_t *state, linkg_link_rx_item_t *item, linkg_transport_forward_batch_t *batch)
{
    linkg_transport_forward_item_t *forward_item;

    if (state == NULL || item == NULL || item->packet == NULL || batch == NULL)
    {
        return;
    }

    if (batch->count >= LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        _linkg_transport_rx_flush_forward(state->traffic_class, batch);
    }

    forward_item = &batch->items[batch->count];

    forward_item->packet              = item->packet;
    forward_item->payload_length      = state->payload_length;
    forward_item->destination_node_id = state->header.destination_node_id;
    forward_item->peer_node_id        = state->peer_node_id;

    batch->owned[batch->count] = false;
    batch->count++;
}

/**
 * @brief 将完整配对的FIRST/LAST连续加入指定业务Class中继批次。
 *
 * @note Pair输出的两个Packet引用均由当前Forward Batch接管。
 */
static void _linkg_transport_rx_append_forward_pair(linkg_transport_class_t traffic_class, const linkg_transport_forward_item_t *items, linkg_transport_forward_batch_t *batch)
{
    uint32_t index;

    if (items == NULL || batch == NULL)
    {
        return;
    }

    if (batch->count + LINKG_TRANSPORT_FRAGMENT_COUNT_MAX > LINKG_TRANSPORT_FORWARD_BATCH_MAX)
    {
        _linkg_transport_rx_flush_forward(traffic_class, batch);
    }

    for (index = 0U; index < LINKG_TRANSPORT_FRAGMENT_COUNT_MAX; index++)
    {
        batch->items[batch->count] = items[index];
        batch->owned[batch->count] = true;
        batch->count++;
    }
}

/**
 * @brief 按REALTIME、VIDEO、DATA顺序Flush全部中继业务Class批次。
 */
static void _linkg_transport_rx_flush_forwards(linkg_transport_forward_batch_t *batches)
{
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        _linkg_transport_rx_flush_forward((linkg_transport_class_t)class_index, &batches[class_index]);
    }
}

/****************************** Path归属 ******************************/

/**
 * @brief 根据当前Link RX批次建立物理来源分组。
 *
 * @note 同一Link下相同来源端点只建立一个Group，后续Path查询和物理RX统计按Group聚合。
 */
static void _linkg_transport_rx_build_source_groups(const linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_rx_source_group_t *groups, uint32_t *group_count, linkg_transport_rx_batch_stats_t *stats)
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
            if (current_group_count >= LINKG_RESOURCE_NETWORK_STA_MAX)
            {
                LINKG_LOG_DEBUG("transport RX source group overflow, index=%u, group_count=%u, max=%u", index, current_group_count, LINKG_RESOURCE_NETWORK_STA_MAX);
                stats->unattributed_frames++;
                continue;
            }

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
 * @brief 批量解析当前Link物理来源对应的Node Path和直接Peer。
 *
 * @note Wi-Fi和Cellular分别通过各自link_id进行Path统计，但后续Transport窗口统一归并到直接Peer/Class协议域。
 */
static void _linkg_transport_rx_resolve_source_groups(uint32_t link_id, linkg_transport_rx_source_group_t *groups, uint32_t group_count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_node_path_rx_item_t node_items[LINKG_RESOURCE_NETWORK_STA_MAX];
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
 * @brief 批量恢复业务Class并解析Transport基础头、分片头和载荷长度。
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

        if (packet == NULL || group_index == LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID || !groups[group_index].resolved)
        {
            continue;
        }

        states[index].traffic_class = _linkg_transport_rx_packet_class(packet);
        states[index].peer_node_id  = groups[group_index].peer_node_id;

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
 * @brief 整批执行直接Peer/Class接收窗口去重、乱序识别和累计统计。
 *
 * @note Wi-Fi和Cellular指向同一直接Peer且业务Class相同时共用同一个RX Window，双发副本因此会被跨Link去重。
 */
static int _linkg_transport_rx_accept_batch(linkg_transport_rx_state_t *states, uint32_t count, const linkg_transport_rx_source_group_t *groups, uint32_t group_count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_peer_t       *group_peers[LINKG_RESOURCE_NETWORK_STA_MAX];
    linkg_transport_peer_class_t *peer_class;
    linkg_transport_peer_t       *peer;
    uint64_t                      now_ms;
    uint32_t                      group_index;
    uint32_t                      index;
    bool                          out_of_order;
    int                           ret;

    for (index = 0U; index < count; index++)
    {
        if (states[index].decoded)
        {
            break;
        }
    }

    if (index == count)
    {
        return 0;
    }

    memset(group_peers, 0, sizeof(group_peers));

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

    now_ms = linkg_time_elapsed_ms();

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

        peer_class  = &peer->classes[states[index].traffic_class];
        out_of_order = peer_class->rx_window.initialized && _linkg_transport_rx_sequence_before(states[index].header.sequence, peer_class->rx_window.highest_sequence);

        states[index].window_result = linkg_transport_window_accept(&peer_class->rx_window, states[index].header.sequence);

        if (states[index].window_result == LINKG_TRANSPORT_WINDOW_ACCEPT)
        {
            peer_class->stats.rx_packets++;
            peer_class->stats.rx_bytes += states[index].payload_length;
            peer_class->stats.last_rx_ms = now_ms;

            if (out_of_order)
            {
                peer_class->stats.rx_out_of_order_packets++;
            }
        }
        else if (states[index].window_result == LINKG_TRANSPORT_WINDOW_DUPLICATE)
        {
            peer_class->stats.rx_duplicate_packets++;
        }
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 分片重组 ******************************/

/**
 * @brief 批量处理目标为本机的已接受分片。
 *
 * @note 重组Key包含直接Peer和业务Class，因此Wi-Fi/Cellular同一Peer/Class的两片可以跨物理Link完成重组。
 */
static void _linkg_transport_rx_reassemble_batch(linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_reassembly_submit_item_t reassembly_items[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    uint32_t                                 state_indices[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    uint32_t                                 reassembly_count;
    uint32_t                                 state_index;
    uint32_t                                 index;
    int                                      ret;

    reassembly_count = 0U;

    for (index = 0U; index < count; index++)
    {
        if (states[index].window_result != LINKG_TRANSPORT_WINDOW_ACCEPT || !states[index].fragmented || states[index].header.destination_node_id != g_transport.local_node_id)
        {
            continue;
        }

        reassembly_items[reassembly_count].header           = &states[index].header;
        reassembly_items[reassembly_count].fragment_header  = &states[index].fragment_header;
        reassembly_items[reassembly_count].packet           = items[index].packet;
        reassembly_items[reassembly_count].completed_packet = NULL;
        reassembly_items[reassembly_count].traffic_class    = states[index].traffic_class;
        reassembly_items[reassembly_count].result           = -EINPROGRESS;
        reassembly_items[reassembly_count].peer_node_id     = states[index].peer_node_id;

        state_indices[reassembly_count] = index;
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
            state_index = state_indices[index];
            states[state_index].reassembly_result = ret;
            stats->invalid_frames++;

            if (reassembly_items[index].completed_packet != NULL)
            {
                linkg_packet_release(reassembly_items[index].completed_packet);
            }
        }

        return;
    }

    for (index = 0U; index < reassembly_count; index++)
    {
        state_index = state_indices[index];

        states[state_index].reassembly_result = reassembly_items[index].result;
        states[state_index].completed_packet  = reassembly_items[index].completed_packet;

        if (states[state_index].reassembly_result < 0)
        {
            if (states[state_index].completed_packet != NULL)
            {
                linkg_packet_release(states[state_index].completed_packet);
                states[state_index].completed_packet = NULL;
            }

            stats->invalid_frames++;
        }
        else if (states[state_index].reassembly_result == 1 && states[state_index].completed_packet == NULL)
        {
            stats->invalid_frames++;
        }
    }
}

/****************************** 数据分发 ******************************/

/**
 * @brief 按接收顺序分发已经通过Peer/Class窗口的数据。
 *
 * @note 本机数据按Transport Type进入本机Handler；只有AP的非本机数据进入中继路径，中继批次严格按照traffic_class隔离。
 */
static void _linkg_transport_rx_dispatch_batch(linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_delivery_batch_t *delivery_batches, linkg_transport_forward_batch_t *forward_batches, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_forward_item_t pair_items[LINKG_TRANSPORT_FRAGMENT_COUNT_MAX];
    linkg_transport_type_t         type;
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

        type = (linkg_transport_type_t)states[index].header.type;

        if (states[index].header.destination_node_id == g_transport.local_node_id)
        {
            if (states[index].fragmented)
            {
                if (states[index].reassembly_result == 1 && states[index].completed_packet != NULL)
                {
                    _linkg_transport_rx_append_reassembled_delivery(&states[index], states[index].completed_packet, &delivery_batches[type]);
                    states[index].completed_packet = NULL;
                }

                continue;
            }

            ret = _linkg_transport_rx_append_normal_delivery(&states[index], &items[index], &delivery_batches[type]);
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
            _linkg_transport_rx_append_forward(&states[index], &items[index], &forward_batches[states[index].traffic_class]);
            continue;
        }

        memset(pair_items, 0, sizeof(pair_items));
        pair_count = 0U;

        ret = linkg_transport_forward_pair_submit(states[index].traffic_class, states[index].peer_node_id, &states[index].header, &states[index].fragment_header, items[index].packet, states[index].payload_length, pair_items, &pair_count);
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
                LINKG_LOG_ERROR("transport forward fragment pair failed, class=%d, peer=%u, error=%d", (int)states[index].traffic_class, states[index].peer_node_id, ret);
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

        _linkg_transport_rx_append_forward_pair(states[index].traffic_class, pair_items, &forward_batches[states[index].traffic_class]);
    }
}

/****************************** 内部批次 ******************************/

/**
 * @brief 处理一个Transport内部接收Chunk。
 *
 * @note 流水线固定为Path归属、协议解析、Peer/Class窗口、本机重组和本机/中继分流。
 */
static void _linkg_transport_rx_process_chunk(uint32_t link_id, linkg_link_rx_item_t *items, uint32_t count, linkg_transport_delivery_batch_t *delivery_batches, linkg_transport_forward_batch_t *forward_batches)
{
    linkg_transport_rx_source_group_t groups[LINKG_RESOURCE_NETWORK_STA_MAX];
    linkg_transport_rx_state_t        states[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_rx_batch_stats_t  stats;
    uint32_t                          group_count;
    int                               ret;

    memset(groups, 0, sizeof(groups));
    memset(states, 0, sizeof(states));
    memset(&stats, 0, sizeof(stats));

    group_count = 0U;

    _linkg_transport_rx_build_source_groups(items, states, count, groups, &group_count, &stats);
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
    _linkg_transport_rx_dispatch_batch(items, states, count, delivery_batches, forward_batches, &stats);
    _linkg_transport_rx_record_batch_stats(&stats);
}

/****************************** 数据接收 ******************************/

/**
 * @brief Link Base统一批量接收回调。
 *
 * @note Wi-Fi和Cellular分别完成物理Path归属统计，但Transport去重、乱序和重组统一使用直接Peer+traffic_class协议域。
 *       本机数据直接按Transport Type交付；AP中继数据严格按REALTIME、VIDEO、DATA独立批次交给Scheduler回调。
 */
void linkg_transport_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t count, void *user_data)
{
    linkg_transport_delivery_batch_t delivery_batches[LINKG_TRANSPORT_TYPE_COUNT];
    linkg_transport_forward_batch_t  forward_batches[LINKG_TRANSPORT_CLASS_COUNT];
    uint32_t                         chunk_count;
    uint32_t                         link_id;
    uint32_t                         offset;

    (void)user_data;

    if (link == NULL || items == NULL || count == 0U || !g_transport.initialized)
    {
        return;
    }

    link_id = linkg_link_get_id(link);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    memset(delivery_batches, 0, sizeof(delivery_batches));
    memset(forward_batches, 0, sizeof(forward_batches));

    offset = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE)
        {
            chunk_count = LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE;
        }

        _linkg_transport_rx_process_chunk(link_id, &items[offset], chunk_count, delivery_batches, forward_batches);
        offset += chunk_count;
    }

    _linkg_transport_rx_flush_deliveries(delivery_batches);
    _linkg_transport_rx_flush_forwards(forward_batches);
}
