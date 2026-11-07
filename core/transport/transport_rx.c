/**
 * @file transport_rx.c
 * @brief LinkG逻辑传输层批量接收实现
 * @author Dawn
 * @version 1.4.1
 * @date 2026-09-11
 */

#include "transport_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_packet_pool.h"
#include "linkg_switch.h"
#include "linkg_time.h"

#include "transport_reorder.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE             32U        // 单次Transport内部处理最大物理帧数量
#define LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX           32U        // 单个Transport类型最大本机交付数量
#define LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID         UINT32_MAX // 无效物理来源分组索引
#define LINKG_TRANSPORT_RX_SOURCE_GROUP_LOG_INTERVAL_MS 1000ULL    // 物理来源分组溢出日志最小间隔

#define LINKG_TRANSPORT_RX_REORDER_READY_MAX (LINKG_TRANSPORT_PEER_MAX * LINKG_TRANSPORT_CLASS_COUNT)

typedef enum
{
    LINKG_TRANSPORT_RX_STAGE_INVALID = 0,        // 初始状态，尚未完成有效解析
    LINKG_TRANSPORT_RX_STAGE_DECODED,            // Transport协议解析成功
    LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED,    // RX Window接受，已完成去重
    LINKG_TRANSPORT_RX_STAGE_REORDER_PENDING,    // 已进入Reorder缓存，等待前序序列
    LINKG_TRANSPORT_RX_STAGE_EMITTED,            // 已通过Reorder Emit回调完成交付
    LINKG_TRANSPORT_RX_STAGE_REORDER_READY,      // 已通过顺序处理，可以进入下一阶段
    LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_PENDING, // 分片重组尚未完成，等待完整数据包
    LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_READY,   // 分片重组完成，完整Payload Packet已生成
    LINKG_TRANSPORT_RX_STAGE_DISPATCH_READY,     // 所需处理全部完成，可以进入最终分发
    LINKG_TRANSPORT_RX_STAGE_DROPPED             // 已终止处理，不再向下传递
} linkg_transport_rx_stage_t;

typedef struct
{
    linkg_transport_reorder_frame_t frames[LINKG_TRANSPORT_RX_REORDER_READY_MAX]; // 只保存每Peer/Class一帧的元数据
    uint32_t                        count;                                        // 待确认READY的Peer/Class数量
} linkg_transport_rx_reorder_ready_t;


/****************************** 本机交付 ******************************/

/**
 * @brief 单个Transport类型的本机交付批次。
 */
typedef struct
{
    linkg_transport_delivery_t  items[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX];       // 本机交付元素，可混合业务Class
    linkg_transport_peer_t     *peers[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX];       // 接收时直接Peer固定槽位，仅借用
    uint32_t                    peer_epochs[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX]; // 接收时捕获的Peer代际
    bool                        owned[LINKG_TRANSPORT_RX_DELIVERY_BATCH_MAX];       // Transport是否持有对应Packet引用
    uint32_t                    count;                                              // 当前元素数量
} linkg_transport_delivery_batch_t;

/****************************** 中继交付 ******************************/

/**
 * @brief 单个业务Class的中继调度批次。
 */
typedef struct
{
    linkg_transport_forward_item_t  items[LINKG_TRANSPORT_FORWARD_BATCH_MAX];       // 同一业务Class中继元素
    linkg_transport_peer_t         *peers[LINKG_TRANSPORT_FORWARD_BATCH_MAX];       // 入站直接Peer固定槽位，仅借用
    uint32_t                        peer_epochs[LINKG_TRANSPORT_FORWARD_BATCH_MAX]; // 入站Peer代际
    bool                            owned[LINKG_TRANSPORT_FORWARD_BATCH_MAX];       // Transport是否持有对应Packet引用
    uint32_t                        count;                                          // 当前元素数量
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
    linkg_transport_peer_t           *peer;               // RX Window接受时的直接Peer固定槽位，仅借用

    uint32_t                          source_group_index; // 当前物理来源分组索引
    uint32_t                          ingress_link_id;    // 当前帧物理入口Link ID
    uint32_t                          peer_epoch;         // RX Window接受时捕获的Peer生命周期代际
    uint32_t                          payload_length;     // 当前Wire Frame实际Transport载荷长度

    linkg_transport_class_t           traffic_class;      // 当前Wire Frame业务类别
    linkg_transport_window_result_t   window_result;      // Peer/Class/数据域接收窗口结果
    linkg_transport_rx_stage_t        stage;              // 当前Packet接收处理阶段

    uint8_t                           peer_node_id;       // 当前物理上一跳直接Peer节点编号
    bool                              fragmented;         // 是否为LinkG内部分片
} linkg_transport_rx_state_t;

/**
 * @brief 当前内部接收Chunk全局异常累计。
 */
typedef struct
{
    uint64_t invalid_frames;      // 非法Transport帧数量
    uint64_t unattributed_frames; // 无法归属直接Peer帧数量
    uint64_t stale_epoch_frames;  // Peer重置期间拒绝的Transport帧数量
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
 * @note 正常高速路径三个计数均为0时不获取g_transport.lock。
 */
static void _linkg_transport_rx_record_batch_stats(const linkg_transport_rx_batch_stats_t *stats)
{
    if (stats == NULL ||
        (stats->invalid_frames == 0U &&
         stats->unattributed_frames == 0U &&
         stats->stale_epoch_frames == 0U))
    {
        return;
    }

    pthread_mutex_lock(&g_transport.lock);

    g_transport.stats.rx_invalid_frames      += stats->invalid_frames;
    g_transport.stats.rx_unattributed_frames += stats->unattributed_frames;
    g_transport.stats.rx_stale_epoch_frames  += stats->stale_epoch_frames;

    pthread_mutex_unlock(&g_transport.lock);
}

/****************************** Reorder READY完成记录 ******************************/

/**
 * @brief 保存READY对应的Peer/Class完成标记，不额外持有Packet引用。
 *
 * @note READY实际交付仍属于当前linkg_transport_receive_batch()调用；
 *       帧内Packet指针仅用于复制描述符，完成操作不解引用Packet。
 */
static void _linkg_transport_rx_record_reorder_ready(linkg_transport_rx_reorder_ready_t *pending, const linkg_transport_reorder_frame_t *frame)
{
    const linkg_transport_reorder_frame_t *saved;
    uint32_t                               index;

    for (index = 0U; index < pending->count; index++)
    {
        saved = &pending->frames[index];

        if (saved->peer == frame->peer &&
            saved->traffic_class == frame->traffic_class)
        {
            return;
        }
    }

    // 一个RX调用最多有LINKG_TRANSPORT_PEER_MAX * CLASS_COUNT种独立交付域。
    if (pending->count >= LINKG_TRANSPORT_RX_REORDER_READY_MAX)
    {
        LINKG_LOG_ERROR("transport RX reorder READY tracking overflow, peer=%u, class=%u",
                        frame->peer_node_id, (unsigned int)frame->traffic_class);
        return;
    }

    pending->frames[pending->count++] = *frame;
}

/**
 * @brief 当前RX调用已完成Handler交付，解除所有READY的Peer/Class交付占用。
 */
static void _linkg_transport_rx_complete_reorder_ready(const linkg_transport_rx_reorder_ready_t *pending)
{
    linkg_transport_reorder_result_t results[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    uint32_t                         batch_count;
    uint32_t                         offset;
    uint32_t                         index;
    int                              ret;

    for (index = 0U; index < LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX; index++)
    {
        results[index] = LINKG_TRANSPORT_REORDER_RESULT_READY;
    }

    offset = 0U;

    while (offset < pending->count)
    {
        batch_count = pending->count - offset;

        if (batch_count > LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX)
        {
            batch_count = LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX;
        }

        ret = linkg_transport_reorder_complete_batch(&pending->frames[offset], results, batch_count);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("transport RX reorder READY completion failed, count=%u, error=%d", batch_count, ret);
        }

        offset += batch_count;
    }
}

/**
 * @brief 为尚未初始化的RX Window选择当前Chunk内最早的Sequence。
 *
 * @note 只处理同一Peer/Class及相同USER_DATA/CONTROL序列域。
 *       已经初始化的接收窗口不受影响。
 *       调用方必须持有g_transport.lock。
 */
static void _linkg_transport_rx_prepare_window_start_locked(linkg_transport_rx_window_t *window, const linkg_transport_rx_state_t *states, uint32_t count, uint32_t first_index, linkg_transport_peer_t *peer, linkg_transport_peer_t *const *group_peers, uint32_t group_count)
{
    const linkg_transport_rx_state_t *first;
    const linkg_transport_rx_state_t *candidate;
    uint32_t                          original_sequence;
    uint32_t                          earliest_sequence;
    uint32_t                          candidate_sequence;
    uint32_t                          group_index;
    uint32_t                          index;
    bool                              user_data;

    if (window->initialized)
    {
        return;
    }

    first             = &states[first_index];
    original_sequence = first->header.sequence;
    earliest_sequence = original_sequence;
    user_data         = first->header.type == LINKG_TRANSPORT_TYPE_USER_DATA;

    // 只扫描同一Peer/Class、相同接收序列域的有效帧。
    for (index = 0U; index < count; index++)
    {
        candidate = &states[index];

        if (candidate->stage != LINKG_TRANSPORT_RX_STAGE_DECODED ||
            candidate->traffic_class != first->traffic_class ||
            (candidate->header.type == LINKG_TRANSPORT_TYPE_USER_DATA) != user_data)
        {
            continue;
        }

        group_index = candidate->source_group_index;

        if (group_index >= group_count || group_peers[group_index] != peer)
        {
            continue;
        }

        candidate_sequence = candidate->header.sequence;

        if (_linkg_transport_rx_sequence_before(candidate_sequence, earliest_sequence) &&
            (original_sequence - candidate_sequence) < LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            earliest_sequence = candidate_sequence;
        }
    }

    if (earliest_sequence == original_sequence)
    {
        return;
    }

    // 预设最早Sequence之前的位置，后续仍调用原RX Window去重接口。
    memset(window->received_bitmap, 0, sizeof(window->received_bitmap));

    window->highest_sequence = earliest_sequence - 1U;
    window->tracked_count    = 0U;
    window->initialized      = true;
}

/****************************** 本机交付 ******************************/

/**
 * @brief 同步交付指定Transport类型的本机Payload批次。
 *
 * @note Handler调用期间Packet均为借用引用；重组Packet由Transport在Handler返回后释放。
 */
static void _linkg_transport_rx_flush_delivery(linkg_transport_type_t type, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_handler_func_t   handler;
    void                            *handler_user_data;
    linkg_transport_rx_batch_stats_t stats;
    uint32_t                         output_count;
    uint32_t                         index;
    int                              ret;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    // 在真正交给上层Handler之前复核代际，剔除窗口接受后已被Reset的旧Peer数据。
    memset(&stats, 0, sizeof(stats));
    output_count = 0U;

    for (index = 0U; index < batch->count; index++)
    {
        if (!linkg_transport_peer_epoch_matches(batch->peers[index], batch->peer_epochs[index]))
        {
            stats.stale_epoch_frames++;

            if (batch->owned[index] && batch->items[index].packet != NULL)
            {
                linkg_packet_release(batch->items[index].packet);
            }

            continue;
        }

        if (output_count != index)
        {
            batch->items[output_count]       = batch->items[index];
            batch->peers[output_count]       = batch->peers[index];
            batch->peer_epochs[output_count] = batch->peer_epochs[index];
            batch->owned[output_count]       = batch->owned[index];
        }

        output_count++;
    }

    batch->count = output_count;
    _linkg_transport_rx_record_batch_stats(&stats);

    if (batch->count == 0U)
    {
        memset(batch, 0, sizeof(*batch));
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
static void _linkg_transport_rx_append_delivery(uint32_t ingress_link_id, linkg_transport_type_t type, linkg_transport_class_t traffic_class, uint8_t source_node_id, uint8_t peer_node_id, linkg_transport_peer_t *peer, uint32_t peer_epoch, linkg_packet_t *packet, bool owned, linkg_transport_delivery_batch_t *batch)
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

    delivery->packet          = packet;
    delivery->ingress_link_id = ingress_link_id;
    delivery->traffic_class   = traffic_class;
    delivery->source_node_id  = source_node_id;
    delivery->peer_node_id    = peer_node_id;

    batch->peers[batch->count]       = peer;
    batch->peer_epochs[batch->count] = peer_epoch;
    batch->owned[batch->count]       = owned;
    batch->count++;
}

/**
 * @brief 去除普通Transport基础头并加入本机类型批次。
 */
static int _linkg_transport_rx_append_normal_delivery(const linkg_transport_rx_state_t *state, linkg_link_rx_item_t *item, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_type_t type;

    if (state == NULL || state->ingress_link_id == LINKG_LINK_ID_INVALID || item == NULL || item->packet == NULL || batch == NULL)
    {
        return -EINVAL;
    }

    if (linkg_packet_pull(item->packet, LINKG_TRANSPORT_WIRE_HEADER_SIZE) == NULL)
    {
        return -EPROTO;
    }

    type = (linkg_transport_type_t)state->header.type;

    _linkg_transport_rx_append_delivery(state->ingress_link_id, type, state->traffic_class, state->header.source_node_id, state->peer_node_id, state->peer, state->peer_epoch, item->packet, false, batch);

    return 0;
}

/**
 * @brief 将完整重组Payload加入本机类型批次。
 *
 * @note Transport允许不同物理Link的分片共同完成重组，
 *       因此完整重组Payload不存在唯一ingress_link_id。
 */
static void _linkg_transport_rx_append_reassembled_delivery(const linkg_transport_rx_state_t *state, linkg_packet_t *packet, linkg_transport_delivery_batch_t *batch)
{
    linkg_transport_type_t type;

    if (state == NULL || packet == NULL || batch == NULL)
    {
        return;
    }

    type = (linkg_transport_type_t)state->header.type;

    _linkg_transport_rx_append_delivery(LINKG_LINK_ID_INVALID, type, state->traffic_class, state->header.source_node_id, state->peer_node_id, state->peer, state->peer_epoch, packet, true, batch);
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
    linkg_transport_rx_batch_stats_t       stats;
    uint32_t                               output_count;
    uint32_t                               index;
    int                                    ret;

    if (batch == NULL || batch->count == 0U)
    {
        return;
    }

    // Forward可能跨越多个RX Chunk，此处必须以入站Peer代际再次过滤旧帧。
    memset(&stats, 0, sizeof(stats));
    output_count = 0U;

    for (index = 0U; index < batch->count; index++)
    {
        if (!linkg_transport_peer_epoch_matches(batch->peers[index], batch->peer_epochs[index]))
        {
            stats.stale_epoch_frames++;

            if (batch->owned[index] && batch->items[index].packet != NULL)
            {
                linkg_packet_release(batch->items[index].packet);
            }

            continue;
        }

        if (output_count != index)
        {
            batch->items[output_count]       = batch->items[index];
            batch->peers[output_count]       = batch->peers[index];
            batch->peer_epochs[output_count] = batch->peer_epochs[index];
            batch->owned[output_count]       = batch->owned[index];
        }

        output_count++;
    }

    batch->count = output_count;
    _linkg_transport_rx_record_batch_stats(&stats);

    if (batch->count == 0U)
    {
        memset(batch, 0, sizeof(*batch));
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

    batch->peers[batch->count]       = state->peer;
    batch->peer_epochs[batch->count] = state->peer_epoch;
    batch->owned[batch->count]       = false;
    batch->count++;
}

/**
 * @brief 将完整配对的FIRST/LAST连续加入指定业务Class中继批次。
 *
 * @note Pair输出的两个Packet引用均由当前Forward Batch接管。
 */
static void _linkg_transport_rx_append_forward_pair(linkg_transport_class_t traffic_class, const linkg_transport_forward_item_t *items, linkg_transport_peer_t *peer, uint32_t peer_epoch, linkg_transport_forward_batch_t *batch)
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
        batch->items[batch->count]       = items[index];
        batch->peers[batch->count]       = peer;
        batch->peer_epochs[batch->count] = peer_epoch;
        batch->owned[batch->count]       = true;
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
 * @brief 累计并限频输出物理来源分组溢出诊断。
 */
static void _linkg_transport_rx_record_source_group_overflow(uint32_t group_count)
{
    static _Atomic uint64_t next_log_ms;
    static _Atomic uint64_t overflow_count;
    uint64_t expected_log_ms;
    uint64_t now_ms;
    uint64_t count;

    atomic_fetch_add_explicit(&overflow_count, 1U, memory_order_relaxed);

    now_ms          = linkg_time_elapsed_ms();
    expected_log_ms = atomic_load_explicit(&next_log_ms, memory_order_relaxed);

    if (now_ms < expected_log_ms)
    {
        return;
    }

    if (!atomic_compare_exchange_strong_explicit(&next_log_ms,
                                                 &expected_log_ms,
                                                 now_ms + LINKG_TRANSPORT_RX_SOURCE_GROUP_LOG_INTERVAL_MS,
                                                 memory_order_relaxed,
                                                 memory_order_relaxed))
    {
        return;
    }

    count = atomic_exchange_explicit(&overflow_count, 0U, memory_order_relaxed);

    LINKG_LOG_DEBUG("transport RX source group overflow, frames=%llu, group_count=%u, max=%u",
                    (unsigned long long)count,
                    group_count,
                    LINKG_RESOURCE_NETWORK_STA_MAX);
}

/**
 * @brief 初始化当前RX批次状态并按物理来源端点建立分组。
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
        states[index].stage              = LINKG_TRANSPORT_RX_STAGE_INVALID;

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
                _linkg_transport_rx_record_source_group_overflow(current_group_count);
                stats->unattributed_frames++;
                states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
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
 * @brief 批量解析Transport基础头、分片头及Payload长度。
 *
 * @note 仅处理已成功归属物理来源的Packet，全部协议解析成功后设置DECODED状态。
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

        // 检查Packet及物理来源归属。
        if (packet == NULL ||
            group_index == LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID ||
            !groups[group_index].resolved)
        {
            states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            continue;
        }

        // 继承物理来源对应的业务Class和Peer信息。
        states[index].traffic_class = _linkg_transport_rx_packet_class(packet);
        states[index].peer_node_id  = groups[group_index].peer_node_id;

        // 解析Transport基础头。
        ret = linkg_transport_wire_decode(packet, &states[index].header);
        if (ret != 0)
        {
            stats->invalid_frames++;
            states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            continue;
        }

        // 根据基础头标记解析分片信息。
        states[index].fragmented = (states[index].header.flags & LINKG_TRANSPORT_FLAG_FRAGMENT) != 0U;

        if (states[index].fragmented)
        {
            ret = linkg_transport_wire_fragment_decode(packet, &states[index].fragment_header);
            if (ret != 0)
            {
                stats->invalid_frames++;
                states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                continue;
            }

            states[index].payload_length = packet->data_length - LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE;
        }
        else
        {
            states[index].payload_length = packet->data_length - LINKG_TRANSPORT_WIRE_HEADER_SIZE;
        }

        // 所有协议解析成功，允许进入接收窗口处理。
        states[index].stage = LINKG_TRANSPORT_RX_STAGE_DECODED;
    }
}

/****************************** 接收窗口 ******************************/

/**
 * @brief 批量确认Peer生命周期并执行Transport接收窗口去重。
 */
static int _linkg_transport_rx_accept_batch(linkg_transport_rx_state_t *states, uint32_t count, const linkg_transport_rx_source_group_t *groups, uint32_t group_count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_peer_t       *group_peers[LINKG_RESOURCE_NETWORK_STA_MAX];
    uint32_t                      group_peer_epochs[LINKG_RESOURCE_NETWORK_STA_MAX];
    linkg_transport_peer_class_t *peer_class;
    linkg_transport_rx_window_t  *rx_window;
    linkg_transport_peer_t       *peer;
    uint64_t                      now_ms;
    uint32_t                      confirmed_lost;
    uint32_t                      group_index;
    uint32_t                      index;
    bool                          group_quiescing[LINKG_RESOURCE_NETWORK_STA_MAX];
    bool                          out_of_order;
    int                           ret;
    bool                          has_decoded;

    // 检查来源分组数量是否合法。
    if (group_count > LINKG_RESOURCE_NETWORK_STA_MAX)
    {
        return -EINVAL;
    }

    // 检查当前批次是否存在需要进行窗口处理的Packet。
    has_decoded = false;

    for (index = 0U; index < count; index++)
    {
        if (states[index].stage == LINKG_TRANSPORT_RX_STAGE_DECODED)
        {
            has_decoded = true;
        }
    }

    if (!has_decoded)
    {
        return 0;
    }

    memset(group_peers, 0, sizeof(group_peers));
    memset(group_peer_epochs, 0, sizeof(group_peer_epochs));
    memset(group_quiescing, 0, sizeof(group_quiescing));

    // 获取Transport状态锁，保护Peer和RX Window。
    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    // 批量解析各来源Group对应的有效Peer。
    for (group_index = 0U; group_index < group_count; group_index++)
    {
        if (!groups[group_index].resolved)
        {
            continue;
        }

        group_peers[group_index] = linkg_transport_find_peer_locked(groups[group_index].peer_node_id);

        if (group_peers[group_index] != NULL && !linkg_transport_peer_epoch_read_active(group_peers[group_index], &group_peer_epochs[group_index]))
        {
            group_peers[group_index]     = NULL;
            group_quiescing[group_index] = true;
        }
    }

    now_ms = linkg_time_elapsed_ms();

    // 逐包确认Peer、选择接收窗口并执行序列号去重。
    for (index = 0U; index < count; index++)
    {
        if (states[index].stage != LINKG_TRANSPORT_RX_STAGE_DECODED)
        {
            continue;
        }

        states[index].window_result = LINKG_TRANSPORT_WINDOW_INVALID;

        group_index = states[index].source_group_index;

        if (group_index == LINKG_TRANSPORT_RX_SOURCE_GROUP_INVALID ||
            group_index >= group_count ||
            !groups[group_index].resolved)
        {
            stats->unattributed_frames++;
            states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            continue;
        }

        peer = group_peers[group_index];

        if (peer == NULL)
        {
            if (group_quiescing[group_index])
            {
                stats->stale_epoch_frames++;
            }
            else
            {
                stats->unattributed_frames++;
            }

            states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            continue;
        }

        if ((uint32_t)states[index].traffic_class >= LINKG_TRANSPORT_CLASS_COUNT)
        {
            stats->invalid_frames++;
            states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            continue;
        }

        states[index].peer       = peer;
        states[index].peer_epoch = group_peer_epochs[group_index];

        peer_class = &peer->classes[states[index].traffic_class];

        // USER_DATA和CONTROL使用独立接收窗口。
        if (states[index].header.type == LINKG_TRANSPORT_TYPE_USER_DATA)
        {
            rx_window = &peer_class->rx_user_window;
        }
        else
        {
            rx_window = &peer_class->rx_control_window;
        }

        // 首次接收时，允许同一Chunk内的有效乱序帧共同建立基线。
        _linkg_transport_rx_prepare_window_start_locked(rx_window, states, count, index, peer, group_peers, group_count);

        // 记录当前序列号是否早于接收窗口最高序列号。
        out_of_order = rx_window->initialized && _linkg_transport_rx_sequence_before(states[index].header.sequence, rx_window->highest_sequence);

        confirmed_lost = 0U;

        // 执行接收窗口判定。
        states[index].window_result = linkg_transport_window_accept_ex(rx_window, states[index].header.sequence, &confirmed_lost);

        peer_class->stats.rx_lost_packets += confirmed_lost;

        // 根据窗口结果更新Packet状态及Peer统计。
        switch (states[index].window_result)
        {
            case LINKG_TRANSPORT_WINDOW_ACCEPT:
            {
                states[index].stage = LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED;

                peer_class->stats.rx_packets++;
                peer_class->stats.rx_bytes += states[index].payload_length;
                peer_class->stats.last_rx_ms = now_ms;

                if (states[index].header.type == LINKG_TRANSPORT_TYPE_USER_DATA)
                {
                    peer_class->stats.last_user_rx_ms = now_ms;
                }

                if (out_of_order)
                {
                    peer_class->stats.rx_out_of_order_packets++;
                }

                break;
            }

            case LINKG_TRANSPORT_WINDOW_DUPLICATE:
            {
                states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                peer_class->stats.rx_duplicate_packets++;
                break;
            }

            case LINKG_TRANSPORT_WINDOW_TOO_OLD:
            {
                states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                break;
            }

            case LINKG_TRANSPORT_WINDOW_INVALID:
            default:
            {
                states[index].stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                break;
            }
        }
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 分片重组 ******************************/

/**
 * @brief 批量重组目标为本机的Transport分片。
 *
 * @note USER_DATA必须经过Reorder，其他Transport类型保留RX Window接收路径。
 *       重组结果统一转换为RX Stage，不在RX状态中保存底层返回值。
 */
static void _linkg_transport_rx_reassemble_batch(linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_reassembly_submit_item_t reassembly_items[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    uint32_t                                 state_indices[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_rx_state_t              *state;
    uint32_t                                 reassembly_count;
    uint32_t                                 state_index;
    uint32_t                                 index;
    int                                      ret;

    reassembly_count = 0U;

    // 收集已经通过接收顺序管理、目标为本机的分片。
    for (index = 0U; index < count; index++)
    {
        state = &states[index];

        if (state->stage != LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED &&
            state->stage != LINKG_TRANSPORT_RX_STAGE_REORDER_READY)
        {
            continue;
        }

        if (!state->fragmented ||
            state->header.destination_node_id != g_transport.local_node_id)
        {
            continue;
        }

        reassembly_items[reassembly_count].header           = &state->header;
        reassembly_items[reassembly_count].fragment_header  = &state->fragment_header;
        reassembly_items[reassembly_count].packet           = items[index].packet;
        reassembly_items[reassembly_count].completed_packet = NULL;
        reassembly_items[reassembly_count].peer             = state->peer;
        reassembly_items[reassembly_count].peer_epoch       = state->peer_epoch;
        reassembly_items[reassembly_count].traffic_class    = state->traffic_class;
        reassembly_items[reassembly_count].result           = -EINPROGRESS;
        reassembly_items[reassembly_count].peer_node_id     = state->peer_node_id;

        state_indices[reassembly_count] = index;
        reassembly_count++;
    }

    if (reassembly_count == 0U)
    {
        return;
    }

    // 批量提交分片重组。
    ret = linkg_transport_reassembly_submit_batch(reassembly_items, reassembly_count);
    if (ret != 0)
    {
        // 批次级失败，所有提交帧终止后续处理。
        for (index = 0U; index < reassembly_count; index++)
        {
            state_index = state_indices[index];
            state       = &states[state_index];

            state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;

            if (reassembly_items[index].completed_packet != NULL)
            {
                linkg_packet_release(reassembly_items[index].completed_packet);
            }

            if (ret == -ESTALE)
            {
                stats->stale_epoch_frames++;
            }
            else
            {
                stats->invalid_frames++;
            }
        }

        return;
    }

    // 将逐帧重组结果统一转换为RX Stage。
    for (index = 0U; index < reassembly_count; index++)
    {
        state_index = state_indices[index];
        state       = &states[state_index];

        if (reassembly_items[index].result == 0 &&
            reassembly_items[index].completed_packet == NULL)
        {
            // 分片已接受，但逻辑Packet尚未完整。
            state->stage = LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_PENDING;
            continue;
        }

        if (reassembly_items[index].result == 1 &&
            reassembly_items[index].completed_packet != NULL)
        {
            // 重组完成，完整Packet交由后续分发阶段接管。
            state->completed_packet = reassembly_items[index].completed_packet;
            state->stage            = LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_READY;
            continue;
        }

        // 重组失败或结果不一致。
        state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;

        if (reassembly_items[index].completed_packet != NULL)
        {
            linkg_packet_release(reassembly_items[index].completed_packet);
        }

        if (reassembly_items[index].result == -ESTALE)
        {
            stats->stale_epoch_frames++;
        }
        else
        {
            stats->invalid_frames++;
        }
    }
}

/****************************** 数据分发 ******************************/

/****************************** 数据分发 ******************************/

/**
 * @brief 根据RX Stage分发已就绪Transport数据。
 *
 * @note 本机数据按Transport Type交付，中继数据仅由AP按业务Class转发。
 *       USER_DATA必须经过Reorder，已完成的本机分片通过completed_packet交付。
 */
static void _linkg_transport_rx_dispatch_batch(linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_delivery_batch_t *delivery_batches, linkg_transport_forward_batch_t *forward_batches, linkg_transport_rx_batch_stats_t *stats)
{
    linkg_transport_forward_item_t pair_items[LINKG_TRANSPORT_FRAGMENT_COUNT_MAX];
    linkg_transport_rx_state_t    *state;
    linkg_transport_type_t         type;
    uint32_t                       pair_count;
    uint32_t                       pair_index;
    uint32_t                       index;
    int                            ret;

    for (index = 0U; index < count; index++)
    {
        state = &states[index];

        // 第一阶段：只有完成前置处理的数据才能进入分发。
        if (state->stage != LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED &&
            state->stage != LINKG_TRANSPORT_RX_STAGE_REORDER_READY &&
            state->stage != LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_READY)
        {
            continue;
        }

        // 第二阶段：确认Peer生命周期，禁止旧会话继续交付。
        if (!linkg_transport_peer_epoch_matches(state->peer, state->peer_epoch))
        {
            state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            stats->stale_epoch_frames++;

            if (state->completed_packet != NULL)
            {
                linkg_packet_release(state->completed_packet);
                state->completed_packet = NULL;
            }

            continue;
        }

        type = (linkg_transport_type_t)state->header.type;

        // 第三阶段：目标为本机，执行本机Transport交付。
        if (state->header.destination_node_id == g_transport.local_node_id)
        {
            // 分支一：已完成重组的本机分片。
            if (state->fragmented)
            {
                if (state->stage != LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_READY ||
                    state->completed_packet == NULL)
                {
                    state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                    stats->invalid_frames++;
                    continue;
                }

                // 完整Packet引用转交Delivery Batch管理。
                _linkg_transport_rx_append_reassembled_delivery(state, state->completed_packet, &delivery_batches[type]);

                state->completed_packet = NULL;
                state->stage            = LINKG_TRANSPORT_RX_STAGE_DISPATCH_READY;

                continue;
            }

            // 分支二：非分片USER_DATA必须已经通过Reorder。
            if (type == LINKG_TRANSPORT_TYPE_USER_DATA)
            {
                if (state->stage != LINKG_TRANSPORT_RX_STAGE_REORDER_READY)
                {
                    state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                    stats->invalid_frames++;
                    continue;
                }
            }
            else
            {
                // 其他Transport类型沿用RX Window接收路径。
                if (state->stage != LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED)
                {
                    state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                    stats->invalid_frames++;
                    continue;
                }
            }

            ret = _linkg_transport_rx_append_normal_delivery(state, &items[index], &delivery_batches[type]);
            if (ret != 0)
            {
                state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                stats->invalid_frames++;
                continue;
            }

            state->stage = LINKG_TRANSPORT_RX_STAGE_DISPATCH_READY;
            continue;
        }

        // 第四阶段：非本机数据仅允许AP执行中继。
        if (g_transport.local_role != LINKG_DEVICE_ROLE_AP ||
            forward_batches == NULL)
        {
            state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            continue;
        }

        // 中继数据不经过本机Reorder和Reassembly。
        if (state->stage != LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED)
        {
            state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            stats->invalid_frames++;
            continue;
        }

        // 分支三：非分片数据直接加入Class中继批次。
        if (!state->fragmented)
        {
            _linkg_transport_rx_append_forward(state, &items[index], &forward_batches[state->traffic_class]);

            state->stage = LINKG_TRANSPORT_RX_STAGE_DISPATCH_READY;
            continue;
        }

        // 分支四：分片中继沿用FIRST/LAST配对机制。
        memset(pair_items, 0, sizeof(pair_items));
        pair_count = 0U;

        ret = linkg_transport_forward_pair_submit(state->peer, state->peer_epoch, state->traffic_class, state->peer_node_id, &state->header, &state->fragment_header, items[index].packet, state->payload_length, pair_items, &pair_count);
        if (ret != 0)
        {
            for (pair_index = 0U; pair_index < pair_count; pair_index++)
            {
                if (pair_items[pair_index].packet != NULL)
                {
                    linkg_packet_release(pair_items[pair_index].packet);
                }
            }

            state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;

            if (ret == -ESTALE)
            {
                stats->stale_epoch_frames++;
            }
            else if (ret == -EPROTO || ret == -EINVAL)
            {
                stats->invalid_frames++;
            }
            else
            {
                LINKG_LOG_ERROR("transport forward fragment pair failed, class=%d, peer=%u, error=%d",
                                (int)state->traffic_class, state->peer_node_id, ret);
            }

            continue;
        }

        // FIRST/LAST尚未配对完成，等待后续分片。
        if (pair_count == 0U)
        {
            state->stage = LINKG_TRANSPORT_RX_STAGE_REASSEMBLY_PENDING;
            continue;
        }

        // 中继配对必须产生完整的固定两片。
        if (pair_count != LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)
        {
            for (pair_index = 0U; pair_index < pair_count; pair_index++)
            {
                if (pair_items[pair_index].packet != NULL)
                {
                    linkg_packet_release(pair_items[pair_index].packet);
                }
            }

            state->stage = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            stats->invalid_frames++;
            continue;
        }

        // 完整FIRST/LAST配对加入Class中继批次。
        _linkg_transport_rx_append_forward_pair(state->traffic_class, pair_items, state->peer, state->peer_epoch, &forward_batches[state->traffic_class]);

        state->stage = LINKG_TRANSPORT_RX_STAGE_DISPATCH_READY;
    }
}

/****************************** Reorder交付 ******************************/

/**
 * @brief Reorder同步交付已就绪USER_DATA Wire Frame。
 *
 * @note 可能由当前RX线程补齐缺口后同步调用，也可能由Reorder Worker调用。
 *       输入Packet引用由Reorder持有；本函数不释放输入Wire Packet。
 */
void linkg_transport_rx_emit_reordered(const linkg_transport_reorder_frame_t *frames, uint32_t count)
{
    linkg_transport_delivery_batch_t delivery_batches[LINKG_TRANSPORT_TYPE_COUNT];
    linkg_transport_rx_state_t       states[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_link_rx_item_t             items[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_rx_batch_stats_t stats;
    uint32_t                         index;

    if (frames == NULL || count == 0U || count > LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE)
    {
        return;
    }

    memset(delivery_batches, 0, sizeof(delivery_batches));
    memset(states, 0, sizeof(states));
    memset(items, 0, sizeof(items));
    memset(&stats, 0, sizeof(stats));

    // 默认全部不可进入后续阶段，逐帧核验后才设置就绪。
    for (index = 0U; index < count; index++)
    {
        states[index].stage         = LINKG_TRANSPORT_RX_STAGE_DROPPED;
        states[index].window_result = LINKG_TRANSPORT_WINDOW_INVALID;
    }

    for (index = 0U; index < count; index++)
    {
        if (frames[index].packet == NULL || frames[index].peer == NULL)
        {
            stats.invalid_frames++;
            continue;
        }

        if (!linkg_transport_peer_epoch_matches(frames[index].peer, frames[index].peer_epoch))
        {
            stats.stale_epoch_frames++;
            continue;
        }

        items[index].packet             = frames[index].packet;

        states[index].header            = frames[index].header;
        states[index].fragment_header   = frames[index].fragment_header;
        states[index].peer              = frames[index].peer;
        states[index].peer_epoch        = frames[index].peer_epoch;
        states[index].peer_node_id      = frames[index].peer_node_id;
        states[index].ingress_link_id   = frames[index].ingress_link_id;
        states[index].traffic_class     = frames[index].traffic_class;
        states[index].fragmented        = (frames[index].header.flags & LINKG_TRANSPORT_FLAG_FRAGMENT) != 0U;
        states[index].window_result     = LINKG_TRANSPORT_WINDOW_ACCEPT;
        states[index].stage             = LINKG_TRANSPORT_RX_STAGE_REORDER_READY;
    }

    // Reorder已经管理Sequence，此处只执行原有分片重组和本机分发。
    _linkg_transport_rx_reassemble_batch(items, states, count, &stats);
    _linkg_transport_rx_dispatch_batch(items, states, count, delivery_batches, NULL, &stats);
    _linkg_transport_rx_flush_deliveries(delivery_batches);
    _linkg_transport_rx_record_batch_stats(&stats);
}

/**
 * @brief 将通过接收窗口的本机USER_DATA批量提交Reorder。
 *
 * @note READY保留原RX快速交付，CACHED和EMITTED禁止重复进入下游。
 *       READY完成标记由最外层RX在Handler返回后统一确认。
 */
static void _linkg_transport_rx_stage_user_reorder(uint32_t link_id, linkg_link_rx_item_t *items, linkg_transport_rx_state_t *states, uint32_t count, linkg_transport_rx_reorder_ready_t *pending)
{
    linkg_transport_reorder_frame_t  frames[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    linkg_transport_reorder_result_t results[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    uint32_t                         state_indices[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    linkg_transport_rx_state_t      *state;
    uint32_t                         frame_count;
    uint32_t                         index;
    int                              ret;

    frame_count = 0U;

    // 每次调用仅处理一个Transport RX Chunk，Chunk容量不超过32。
    for (index = 0U; index < count; index++)
    {
        state = &states[index];

        if (state->stage != LINKG_TRANSPORT_RX_STAGE_WINDOW_ACCEPTED ||
            state->window_result != LINKG_TRANSPORT_WINDOW_ACCEPT ||
            state->header.type != LINKG_TRANSPORT_TYPE_USER_DATA ||
            state->header.destination_node_id != g_transport.local_node_id)
        {
            continue;
        }

        frames[frame_count].packet          = items[index].packet;
        frames[frame_count].peer            = state->peer;
        frames[frame_count].header          = state->header;
        frames[frame_count].fragment_header = state->fragment_header;
        frames[frame_count].ingress_link_id = link_id;
        frames[frame_count].peer_epoch      = state->peer_epoch;
        frames[frame_count].traffic_class   = state->traffic_class;
        frames[frame_count].peer_node_id    = state->peer_node_id;

        state_indices[frame_count] = index;
        frame_count++;
    }

    if (frame_count == 0U)
    {
        return;
    }

    ret = linkg_transport_reorder_submit_batch(frames, frame_count, results);
    if (ret != 0)
    {
        for (index = 0U; index < frame_count; index++)
        {
            state                = &states[state_indices[index]];
            state->stage         = LINKG_TRANSPORT_RX_STAGE_DROPPED;
            state->window_result = LINKG_TRANSPORT_WINDOW_INVALID;
        }

        LINKG_LOG_ERROR("transport reorder submit batch failed, count=%u, error=%d", frame_count, ret);
        return;
    }

    // stage与window_result必须同时更新，适配现有下游接收条件。
    for (index = 0U; index < frame_count; index++)
    {
        state = &states[state_indices[index]];

        switch (results[index])
        {
            case LINKG_TRANSPORT_REORDER_RESULT_READY:
            {
                state->stage         = LINKG_TRANSPORT_RX_STAGE_REORDER_READY;
                state->window_result = LINKG_TRANSPORT_WINDOW_ACCEPT;
                _linkg_transport_rx_record_reorder_ready(pending, &frames[index]);
                break;
            }

            case LINKG_TRANSPORT_REORDER_RESULT_CACHED:
            {
                state->stage         = LINKG_TRANSPORT_RX_STAGE_REORDER_PENDING;
                state->window_result = LINKG_TRANSPORT_WINDOW_INVALID;
                break;
            }

            case LINKG_TRANSPORT_REORDER_RESULT_EMITTED:
            {
                state->stage         = LINKG_TRANSPORT_RX_STAGE_EMITTED;
                state->window_result = LINKG_TRANSPORT_WINDOW_INVALID;
                break;
            }

            case LINKG_TRANSPORT_REORDER_RESULT_TOO_OLD:
            case LINKG_TRANSPORT_REORDER_RESULT_STALE:
            case LINKG_TRANSPORT_REORDER_RESULT_OUT_OF_WINDOW:
            {
                state->stage         = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                state->window_result = LINKG_TRANSPORT_WINDOW_INVALID;
                break;
            }

            case LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED:
            default:
            {
                state->stage         = LINKG_TRANSPORT_RX_STAGE_DROPPED;
                state->window_result = LINKG_TRANSPORT_WINDOW_INVALID;

                LINKG_LOG_ERROR("transport reorder invalid result, peer=%u, class=%u, seq=%u, result=%d",
                                frames[index].peer_node_id,
                                (unsigned int)frames[index].traffic_class,
                                frames[index].header.sequence,
                                (int)results[index]);
                break;
            }
        }
    }
}

/****************************** 内部批次 ******************************/

/**
 * @brief 处理一个Transport内部接收Chunk。
 *
 * @note 流水线固定为Path归属、协议解析、Peer/Class窗口、本机重组和本机/中继分流。
 */
static void _linkg_transport_rx_process_chunk(uint32_t link_id, linkg_link_rx_item_t *items, uint32_t count, linkg_transport_delivery_batch_t *delivery_batches, linkg_transport_forward_batch_t *forward_batches, linkg_transport_rx_reorder_ready_t *pending)
{
    linkg_transport_rx_source_group_t groups[LINKG_RESOURCE_NETWORK_STA_MAX];
    linkg_transport_rx_state_t        states[LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE];
    linkg_transport_rx_batch_stats_t  stats;
    uint32_t                          group_count;
    uint32_t                          index;
    int                               ret;

    memset(groups, 0, sizeof(groups));
    memset(states, 0, sizeof(states));
    memset(&stats, 0, sizeof(stats));

    group_count = 0U;

    _linkg_transport_rx_build_source_groups(items, states, count, groups, &group_count, &stats);
    _linkg_transport_rx_resolve_source_groups(link_id, groups, group_count, &stats);
    _linkg_transport_rx_decode_batch(items, states, count, groups, &stats);

    for (index = 0U; index < count; index++)
    {
        states[index].ingress_link_id = link_id;
    }

    ret = _linkg_transport_rx_accept_batch(states, count, groups, group_count, &stats);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("transport RX accept batch failed, count=%u, error=%d", count, ret);
        _linkg_transport_rx_record_batch_stats(&stats);
        return;
    }

    _linkg_transport_rx_stage_user_reorder(link_id, items, states, count, pending);
    _linkg_transport_rx_reassemble_batch(items, states, count, &stats);
    _linkg_transport_rx_dispatch_batch(items, states, count, delivery_batches, forward_batches, &stats);
    _linkg_transport_rx_record_batch_stats(&stats);
}

/****************************** 数据接收 ******************************/

/**
 * @brief Link Base统一批量接收回调。
 *
 * @note USER_DATA由Reorder按Peer/Class统一处理Sequence。
 *       READY在当前RX Handler完成交付后才解除Reorder交付占用。
 */
void linkg_transport_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t count, void *user_data)
{
    linkg_transport_delivery_batch_t   delivery_batches[LINKG_TRANSPORT_TYPE_COUNT];
    linkg_transport_forward_batch_t    forward_batches[LINKG_TRANSPORT_CLASS_COUNT];
    linkg_transport_rx_reorder_ready_t pending;
    uint32_t                           chunk_count;
    uint32_t                           link_id;
    uint32_t                           offset;

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
    memset(&pending, 0, sizeof(pending));

    offset = 0U;

    // 按固定Chunk执行Transport接收流水线。
    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE)
        {
            chunk_count = LINKG_TRANSPORT_RX_BATCH_CHUNK_SIZE;
        }

        _linkg_transport_rx_process_chunk(link_id, &items[offset], chunk_count, delivery_batches, forward_batches, &pending);

        offset += chunk_count;
    }

    // 当前RX所有READY数据完成本机Handler交付。
    _linkg_transport_rx_flush_deliveries(delivery_batches);

    // Handler返回后，解除对应Peer/Class的交付占用。
    _linkg_transport_rx_complete_reorder_ready(&pending);

    // 中继数据维持原有独立交付流程。
    _linkg_transport_rx_flush_forwards(forward_batches);
}
