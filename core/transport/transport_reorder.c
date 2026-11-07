/**
 * @file transport_reorder.c
 * @brief LinkG本机USER_DATA接收顺序管理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-06
 */

#include "transport_reorder.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_REORDER_THREAD_NAME      "trans-reorder" // Reorder工作线程名称
#define LINKG_TRANSPORT_REORDER_WINDOW_SIZE      64U             // 每Peer/Class独立缓存窗口
#define LINKG_TRANSPORT_REORDER_WINDOW_MASK      63U             // 64槽窗口索引掩码
#define LINKG_TRANSPORT_REORDER_REALTIME_WAIT_US 15000ULL        // REALTIME最大缺口等待时间
#define LINKG_TRANSPORT_REORDER_VIDEO_WAIT_US    60000ULL        // VIDEO最大缺口等待时间
#define LINKG_TRANSPORT_REORDER_DATA_WAIT_US     80000ULL        // DATA最大缺口等待时间

_Static_assert((LINKG_TRANSPORT_REORDER_WINDOW_SIZE & (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U)) == 0U, "reorder window size must be power of two");
_Static_assert(LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX <= UINT8_MAX, "reorder group index must fit uint8_t");

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_transport_reorder_frame_t frame;         // 缓存Wire Frame，Reorder持有独立Packet引用
    uint64_t                        expires_at_us; // 当前缓存帧的最晚释放判定时间
} linkg_transport_reorder_slot_t;

typedef struct
{
    linkg_transport_reorder_slot_t  slots[LINKG_TRANSPORT_REORDER_WINDOW_SIZE]; // 64槽Sequence缓存
    pthread_t                       delivery_thread;                            // 当前交付线程，仅delivery_in_progress时有效
    uint32_t                        expected_sequence;                          // 下一个应处理的Wire Sequence
    uint32_t                        slot_count;                                 // 当前缓存数量
    uint32_t                        peer_epoch;                                 // Peer生命周期代际
    uint8_t                         peer_node_id;                               // 直接Peer节点编号
    bool                            expected_initialized;                       // 接收基线是否有效
    bool                            delivery_in_progress;                       // 同Peer/Class交付是否正在进行
} linkg_transport_reorder_class_t;

typedef struct
{
    uint8_t                 indices[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX]; // 原始输入帧索引
    uint32_t                peer_index;                                        // Transport固定Peer槽位
    uint32_t                count;                                             // 分组内帧数
    linkg_transport_class_t traffic_class;                                     // 单一业务类别
} linkg_transport_reorder_group_t;

typedef struct
{
    pthread_mutex_t                     lock;                                                           // Reorder状态保护锁
    pthread_cond_t                      condition;                                                      // Worker唤醒和交付完成通知
    linkg_thread_t                      thread;                                                         // 单一超时Worker
    linkg_transport_reorder_class_t     classes[LINKG_TRANSPORT_PEER_MAX][LINKG_TRANSPORT_CLASS_COUNT]; // 每Peer/Class独立状态
    linkg_transport_reorder_emit_func_t emit;                                                           // 锁外同步交付回调
    uint32_t                            worker_cursor;                                                  // Worker公平扫描起点
    bool                                initialized;                                                    // 运行资源已初始化
    bool                                running;                                                        // Worker运行许可
} linkg_transport_reorder_runtime_t;

/****************************** 模块状态 ******************************/

static linkg_transport_reorder_runtime_t g_reorder;

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取Peer在Transport固定数组中的槽位索引。
 */
static int _linkg_transport_reorder_peer_index(const linkg_transport_peer_t *peer)
{
    uint32_t peer_index;

    if (peer == NULL)
    {
        return -EINVAL;
    }

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        if (peer == &g_transport.peers[peer_index])
        {
            return (int)peer_index;
        }
    }

    return -EINVAL;
}

/**
 * @brief 判断Sequence是否早于另一个Sequence，允许uint32_t自然回绕。
 */
static bool _linkg_transport_reorder_sequence_before(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

/**
 * @brief 获取当前业务的乱序等待时间。
 */
static uint64_t _linkg_transport_reorder_wait_us(linkg_transport_class_t traffic_class)
{
    switch (traffic_class)
    {
        case LINKG_TRANSPORT_CLASS_REALTIME:
            return LINKG_TRANSPORT_REORDER_REALTIME_WAIT_US;

        case LINKG_TRANSPORT_CLASS_VIDEO:
            return LINKG_TRANSPORT_REORDER_VIDEO_WAIT_US;

        case LINKG_TRANSPORT_CLASS_DATA:
        default:
            return LINKG_TRANSPORT_REORDER_DATA_WAIT_US;
    }
}

/**
 * @brief 释放单个缓存槽持有的Packet引用。
 */
static void _linkg_transport_reorder_clear_slot(linkg_transport_reorder_slot_t *slot)
{
    if (slot->frame.packet != NULL)
    {
        linkg_packet_release(slot->frame.packet);
    }

    memset(slot, 0, sizeof(*slot));
}

/**
 * @brief 清空Peer/Class缓存和全部Sequence状态。
 */
static void _linkg_transport_reorder_clear_class(linkg_transport_reorder_class_t *current)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_REORDER_WINDOW_SIZE; index++)
    {
        _linkg_transport_reorder_clear_slot(&current->slots[index]);
    }

    memset(current, 0, sizeof(*current));
}

/**
 * @brief 按Peer/Class归并当前批次的输入帧索引。
 */
static uint32_t _linkg_transport_reorder_build_groups(const linkg_transport_reorder_frame_t *frames, uint32_t count, const uint32_t *peer_indices, linkg_transport_reorder_group_t *groups)
{
    linkg_transport_reorder_group_t *group;
    uint32_t                        group_count;
    uint32_t                        group_index;
    uint32_t                        index;

    group_count = 0U;

    for (index = 0U; index < count; index++)
    {
        for (group_index = 0U; group_index < group_count; group_index++)
        {
            if (groups[group_index].peer_index == peer_indices[index] &&
                groups[group_index].traffic_class == frames[index].traffic_class)
            {
                break;
            }
        }

        if (group_index == group_count)
        {
            group = &groups[group_count++];
            memset(group, 0, sizeof(*group));

            group->peer_index    = peer_indices[index];
            group->traffic_class = frames[index].traffic_class;
        }

        group = &groups[group_index];
        group->indices[group->count++] = (uint8_t)index;
    }

    return group_count;
}

/**
 * @brief 按Sequence排列分组索引，不移动输入Frame或Packet。
 */
static void _linkg_transport_reorder_sort_group(const linkg_transport_reorder_group_t *group, const linkg_transport_reorder_frame_t *frames, uint8_t *order)
{
    uint32_t position;
    uint32_t compare;
    uint8_t  frame_index;
    bool     sorted;

    memcpy(order, group->indices, group->count * sizeof(order[0]));

    sorted = true;

    for (position = 1U; position < group->count; position++)
    {
        if (_linkg_transport_reorder_sequence_before(frames[order[position]].header.sequence, frames[order[position - 1U]].header.sequence))
        {
            sorted = false;
            break;
        }
    }

    if (sorted)
    {
        return;
    }

    for (position = 1U; position < group->count; position++)
    {
        frame_index = order[position];
        compare     = position;

        while (compare > 0U &&
               _linkg_transport_reorder_sequence_before(frames[frame_index].header.sequence, frames[order[compare - 1U]].header.sequence))
        {
            order[compare] = order[compare - 1U];
            compare--;
        }

        order[compare] = frame_index;
    }
}

/**
 * @brief 校验Peer代际并建立首次Sequence基线。
 */
static bool _linkg_transport_reorder_prepare_group_locked(linkg_transport_reorder_class_t *current, const linkg_transport_reorder_group_t *group, const uint8_t *order, const linkg_transport_reorder_frame_t *frames, linkg_transport_reorder_result_t *results)
{
    const linkg_transport_reorder_frame_t *frame;
    uint32_t                               position;
    uint32_t                               index;
    bool                                   valid;

    valid = false;

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];
        frame = &frames[index];

        if (!linkg_transport_peer_epoch_matches(frame->peer, frame->peer_epoch) ||
            (current->expected_initialized &&
             (current->peer_epoch != frame->peer_epoch || current->peer_node_id != frame->peer_node_id)))
        {
            results[index] = LINKG_TRANSPORT_REORDER_RESULT_STALE;
            continue;
        }

        valid = true;
    }

    if (!valid)
    {
        return false;
    }

    if (current->expected_initialized)
    {
        return true;
    }

    if (current->slot_count != 0U || current->delivery_in_progress)
    {
        for (position = 0U; position < group->count; position++)
        {
            results[order[position]] = LINKG_TRANSPORT_REORDER_RESULT_STALE;
        }

        return false;
    }

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
        {
            continue;
        }

        current->expected_sequence    = frames[index].header.sequence;
        current->peer_epoch           = frames[index].peer_epoch;
        current->peer_node_id         = frames[index].peer_node_id;
        current->expected_initialized = true;
        break;
    }

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] == LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED &&
            (frames[index].peer_epoch != current->peer_epoch || frames[index].peer_node_id != current->peer_node_id))
        {
            results[index] = LINKG_TRANSPORT_REORDER_RESULT_STALE;
        }
    }

    return true;
}

/**
 * @brief 空窗口遇到全部前向超窗新帧时，重建接收基线以恢复业务。
 * @note 不在有缓存或交付占用时提前跳过缺口。
 */
static void _linkg_transport_reorder_rebase_empty_locked(linkg_transport_reorder_class_t *current, const linkg_transport_reorder_group_t *group, const uint8_t *order, const linkg_transport_reorder_frame_t *frames, const linkg_transport_reorder_result_t *results)
{
    uint32_t position;
    uint32_t index;

    if (current->delivery_in_progress || current->slot_count != 0U)
    {
        return;
    }

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
        {
            continue;
        }

        if (!_linkg_transport_reorder_sequence_before(frames[index].header.sequence, current->expected_sequence) &&
            (frames[index].header.sequence - current->expected_sequence) < LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            return;
        }
    }

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] == LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED &&
            _linkg_transport_reorder_sequence_before(current->expected_sequence, frames[index].header.sequence))
        {
            current->expected_sequence = frames[index].header.sequence;
            return;
        }
    }
}

/**
 * @brief 标记已过期、重复或超窗的新输入帧。
 */
static void _linkg_transport_reorder_filter_group_locked(linkg_transport_reorder_class_t *current, const linkg_transport_reorder_group_t *group, const uint8_t *order, const linkg_transport_reorder_frame_t *frames, linkg_transport_reorder_result_t *results)
{
    linkg_transport_reorder_slot_t *slot;
    uint32_t                        sequence;
    uint32_t                        position;
    uint32_t                        previous;
    uint32_t                        index;

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
        {
            continue;
        }

        sequence = frames[index].header.sequence;

        if (_linkg_transport_reorder_sequence_before(sequence, current->expected_sequence))
        {
            results[index] = LINKG_TRANSPORT_REORDER_RESULT_TOO_OLD;
            continue;
        }

        if ((sequence - current->expected_sequence) >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            results[index] = LINKG_TRANSPORT_REORDER_RESULT_OUT_OF_WINDOW;
            continue;
        }

        slot = &current->slots[sequence & LINKG_TRANSPORT_REORDER_WINDOW_MASK];

        if (slot->frame.packet != NULL)
        {
            results[index] = slot->frame.header.sequence == sequence ? LINKG_TRANSPORT_REORDER_RESULT_TOO_OLD : LINKG_TRANSPORT_REORDER_RESULT_OUT_OF_WINDOW;
            continue;
        }

        for (previous = 0U; previous < position; previous++)
        {
            if (results[order[previous]] == LINKG_TRANSPORT_REORDER_RESULT_STALE)
            {
                continue;
            }

            if (frames[order[previous]].header.sequence == sequence)
            {
                results[index] = LINKG_TRANSPORT_REORDER_RESULT_TOO_OLD;
                break;
            }
        }
    }
}

/**
 * @brief 计算从expected_sequence开始的输入连续前缀及原始输入顺序。
 */
static uint32_t _linkg_transport_reorder_ready_prefix_locked(const linkg_transport_reorder_class_t *current, const linkg_transport_reorder_group_t *group, const uint8_t *order, const linkg_transport_reorder_frame_t *frames, const linkg_transport_reorder_result_t *results, bool *input_in_order)
{
    uint32_t expected_sequence;
    uint32_t previous_index;
    uint32_t prefix_count;
    uint32_t position;
    uint32_t index;

    expected_sequence = current->expected_sequence;
    previous_index    = 0U;
    prefix_count      = 0U;
    *input_in_order   = true;

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
        {
            continue;
        }

        if (frames[index].header.sequence != expected_sequence)
        {
            break;
        }

        if (prefix_count > 0U && index < previous_index)
        {
            *input_in_order = false;
        }

        previous_index = index;
        expected_sequence++;
        prefix_count++;
    }

    return prefix_count;
}

/**
 * @brief 缓存分组中未处理的输入帧，取得Packet独立引用。
 */
static void _linkg_transport_reorder_cache_group_locked(linkg_transport_reorder_class_t *current, const linkg_transport_reorder_group_t *group, const uint8_t *order, const linkg_transport_reorder_frame_t *frames, linkg_transport_reorder_result_t *results, uint64_t now_us, bool *worker_wakeup)
{
    linkg_transport_reorder_slot_t *slot;
    uint64_t                        wait_us;
    uint32_t                        sequence;
    uint32_t                        position;
    uint32_t                        index;

    wait_us = _linkg_transport_reorder_wait_us(group->traffic_class);

    for (position = 0U; position < group->count; position++)
    {
        index = order[position];

        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
        {
            continue;
        }

        sequence = frames[index].header.sequence;
        slot     = &current->slots[sequence & LINKG_TRANSPORT_REORDER_WINDOW_MASK];

        linkg_packet_retain(frames[index].packet);

        slot->frame         = frames[index];
        slot->expires_at_us = now_us + wait_us;
        current->slot_count++;

        results[index] = LINKG_TRANSPORT_REORDER_RESULT_CACHED;
        *worker_wakeup = true;
    }
}

/**
 * @brief 将从expected_sequence开始的连续缓存移入同步Emit输出。
 * @note 输出每个Frame均携带Reorder持有的Packet引用，回调后统一释放。
 */
static void _linkg_transport_reorder_take_contiguous_locked(linkg_transport_reorder_class_t *current, const linkg_transport_reorder_group_t *group, const linkg_transport_reorder_frame_t *frames, linkg_transport_reorder_result_t *results, linkg_transport_reorder_frame_t *emit_frames, bool *emit_owned, uint32_t *emit_count)
{
    linkg_transport_reorder_slot_t *slot;
    uint32_t                        sequence;
    uint32_t                        position;
    uint32_t                        index;

    while (*emit_count < LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX)
    {
        sequence = current->expected_sequence;
        slot     = &current->slots[sequence & LINKG_TRANSPORT_REORDER_WINDOW_MASK];

        if (slot->frame.packet == NULL || slot->frame.header.sequence != sequence)
        {
            break;
        }

        emit_frames[*emit_count] = slot->frame;
        emit_owned[*emit_count]  = true;
        (*emit_count)++;

        for (position = 0U; position < group->count; position++)
        {
            index = group->indices[position];

            if (results[index] == LINKG_TRANSPORT_REORDER_RESULT_CACHED &&
                frames[index].header.sequence == sequence)
            {
                results[index] = LINKG_TRANSPORT_REORDER_RESULT_EMITTED;
                break;
            }
        }

        memset(slot, 0, sizeof(*slot));
        current->slot_count--;
        current->expected_sequence++;
        current->delivery_in_progress = true;
        current->delivery_thread      = pthread_self();
    }
}

/**
 * @brief 在前向Sequence越窗时，按序取出必须滑出窗口的缓存帧。
 *
 * @note 只跳过缺失Sequence；已缓存的Frame先移交Emit，绝不直接覆盖。
 *       返回时最多移交32帧，剩余缓存等待下一轮推进。
 */
static uint32_t _linkg_transport_reorder_take_pressure_locked(linkg_transport_reorder_class_t *current, uint32_t incoming_sequence, linkg_transport_reorder_frame_t *output, bool *owned, uint32_t capacity)
{
    linkg_transport_reorder_slot_t *slot;
    linkg_transport_reorder_slot_t *nearest;
    uint32_t                        target_sequence;
    uint32_t                        target_distance;
    uint32_t                        nearest_distance;
    uint32_t                        distance;
    uint32_t                        slot_index;
    uint32_t                        count;

    if (current->delivery_in_progress || capacity == 0U)
    {
        return 0U;
    }

    target_sequence = incoming_sequence - (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U);
    count           = 0U;

    // 只提前释放新窗口左边界之前的缓存，窗口内已有帧继续等待或交付。
    while (_linkg_transport_reorder_sequence_before(current->expected_sequence, target_sequence) && count < capacity)
    {
        target_distance  = target_sequence - current->expected_sequence;
        nearest_distance = target_distance;
        nearest          = NULL;

        for (slot_index = 0U; slot_index < LINKG_TRANSPORT_REORDER_WINDOW_SIZE; slot_index++)
        {
            slot = &current->slots[slot_index];

            if (slot->frame.packet == NULL)
            {
                continue;
            }

            distance = slot->frame.header.sequence - current->expected_sequence;
            if (distance < nearest_distance)
            {
                nearest_distance = distance;
                nearest          = slot;
            }
        }

        if (nearest == NULL)
        {
            // 滑出范围内没有已收到的帧，可以直接跳过全部缺口。
            current->expected_sequence = target_sequence;
            break;
        }

        // 最近的已缓存帧之前全部为缺口；保留该帧并按序移交。
        current->expected_sequence = nearest->frame.header.sequence;
        output[count]              = nearest->frame;
        owned[count]               = true;
        count++;

        memset(nearest, 0, sizeof(*nearest));
        current->slot_count--;
        current->expected_sequence++;
    }

    if (count > 0U)
    {
        current->delivery_in_progress = true;
        current->delivery_thread      = pthread_self();
    }

    return count;
}

/**
 * @brief 根据状态为单个Peer/Class选择互斥交付路径。
 */
static void _linkg_transport_reorder_process_group_locked(const linkg_transport_reorder_group_t *group, const linkg_transport_reorder_frame_t *frames, linkg_transport_reorder_result_t *results, linkg_transport_reorder_frame_t *emit_frames, bool *emit_owned, uint32_t *emit_count, uint64_t now_us, bool *worker_wakeup)
{
    linkg_transport_reorder_class_t *current;
    linkg_transport_reorder_slot_t  *next_slot;
    uint8_t                          order[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    uint32_t                         prefix_count;
    uint32_t                         next_sequence;
    uint32_t                         position;
    uint32_t                         ready_count;
    uint32_t                         index;
    bool                             input_in_order;
    bool                             cached_next;

    current = &g_reorder.classes[group->peer_index][group->traffic_class];
    _linkg_transport_reorder_sort_group(group, frames, order);

    // 分支一：失效生命周期不可修改旧Peer/Class缓存。
    if (!_linkg_transport_reorder_prepare_group_locked(current, group, order, frames, results))
    {
        return;
    }

    // 分支二：同Peer/Class上一批交付未结束，本组新帧只能缓存。
    if (current->delivery_in_progress)
    {
        _linkg_transport_reorder_filter_group_locked(current, group, order, frames, results);
        _linkg_transport_reorder_cache_group_locked(current, group, order, frames, results, now_us, worker_wakeup);
        return;
    }

    // 分支三：没有缓存和交付占用时允许对远端Sequence重新建立窗口基线。
    _linkg_transport_reorder_rebase_empty_locked(current, group, order, frames, results);
    _linkg_transport_reorder_filter_group_locked(current, group, order, frames, results);

    prefix_count = _linkg_transport_reorder_ready_prefix_locked(current, group, order, frames, results, &input_in_order);
    next_sequence = current->expected_sequence + prefix_count;
    next_slot     = &current->slots[next_sequence & LINKG_TRANSPORT_REORDER_WINDOW_MASK];
    cached_next   = next_slot->frame.packet != NULL && next_slot->frame.header.sequence == next_sequence;

    // 分支四：原始RX中存在有序连续前缀，无需衔接缓存，直接READY。
    if (prefix_count > 0U && input_in_order && !cached_next)
    {
        ready_count = 0U;

        for (position = 0U; position < group->count && ready_count < prefix_count; position++)
        {
            index = order[position];

            if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
            {
                continue;
            }

            results[index] = LINKG_TRANSPORT_REORDER_RESULT_READY;
            ready_count++;
        }

        current->expected_sequence    = next_sequence;
        current->delivery_in_progress = true;
        current->delivery_thread      = pthread_self();

        _linkg_transport_reorder_cache_group_locked(current, group, order, frames, results, now_us, worker_wakeup);
        return;
    }

    // 分支五：批内乱序或补齐历史缓存，缓存后抽取有序前缀同步Emit。
    _linkg_transport_reorder_cache_group_locked(current, group, order, frames, results, now_us, worker_wakeup);
    _linkg_transport_reorder_take_contiguous_locked(current, group, frames, results, emit_frames, emit_owned, emit_count);
}

/**
 * @brief 按Peer/Class整理批次并分别作出交付决策。
 */
static void _linkg_transport_reorder_process_batch_locked(const linkg_transport_reorder_frame_t *frames, uint32_t count, const uint32_t *peer_indices, linkg_transport_reorder_result_t *results, linkg_transport_reorder_frame_t *emit_frames, bool *emit_owned, uint32_t *emit_count, bool *worker_wakeup)
{
    linkg_transport_reorder_group_t groups[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    uint32_t                        group_count;
    uint32_t                        group_index;
    uint64_t                        now_us;

    group_count = _linkg_transport_reorder_build_groups(frames, count, peer_indices, groups);
    now_us      = linkg_time_monotonic_us();

    for (group_index = 0U; group_index < group_count; group_index++)
    {
        _linkg_transport_reorder_process_group_locked(&groups[group_index], frames, results, emit_frames, emit_owned, emit_count, now_us, worker_wakeup);
    }
}

/**
 * @brief 在完成同步Emit后，释放相应Peer/Class的交付占用并唤醒等待者。
 */
static void _linkg_transport_reorder_complete_emit_locked(const linkg_transport_reorder_frame_t *frames, uint32_t count)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        peer_index;
    uint32_t                        class_index;
    uint32_t                        index;
    int                             slot_index;
    bool                            changed;

    changed = false;

    for (index = 0U; index < count; index++)
    {
        slot_index = _linkg_transport_reorder_peer_index(frames[index].peer);
        if (slot_index < 0)
        {
            continue;
        }

        peer_index  = (uint32_t)slot_index;
        class_index = (uint32_t)frames[index].traffic_class;
        current     = &g_reorder.classes[peer_index][class_index];

        if (current->expected_initialized &&
            current->peer_epoch == frames[index].peer_epoch &&
            current->peer_node_id == frames[index].peer_node_id &&
            current->delivery_in_progress)
        {
            current->delivery_in_progress = false;
            changed = true;
        }
    }

    if (changed)
    {
        pthread_cond_broadcast(&g_reorder.condition);
    }
}

/**
 * @brief 在Reorder锁外同步回调，之后释放全部输出引用和交付占用。
 */
static void _linkg_transport_reorder_emit_batch(linkg_transport_reorder_emit_func_t emit, const linkg_transport_reorder_frame_t *frames, const bool *owned, uint32_t count)
{
    uint32_t index;
    int      ret;

    if (count == 0U)
    {
        return;
    }

    emit(frames, count);

    for (index = 0U; index < count; index++)
    {
        if (owned[index])
        {
            linkg_packet_release(frames[index].packet);
        }
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("transport reorder emit completion lock failed, error=%d", ret);
        return;
    }

    _linkg_transport_reorder_complete_emit_locked(frames, count);
    pthread_mutex_unlock(&g_reorder.lock);
}

/**
 * @brief 判断当前分组是否包含无法在当前64槽窗口内一次处理的前向帧。
 *
 * @note 未建立基线时使用批内最早Sequence；不改变已经稳定运行的READY快速路径。
 */
static bool _linkg_transport_reorder_group_needs_pressure_locked(const linkg_transport_reorder_group_t *group, const linkg_transport_reorder_frame_t *frames)
{
    linkg_transport_reorder_class_t *current;
    uint8_t                          order[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    uint32_t                         base_sequence;
    uint32_t                         sequence;
    uint32_t                         position;
    uint32_t                         index;

    current = &g_reorder.classes[group->peer_index][group->traffic_class];
    _linkg_transport_reorder_sort_group(group, frames, order);

    base_sequence = current->expected_initialized ? current->expected_sequence : frames[order[0]].header.sequence;

    for (position = 0U; position < group->count; position++)
    {
        index    = order[position];
        sequence = frames[index].header.sequence;

        if (_linkg_transport_reorder_sequence_before(sequence, base_sequence))
        {
            continue;
        }

        if ((sequence - base_sequence) >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断当前线程是否已有任意Peer/Class交付占用，防止等待交叉重入。
 *
 * @note 调用方必须持有g_reorder.lock。
 */
static bool _linkg_transport_reorder_self_delivering_locked(void)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        peer_index;
    uint32_t                        class_index;

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            current = &g_reorder.classes[peer_index][class_index];

            if (current->delivery_in_progress && pthread_equal(current->delivery_thread, pthread_self()) != 0)
            {
                return true;
            }
        }
    }

    return false;
}

/**
 * @brief 释放Reorder锁执行一批同步Emit，再重新取得锁。
 *
 * @note 调用方必须持有g_reorder.lock，正常返回时仍持锁。
 */
static int _linkg_transport_reorder_emit_and_relock(linkg_transport_reorder_emit_func_t emit, const linkg_transport_reorder_frame_t *frames, const bool *owned, uint32_t count, bool *lock_held)
{
    int ret;

    ret = pthread_mutex_unlock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    *lock_held = false;
    _linkg_transport_reorder_emit_batch(emit, frames, owned, count);

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret == 0)
    {
        *lock_held = true;
    }

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 将包含前向超窗帧的分组拆成有界Sequence段并同步推进。
 *
 * @note 保留正常路径的READY语义；此压力路径的READY在返回前转为EMITTED，
 *       确保可以继续处理同一个提交中的后续Sequence。
 *       调用方必须持有g_reorder.lock。
 */
static int _linkg_transport_reorder_process_pressure_group_locked(const linkg_transport_reorder_group_t *group, const linkg_transport_reorder_frame_t *frames, linkg_transport_reorder_result_t *results, linkg_transport_reorder_emit_func_t emit, bool *worker_wakeup, bool *lock_held)
{
    linkg_transport_reorder_frame_t  emit_frames[LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX];
    linkg_transport_reorder_class_t *current;
    linkg_transport_reorder_group_t  partial;
    uint8_t                           order[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    bool                              emit_owned[LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX];
    uint32_t                          emit_count;
    uint32_t                          position;
    uint32_t                          start;
    uint32_t                          index;
    uint32_t                          sequence;
    uint32_t                          next;
    uint32_t                          partial_base;
    int                               ret;

    current = &g_reorder.classes[group->peer_index][group->traffic_class];
    _linkg_transport_reorder_sort_group(group, frames, order);
    position = 0U;

    while (position < group->count)
    {
        index = order[position];

        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED)
        {
            position++;
            continue;
        }

        if (!g_reorder.running || !linkg_transport_peer_epoch_matches(frames[index].peer, frames[index].peer_epoch) ||
            (current->expected_initialized &&
             (current->peer_epoch != frames[index].peer_epoch || current->peer_node_id != frames[index].peer_node_id)))
        {
            results[index] = LINKG_TRANSPORT_REORDER_RESULT_STALE;
            position++;
            continue;
        }

        sequence = frames[index].header.sequence;

        // 空缓存允许恢复到新的前向基线，不受旧缺口拖累。
        if (current->expected_initialized && !current->delivery_in_progress && current->slot_count == 0U &&
            _linkg_transport_reorder_sequence_before(current->expected_sequence, sequence) &&
            (sequence - current->expected_sequence) >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            current->expected_sequence = sequence;
        }

        if (current->expected_initialized &&
            !_linkg_transport_reorder_sequence_before(sequence, current->expected_sequence) &&
            (sequence - current->expected_sequence) >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            if (current->delivery_in_progress)
            {
                // 其他线程交付时不能覆盖缓存；释放锁等待交付结束后重新判定窗口。
                // 当前线程已有交付回调时不能等待，否则可能形成跨Peer/Class循环等待。
                if (_linkg_transport_reorder_self_delivering_locked())
                {
                    results[index] = LINKG_TRANSPORT_REORDER_RESULT_OUT_OF_WINDOW;
                    position++;
                    continue;
                }

                ret = pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
                if (ret != 0)
                {
                    return -ret;
                }

                continue;
            }

            memset(emit_owned, 0, sizeof(emit_owned));
            emit_count = _linkg_transport_reorder_take_pressure_locked(current, sequence, emit_frames, emit_owned, LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX);

            if (emit_count > 0U)
            {
                ret = _linkg_transport_reorder_emit_and_relock(emit, emit_frames, emit_owned, emit_count, lock_held);
                if (ret != 0)
                {
                    return ret;
                }
            }

            // 可能存在超过32帧的历史缓存，继续推进直到新Sequence可进入窗口。
            continue;
        }

        memset(&partial, 0, sizeof(partial));
        partial.peer_index    = group->peer_index;
        partial.traffic_class = group->traffic_class;
        start                 = position;
        partial_base          = current->expected_initialized ? current->expected_sequence : sequence;

        // 同一次内部提交只选取当前窗口能容纳的帧，不能预先丢弃更远的帧。
        while (position < group->count)
        {
            next = order[position];

            if (results[next] == LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED &&
                !_linkg_transport_reorder_sequence_before(frames[next].header.sequence, partial_base) &&
                (frames[next].header.sequence - partial_base) >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
            {
                break;
            }

            partial.indices[partial.count++] = (uint8_t)next;
            position++;
        }

        if (partial.count == 0U)
        {
            // 窗口可能在其他线程释放锁后发生变化，下轮重新检查。
            position = start;
            continue;
        }

        emit_count = 0U;
        memset(emit_owned, 0, sizeof(emit_owned));
        _linkg_transport_reorder_process_group_locked(&partial, frames, results, emit_frames, emit_owned, &emit_count, linkg_time_monotonic_us(), worker_wakeup);

        if (*worker_wakeup)
        {
            pthread_cond_broadcast(&g_reorder.condition);
        }

        if (emit_count == 0U)
        {
            // 原READY快速路径在压力分组内直接同步交付，避免阻塞下一段处理。
            for (next = 0U; next < partial.count; next++)
            {
                index = partial.indices[next];

                if (results[index] == LINKG_TRANSPORT_REORDER_RESULT_READY)
                {
                    emit_frames[emit_count] = frames[index];
                    emit_owned[emit_count]  = false;
                    results[index]          = LINKG_TRANSPORT_REORDER_RESULT_EMITTED;
                    emit_count++;
                }
            }
        }

        if (emit_count > 0U)
        {
            ret = _linkg_transport_reorder_emit_and_relock(emit, emit_frames, emit_owned, emit_count, lock_held);
            if (ret != 0)
            {
                return ret;
            }
        }
    }

    return 0;
}

/****************************** Worker ******************************/

/**
 * @brief 按全窗口最早到期时间推进Peer/Class缓存并提取可交付帧。
 *
 * @note 任意缓存帧到期时，推进到最远的已到期Sequence。
 *       推进区间内已有帧按序输出，缺失Sequence直接跳过。
 *       超时推进完成后，继续输出紧邻的连续缓存帧。
 */
static uint32_t _linkg_transport_reorder_take_worker_class_locked(linkg_transport_reorder_class_t *current, linkg_transport_reorder_frame_t *output, uint32_t capacity, uint64_t now_us, uint64_t *deadline_us)
{
    linkg_transport_reorder_slot_t *slot;
    uint64_t                        earliest_deadline;
    uint32_t                        farthest_expired_distance;
    uint32_t                        sequence;
    uint32_t                        distance;
    uint32_t                        slot_index;
    uint32_t                        count;
    bool                            expired;

    if (!current->expected_initialized ||
        current->delivery_in_progress ||
        current->slot_count == 0U ||
        capacity == 0U)
    {
        return 0U;
    }

    earliest_deadline         = 0ULL;
    farthest_expired_distance = 0U;
    expired                   = false;

    // 扫描全部缓存，取得最早截止时间以及最远的已到期Sequence。
    for (slot_index = 0U; slot_index < LINKG_TRANSPORT_REORDER_WINDOW_SIZE; slot_index++)
    {
        slot = &current->slots[slot_index];

        if (slot->frame.packet == NULL)
        {
            continue;
        }

        distance = slot->frame.header.sequence - current->expected_sequence;

        if (distance >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            continue;
        }

        if (earliest_deadline == 0ULL || slot->expires_at_us < earliest_deadline)
        {
            earliest_deadline = slot->expires_at_us;
        }

        if (slot->expires_at_us <= now_us &&
            (!expired || distance > farthest_expired_distance))
        {
            farthest_expired_distance = distance;
            expired                   = true;
        }
    }

    // Worker等待全窗口的最早截止时间，不受更近Sequence影响。
    if (earliest_deadline != 0ULL &&
        (*deadline_us == 0ULL || earliest_deadline < *deadline_us))
    {
        *deadline_us = earliest_deadline;
    }

    count = 0U;

    // 按Sequence向前推进，超时范围内缺失的Sequence直接跳过。
    for (distance = 0U; distance < LINKG_TRANSPORT_REORDER_WINDOW_SIZE && count < capacity; distance++)
    {
        sequence = current->expected_sequence;
        slot     = &current->slots[sequence & LINKG_TRANSPORT_REORDER_WINDOW_MASK];

        if (slot->frame.packet == NULL || slot->frame.header.sequence != sequence)
        {
            // 没有超时，或者已越过超时边界时，不能跳过新的缺口。
            if (!expired || distance > farthest_expired_distance)
            {
                break;
            }

            current->expected_sequence++;
            continue;
        }

        // 已缓存的帧按Sequence顺序交付。
        output[count++] = slot->frame;
        memset(slot, 0, sizeof(*slot));

        current->slot_count--;
        current->expected_sequence++;
    }

    if (count > 0U)
    {
        current->delivery_in_progress = true;
        current->delivery_thread      = pthread_self();
    }

    return count;
}

/**
 * @brief 逐Peer/Class扫描可交付的Worker帧，并计算最近超时期限。
 */
static uint32_t _linkg_transport_reorder_take_worker_locked(linkg_transport_reorder_frame_t *output, uint32_t capacity, uint64_t now_us, uint64_t *deadline_us)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        peer_index;
    uint32_t                        class_index;
    uint32_t                        flat_index;
    uint32_t                        total;
    uint32_t                        offset;
    uint32_t                        count;

    total = LINKG_TRANSPORT_PEER_MAX * LINKG_TRANSPORT_CLASS_COUNT;

    for (offset = 0U; offset < total; offset++)
    {
        flat_index  = (g_reorder.worker_cursor + offset) % total;
        peer_index  = flat_index / LINKG_TRANSPORT_CLASS_COUNT;
        class_index = flat_index % LINKG_TRANSPORT_CLASS_COUNT;
        current     = &g_reorder.classes[peer_index][class_index];

        if (current->expected_initialized &&
            !linkg_transport_peer_epoch_matches(&g_transport.peers[peer_index], current->peer_epoch) &&
            !current->delivery_in_progress)
        {
            _linkg_transport_reorder_clear_class(current);
            continue;
        }

        count = _linkg_transport_reorder_take_worker_class_locked(current, output, capacity, now_us, deadline_us);
        if (count > 0U)
        {
            g_reorder.worker_cursor = (flat_index + 1U) % total;
            return count;
        }
    }

    return 0U;
}

/**
 * @brief Reorder统一超时及缓存交付线程。
 */
static void _linkg_transport_reorder_worker(linkg_thread_t *thread, void *user_data)
{
    linkg_transport_reorder_frame_t frames[LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX];
    bool                            owned[LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX];
    struct timespec                 timeout;
    uint64_t                        deadline_us;
    uint64_t                        now_us;
    uint32_t                        count;
    uint32_t                        index;
    int                             ret;

    (void)user_data;

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return;
    }

    while (g_reorder.running && linkg_thread_is_running(thread))
    {
        deadline_us = 0ULL;
        now_us      = linkg_time_monotonic_us();

        count = _linkg_transport_reorder_take_worker_locked(frames, LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX, now_us, &deadline_us);

        if (count > 0U)
        {
            // 一次只交付一个Peer/Class，交付期间不持有Reorder锁。
            pthread_mutex_unlock(&g_reorder.lock);

            for (index = 0U; index < count; index++)
            {
                owned[index] = true;
            }

            _linkg_transport_reorder_emit_batch(g_reorder.emit, frames, owned, count);

            ret = pthread_mutex_lock(&g_reorder.lock);
            if (ret != 0)
            {
                return;
            }

            continue;
        }

        if (deadline_us == 0ULL)
        {
            ret = pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
        }
        else
        {
            timeout.tv_sec  = (time_t)(deadline_us / 1000000ULL);
            timeout.tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);
            ret = pthread_cond_timedwait(&g_reorder.condition, &g_reorder.lock, &timeout);
        }

        if (ret != 0 && ret != ETIMEDOUT)
        {
            LINKG_LOG_ERROR("transport reorder worker condition wait failed, error=%d", ret);
            break;
        }
    }

    pthread_mutex_unlock(&g_reorder.lock);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Reorder运行资源。
 */
int linkg_transport_reorder_runtime_init(linkg_transport_reorder_emit_func_t emit)
{
    pthread_condattr_t attributes;
    int                ret;

    if (emit == NULL)
    {
        return -EINVAL;
    }

    if (g_reorder.initialized)
    {
        return -EALREADY;
    }

    memset(&g_reorder, 0, sizeof(g_reorder));

    ret = pthread_mutex_init(&g_reorder.lock, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    ret = pthread_condattr_init(&attributes);
    if (ret != 0)
    {
        pthread_mutex_destroy(&g_reorder.lock);
        return -ret;
    }

    ret = pthread_condattr_setclock(&attributes, CLOCK_MONOTONIC);
    if (ret == 0)
    {
        ret = pthread_cond_init(&g_reorder.condition, &attributes);
    }

    pthread_condattr_destroy(&attributes);

    if (ret != 0)
    {
        pthread_mutex_destroy(&g_reorder.lock);
        return -ret;
    }

    ret = linkg_thread_init(&g_reorder.thread, LINKG_TRANSPORT_REORDER_THREAD_NAME, _linkg_transport_reorder_worker, NULL);
    if (ret != 0)
    {
        pthread_cond_destroy(&g_reorder.condition);
        pthread_mutex_destroy(&g_reorder.lock);
        return ret;
    }

    g_reorder.emit        = emit;
    g_reorder.running     = true;
    g_reorder.initialized = true;

    ret = linkg_thread_start(&g_reorder.thread);
    if (ret != 0)
    {
        g_reorder.running     = false;
        g_reorder.initialized = false;
        g_reorder.emit        = NULL;

        linkg_thread_deinit(&g_reorder.thread);
        pthread_cond_destroy(&g_reorder.condition);
        pthread_mutex_destroy(&g_reorder.lock);
        return ret;
    }

    return 0;
}

/**
 * @brief 等待已有交付结束，停止Worker并释放所有缓存资源。
 */
int linkg_transport_reorder_runtime_deinit(void)
{
    uint32_t peer_index;
    uint32_t class_index;
    bool     delivering;
    int      ret;

    if (!g_reorder.initialized)
    {
        return 0;
    }

    if (linkg_transport_reorder_is_worker_thread())
    {
        return -EDEADLK;
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    g_reorder.running = false;
    pthread_cond_broadcast(&g_reorder.condition);
    pthread_mutex_unlock(&g_reorder.lock);

    ret = linkg_thread_stop(&g_reorder.thread);
    if (ret != 0)
    {
        return ret;
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    // Link Manager已停止接收回调，此时只需等待正在进行的同步Emit或READY。
    for (;;)
    {
        delivering = false;

        for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX && !delivering; peer_index++)
        {
            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                if (g_reorder.classes[peer_index][class_index].delivery_in_progress)
                {
                    delivering = true;
                    break;
                }
            }
        }

        if (!delivering)
        {
            break;
        }

        ret = pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
        if (ret != 0)
        {
            pthread_mutex_unlock(&g_reorder.lock);
            return -ret;
        }
    }

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            _linkg_transport_reorder_clear_class(&g_reorder.classes[peer_index][class_index]);
        }
    }

    pthread_mutex_unlock(&g_reorder.lock);
    linkg_thread_deinit(&g_reorder.thread);
    pthread_cond_destroy(&g_reorder.condition);
    pthread_mutex_destroy(&g_reorder.lock);
    memset(&g_reorder, 0, sizeof(g_reorder));

    return 0;
}

/**
 * @brief 检查当前线程是否正在执行Reorder交付。
 *
 * @return 0表示允许同步生命周期操作，-EDEADLK表示当前线程持有交付占用。
 *
 * @note 当前线程交付任意Peer/Class期间，禁止同步Reset或注销任何Peer。
 *       只负责检查，不修改Peer生命周期和Reorder状态。
 */
int linkg_transport_reorder_check_lifecycle_context(void)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        peer_index;
    uint32_t                        class_index;
    int                             ret;

    if (!g_reorder.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            current = &g_reorder.classes[peer_index][class_index];

            if (!current->delivery_in_progress)
            {
                continue;
            }

            if (pthread_equal(current->delivery_thread, pthread_self()) != 0)
            {
                pthread_mutex_unlock(&g_reorder.lock);
                return -EDEADLK;
            }
        }
    }

    ret = pthread_mutex_unlock(&g_reorder.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 等待指定Peer交付结束，清理全部业务缓存及Sequence状态。
 */
int linkg_transport_reorder_reset_peer(uint8_t peer_node_id)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        peer_index;
    uint32_t                        class_index;
    bool                            delivering;
    int                             ret;

    if (!g_reorder.initialized)
    {
        return -ENODEV;
    }

    if (linkg_transport_reorder_is_worker_thread())
    {
        return -EDEADLK;
    }

    ret = linkg_transport_reorder_check_lifecycle_context();
    if (ret != 0)
    {
        return ret;
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    for (;;)
    {
        delivering = false;

        for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX && !delivering; peer_index++)
        {
            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                current = &g_reorder.classes[peer_index][class_index];

                if (current->expected_initialized && current->peer_node_id == peer_node_id && current->delivery_in_progress)
                {
                    // 当前线程不能等待自己尚未完成的交付。
                    if (pthread_equal(current->delivery_thread, pthread_self()) != 0)
                    {
                        pthread_mutex_unlock(&g_reorder.lock);
                        return -EDEADLK;
                    }

                    delivering = true;
                    break;
                }
            }
        }

        if (!delivering)
        {
            break;
        }

        ret = pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
        if (ret != 0)
        {
            pthread_mutex_unlock(&g_reorder.lock);
            return -ret;
        }
    }

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            current = &g_reorder.classes[peer_index][class_index];

            if (current->expected_initialized && current->peer_node_id == peer_node_id)
            {
                _linkg_transport_reorder_clear_class(current);
            }
        }
    }

    pthread_cond_broadcast(&g_reorder.condition);
    pthread_mutex_unlock(&g_reorder.lock);

    return 0;
}

/**
 * @brief 判断当前线程是否为Reorder Worker。
 */
bool linkg_transport_reorder_is_worker_thread(void)
{
    if (!g_reorder.initialized || !linkg_thread_is_started(&g_reorder.thread))
    {
        return false;
    }

    return pthread_equal(pthread_self(), g_reorder.thread.tid) != 0;
}

/****************************** 数据接收 ******************************/

/**
 * @brief 批量提交USER_DATA Wire Frame并返回逐帧处理结果。
 */
int linkg_transport_reorder_submit_batch(const linkg_transport_reorder_frame_t *frames, uint32_t count, linkg_transport_reorder_result_t *results)
{
    linkg_transport_reorder_frame_t     emit_frames[LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX];
    linkg_transport_reorder_emit_func_t emit;
    linkg_transport_reorder_group_t     groups[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    uint32_t                            peer_indices[LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX];
    bool                                emit_owned[LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX];
    uint32_t                            group_count;
    uint32_t                            group_index;
    uint32_t                            emit_count;
    uint32_t                            index;
    bool                                worker_wakeup;
    bool                                pressure;
    bool                                lock_held;
    int                                 peer_index;
    int                                 ret;

    if (count == 0U)
    {
        return 0;
    }

    if (frames == NULL || results == NULL || count > LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX)
    {
        return -EINVAL;
    }

    for (index = 0U; index < count; index++)
    {
        results[index] = LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED;
    }

    for (index = 0U; index < count; index++)
    {
        if (frames[index].packet == NULL ||
            frames[index].peer == NULL ||
            (uint32_t)frames[index].traffic_class >= LINKG_TRANSPORT_CLASS_COUNT)
        {
            return -EINVAL;
        }

        peer_index = _linkg_transport_reorder_peer_index(frames[index].peer);
        if (peer_index < 0)
        {
            return -EINVAL;
        }

        peer_indices[index] = (uint32_t)peer_index;
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (!g_reorder.initialized || !g_reorder.running)
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return -ENODEV;
    }

    emit_count    = 0U;
    worker_wakeup = false;
    lock_held     = true;
    emit          = g_reorder.emit;
    group_count   = _linkg_transport_reorder_build_groups(frames, count, peer_indices, groups);
    pressure      = false;

    memset(emit_owned, 0, sizeof(emit_owned));

    // 无窗口压力时继续使用原批量READY/Emit处理，不影响正常高速路径。
    for (group_index = 0U; group_index < group_count; group_index++)
    {
        if (_linkg_transport_reorder_group_needs_pressure_locked(&groups[group_index], frames))
        {
            pressure = true;
            break;
        }
    }

    if (!pressure)
    {
        _linkg_transport_reorder_process_batch_locked(frames, count, peer_indices, results, emit_frames, emit_owned, &emit_count, &worker_wakeup);
    }
    else
    {
        // 压力路径分组处理：先交付滑出窗口的缓存，再重新验证新输入。
        for (group_index = 0U; group_index < group_count; group_index++)
        {
            if (_linkg_transport_reorder_group_needs_pressure_locked(&groups[group_index], frames))
            {
                ret = _linkg_transport_reorder_process_pressure_group_locked(&groups[group_index], frames, results, emit, &worker_wakeup, &lock_held);
                if (ret != 0)
                {
                    if (lock_held)
                    {
                        pthread_mutex_unlock(&g_reorder.lock);
                    }

                    return ret;
                }
            }
            else
            {
                _linkg_transport_reorder_process_group_locked(&groups[group_index], frames, results, emit_frames, emit_owned, &emit_count, linkg_time_monotonic_us(), &worker_wakeup);

                if (emit_count > 0U)
                {
                    ret = _linkg_transport_reorder_emit_and_relock(emit, emit_frames, emit_owned, emit_count, &lock_held);
                    if (ret != 0)
                    {
                        if (lock_held)
                        {
                            pthread_mutex_unlock(&g_reorder.lock);
                        }

                        return ret;
                    }

                    emit_count = 0U;
                    memset(emit_owned, 0, sizeof(emit_owned));
                }
            }
        }
    }

    if (worker_wakeup)
    {
        pthread_cond_broadcast(&g_reorder.condition);
    }

    pthread_mutex_unlock(&g_reorder.lock);

    if (emit_count > 0U)
    {
        _linkg_transport_reorder_emit_batch(emit, emit_frames, emit_owned, emit_count);
    }

    return 0;
}

/****************************** 交付完成 ******************************/

/**
 * @brief RX Handler交付所有READY帧后解除相应Peer/Class交付占用。
 */
int linkg_transport_reorder_complete_batch(const linkg_transport_reorder_frame_t *frames, const linkg_transport_reorder_result_t *results, uint32_t count)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        class_index;
    uint32_t                        peer_index;
    uint32_t                        index;
    int                             slot_index;
    bool                            changed;
    int                             ret;

    if (count == 0U)
    {
        return 0;
    }

    if (frames == NULL || results == NULL || count > LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_reorder.lock);
    if (ret != 0)
    {
        return -ret;
    }

    changed = false;

    for (index = 0U; index < count; index++)
    {
        if (results[index] != LINKG_TRANSPORT_REORDER_RESULT_READY)
        {
            continue;
        }

        slot_index = _linkg_transport_reorder_peer_index(frames[index].peer);
        if (slot_index < 0)
        {
            continue;
        }

        peer_index  = (uint32_t)slot_index;
        class_index = (uint32_t)frames[index].traffic_class;

        if (class_index >= LINKG_TRANSPORT_CLASS_COUNT)
        {
            continue;
        }

        current = &g_reorder.classes[peer_index][class_index];

        if (current->expected_initialized &&
            current->peer_epoch == frames[index].peer_epoch &&
            current->peer_node_id == frames[index].peer_node_id &&
            current->delivery_in_progress)
        {
            current->delivery_in_progress = false;
            changed = true;
        }
    }

    if (changed)
    {
        pthread_cond_broadcast(&g_reorder.condition);
    }

    pthread_mutex_unlock(&g_reorder.lock);

    return 0;
}
