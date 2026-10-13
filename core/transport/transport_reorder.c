/**
 * @file transport_reorder.c
 * @brief LinkG本机USER_DATA接收顺序恢复
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-12
 */

#include "transport_reorder.h"

#include <errno.h>
#include <string.h>
#include <time.h>

#include "linkg_packet_pool.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_REORDER_THREAD_NAME       "trans-reorder" // 全局串行交付线程名称
#define LINKG_TRANSPORT_REORDER_WINDOW_SIZE       64U             // VIDEO/DATA窗口Sequence槽数量
#define LINKG_TRANSPORT_REORDER_DIRECT_QUEUE_SIZE 64U             // 单Peer/Class即时交付待处理队列容量
#define LINKG_TRANSPORT_REORDER_BATCH_MAX         32U             // 单次有序交付帧数
#define LINKG_TRANSPORT_REORDER_TIMEOUT_US        60000ULL        // VIDEO/DATA缺口和不完整分片最长等待时间
#define LINKG_TRANSPORT_REORDER_SERVICE_CYCLE     7U              // 加权业务调度周期，防止高负载REALTIME持续饿死VIDEO/DATA

_Static_assert((LINKG_TRANSPORT_REORDER_WINDOW_SIZE & (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U)) == 0U, "reorder window size must be power of two");

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_transport_reorder_frame_t slots[LINKG_TRANSPORT_REORDER_WINDOW_SIZE];         // 冗余VIDEO/DATA窗口
    linkg_transport_reorder_frame_t direct[LINKG_TRANSPORT_REORDER_DIRECT_QUEUE_SIZE];  // 冗余REALTIME即时交付队列
    uint64_t                        wait_start_us;                                      // 当前缺口首次等待起点
    uint32_t                        expected_sequence;                                  // 下一个应交付的Wire Sequence
    uint32_t                        last_single_sequence;                               // 单链路阶段已收到的最大Wire Sequence
    uint32_t                        realtime_highest;                                   // REALTIME最后完成交付的Packet末尾Sequence
    uint32_t                        direct_head;                                        // 即时队列读下标
    uint32_t                        direct_count;                                       // 即时队列待交付帧数
    uint32_t                        slot_count;                                         // 当前重排缓存帧数
    uint32_t                        peer_epoch;                                         // 当前缓存所属Peer代际
    uint8_t                         peer_node_id;                                       // 当前缓存所属直接Peer
    bool                            expected_initialized;                               // 有效expected_sequence
    bool                            single_initialized;                                 // 有效last_single_sequence
    bool                            realtime_initialized;                               // 有效realtime_highest
    bool                            redundant;                                          // 本Class当前使用冗余接收模式
} linkg_transport_reorder_class_t;

typedef struct
{
    pthread_mutex_t                       lock;                                              // 缓存和生命周期保护锁
    pthread_cond_t                        condition;                                         // 新帧和超时唤醒条件
    linkg_thread_t                        thread;                                            // 全局唯一有序交付线程
    linkg_transport_reorder_class_t       classes[LINKG_TRANSPORT_PEER_MAX][LINKG_TRANSPORT_CLASS_COUNT]; // 各Peer/Class独立状态
    linkg_transport_reorder_emit_func_t   emit;                                              // 不持锁调用的Transport接收交付回调
    uint32_t                              service_turn;                                      // 下次优先尝试的服务位置
    uint32_t                              next_peer_index[LINKG_TRANSPORT_CLASS_COUNT];      // 各Class下一次起始Peer，防止多STA饥饿
    uint8_t                               processing_peer_node_id;                           // 当前回调执行中的直接Peer
    bool                                  initialized;                                       // 运行时已初始化
    bool                                  running;                                           // Worker继续工作
} linkg_transport_reorder_runtime_t;

/****************************** 模块状态 ******************************/

static linkg_transport_reorder_runtime_t g_reorder;

/****************************** 内部辅助 ******************************/

/**
 * @brief 使用32位环绕Sequence的前后关系，要求比较距离小于半个序号空间。
 */
static bool _linkg_transport_reorder_before(uint32_t left, uint32_t right)
{
    return (int32_t)(left - right) < 0;
}

/**
 * @brief 获取当前固定Peer槽位索引。
 */
static int _linkg_transport_reorder_peer_index(const linkg_transport_peer_t *peer)
{
    uint32_t index;

    if (peer == NULL)
    {
        return -EINVAL;
    }

    for (index = 0U; index < LINKG_TRANSPORT_PEER_MAX; index++)
    {
        if (peer == &g_transport.peers[index])
        {
            return (int)index;
        }
    }

    return -EINVAL;
}

/**
 * @brief 释放被Reorder持有的Wire Frame引用。
 */
static void _linkg_transport_reorder_release_frame(linkg_transport_reorder_frame_t *frame)
{
    if (frame->packet != NULL)
    {
        linkg_packet_release(frame->packet);
    }

    memset(frame, 0, sizeof(*frame));
}

/**
 * @brief 清除尚未交付的缓存；调用方持有Reorder锁。
 */
static void _linkg_transport_reorder_clear_class(linkg_transport_reorder_class_t *current)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_REORDER_WINDOW_SIZE; index++)
    {
        _linkg_transport_reorder_release_frame(&current->slots[index]);
    }

    for (index = 0U; index < LINKG_TRANSPORT_REORDER_DIRECT_QUEUE_SIZE; index++)
    {
        _linkg_transport_reorder_release_frame(&current->direct[index]);
    }

    memset(current, 0, sizeof(*current));
}

/**
 * @brief 判断两个分片是否符合当前固定两片协议。
 */
static bool _linkg_transport_reorder_pair_match(const linkg_transport_reorder_frame_t *first, const linkg_transport_reorder_frame_t *tail)
{
    return first->fragmented && tail->fragmented &&
           first->fragment_header.fragment_offset == 0U &&
           tail->fragment_header.fragment_offset == LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE &&
           first->fragment_header.packet_id == tail->fragment_header.packet_id &&
           first->fragment_header.packet_length == tail->fragment_header.packet_length &&
           first->header.source_node_id == tail->header.source_node_id &&
           first->header.destination_node_id == tail->header.destination_node_id &&
           first->header.type == tail->header.type &&
           first->peer_epoch == tail->peer_epoch;
}

/**
 * @brief 将已消费的窗口槽所有权移动到输出批次。
 */
static void _linkg_transport_reorder_pop_slot(linkg_transport_reorder_class_t *current, linkg_transport_reorder_frame_t *output)
{
    linkg_transport_reorder_frame_t *slot;

    slot    = &current->slots[current->expected_sequence & (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U)];
    *output = *slot;

    memset(slot, 0, sizeof(*slot));

    current->slot_count--;
    current->expected_sequence++;
    current->wait_start_us = 0U;
}

/**
 * @brief 高Sequence超过256槽时强制推进窗口并丢弃无法恢复的旧缓存。
 */
static void _linkg_transport_reorder_advance_window(linkg_transport_reorder_class_t *current, uint32_t sequence)
{
    uint32_t next_expected;
    uint32_t index;

    next_expected = sequence - (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U);

    for (index = 0U; index < LINKG_TRANSPORT_REORDER_WINDOW_SIZE; index++)
    {
        if (current->slots[index].packet != NULL &&
            _linkg_transport_reorder_before(current->slots[index].header.sequence, next_expected))
        {
            _linkg_transport_reorder_release_frame(&current->slots[index]);
            current->slot_count--;
        }
    }

    current->expected_sequence = next_expected;
    current->wait_start_us     = 0U;
}

/**
 * @brief 一个缺口已经超时后直接跳至最近已缓存Sequence，避免连续缺口重复等待60ms。
 */
static void _linkg_transport_reorder_skip_gap(linkg_transport_reorder_class_t *current)
{
    uint32_t nearest_sequence;
    uint32_t distance;
    uint32_t nearest_distance;
    uint32_t index;

    nearest_distance = UINT32_MAX;
    nearest_sequence = current->expected_sequence;

    for (index = 0U; index < LINKG_TRANSPORT_REORDER_WINDOW_SIZE; index++)
    {
        if (current->slots[index].packet == NULL)
        {
            continue;
        }

        distance = current->slots[index].header.sequence - current->expected_sequence;

        if (distance < nearest_distance)
        {
            nearest_distance = distance;
            nearest_sequence = current->slots[index].header.sequence;
        }
    }

    if (nearest_distance != UINT32_MAX)
    {
        current->expected_sequence = nearest_sequence;
    }

    current->wait_start_us = 0U;
}

/**
 * @brief 对单个VIDEO/DATA窗口提取连续完整帧，分片必须成对进入Reassembly。
 *
 * @note 有缺口或缺少LAST时最多等待60ms。超时或滑窗时跳过缺失Sequence。
 */
static uint32_t _linkg_transport_reorder_drain_window(linkg_transport_reorder_class_t *current, linkg_transport_reorder_frame_t *output, uint32_t capacity, uint64_t now_us, uint64_t *deadline_us)
{
    linkg_transport_reorder_frame_t *first;
    linkg_transport_reorder_frame_t *tail;
    uint64_t                        expires_at_us;
    uint32_t                        count;

    count = 0U;

    while (current->slot_count > 0U && count < capacity)
    {
        first = &current->slots[current->expected_sequence & (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U)];

        if (first->packet == NULL)
        {
            if (current->wait_start_us == 0U)
            {
                current->wait_start_us = now_us;
            }
        }
        else if (first->fragmented && first->fragment_header.fragment_offset == 0U)
        {
            tail = &current->slots[(current->expected_sequence + 1U) & (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U)];

            if (tail->packet != NULL && !_linkg_transport_reorder_pair_match(first, tail))
            {
                _linkg_transport_reorder_release_frame(first);
                current->slot_count--;
                current->expected_sequence++;
                current->wait_start_us = 0U;
                continue;
            }

            if (tail->packet != NULL)
            {
                if (capacity - count < LINKG_TRANSPORT_FRAGMENT_COUNT_MAX)
                {
                    break;
                }

                _linkg_transport_reorder_pop_slot(current, &output[count++]);
                _linkg_transport_reorder_pop_slot(current, &output[count++]);
                continue;
            }

            if (current->wait_start_us == 0U)
            {
                current->wait_start_us = now_us;
            }
        }
        else if (first->fragmented)
        {
            // 孤立LAST不可能构成完整Packet，不能交付原有重组缓存。
            _linkg_transport_reorder_release_frame(first);
            current->slot_count--;
            current->expected_sequence++;
            current->wait_start_us = 0U;
            continue;
        }
        else
        {
            _linkg_transport_reorder_pop_slot(current, &output[count++]);
            continue;
        }

        expires_at_us = current->wait_start_us + LINKG_TRANSPORT_REORDER_TIMEOUT_US;

        if (now_us < expires_at_us)
        {
            if (*deadline_us == 0U || expires_at_us < *deadline_us)
            {
                *deadline_us = expires_at_us;
            }
            break;
        }

        if (first->packet != NULL)
        {
            // FIRST超时未能配对时丢弃FIRST，后续迟到LAST也会被跳过。
            _linkg_transport_reorder_release_frame(first);
            current->slot_count--;
        }

        current->expected_sequence++;
        _linkg_transport_reorder_skip_gap(current);
    }

    if (current->slot_count == 0U)
    {
        current->wait_start_us = 0U;
    }

    return count;
}

/**
 * @brief 从当前Peer/Class即时队列提取一批，优先于同Class新启用的Reorder帧。
 */
static uint32_t _linkg_transport_reorder_drain_direct(linkg_transport_reorder_class_t *current, linkg_transport_reorder_frame_t *output)
{
    uint32_t count;

    count = 0U;

    while (current->direct_count > 0U && count < LINKG_TRANSPORT_REORDER_BATCH_MAX)
    {
        output[count] = current->direct[current->direct_head];
        memset(&current->direct[current->direct_head], 0, sizeof(current->direct[current->direct_head]));

        current->direct_head = (current->direct_head + 1U) % LINKG_TRANSPORT_REORDER_DIRECT_QUEUE_SIZE;
        current->direct_count--;
        count++;
    }

    return count;
}

/**
 * @brief 扫描全部Peer/Class，选择最高优先级的一批已就绪Frame。
 *
 * @note Worker独占最终回调，保证跨Link提交以及超时释放不发生交付倒序。
 */
static uint32_t _linkg_transport_reorder_take_ready(linkg_transport_reorder_frame_t *output, uint8_t *peer_node_id, uint64_t *deadline_us)
{
    linkg_transport_reorder_class_t *current;
    uint64_t                        now_us;
    static const linkg_transport_class_t priority_order[LINKG_TRANSPORT_REORDER_SERVICE_CYCLE] =
    {
        LINKG_TRANSPORT_CLASS_REALTIME,
        LINKG_TRANSPORT_CLASS_REALTIME,
        LINKG_TRANSPORT_CLASS_REALTIME,
        LINKG_TRANSPORT_CLASS_VIDEO,
        LINKG_TRANSPORT_CLASS_REALTIME,
        LINKG_TRANSPORT_CLASS_VIDEO,
        LINKG_TRANSPORT_CLASS_DATA
    };
    uint32_t                        order_index;
    uint32_t                        class_index;
    uint32_t                        peer_index;
    uint32_t                        peer_offset;
    uint32_t                        count;
    uint32_t                        turn;

    now_us       = linkg_time_monotonic_us();
    *deadline_us = 0U;

    for (turn = 0U; turn < LINKG_TRANSPORT_REORDER_SERVICE_CYCLE; turn++)
    {
        order_index = (g_reorder.service_turn + turn) % LINKG_TRANSPORT_REORDER_SERVICE_CYCLE;
        class_index = (uint32_t)priority_order[order_index];

        for (peer_offset = 0U; peer_offset < LINKG_TRANSPORT_PEER_MAX; peer_offset++)
        {
            peer_index = (g_reorder.next_peer_index[class_index] + peer_offset) % LINKG_TRANSPORT_PEER_MAX;
            current = &g_reorder.classes[peer_index][class_index];

            if (current->direct_count > 0U)
            {
                count = _linkg_transport_reorder_drain_direct(current, output);
                *peer_node_id = output[0].peer_node_id;
                g_reorder.service_turn = (order_index + 1U) % LINKG_TRANSPORT_REORDER_SERVICE_CYCLE;
                g_reorder.next_peer_index[class_index] = (peer_index + 1U) % LINKG_TRANSPORT_PEER_MAX;
                return count;
            }

            if (!current->redundant || class_index == LINKG_TRANSPORT_CLASS_REALTIME || current->slot_count == 0U)
            {
                continue;
            }

            count = _linkg_transport_reorder_drain_window(current, output, LINKG_TRANSPORT_REORDER_BATCH_MAX, now_us, deadline_us);
            if (count > 0U)
            {
                *peer_node_id = output[0].peer_node_id;
                g_reorder.service_turn = (order_index + 1U) % LINKG_TRANSPORT_REORDER_SERVICE_CYCLE;
                g_reorder.next_peer_index[class_index] = (peer_index + 1U) % LINKG_TRANSPORT_PEER_MAX;
                return count;
            }
        }
    }

    return 0U;
}

/**
 * @brief Worker等待最新入队信号或最早重排缺口到期。
 */
static void _linkg_transport_reorder_wait_locked(uint64_t deadline_us)
{
    struct timespec deadline;

    if (deadline_us == 0U)
    {
        (void)pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
        return;
    }

    deadline.tv_sec  = (time_t)(deadline_us / 1000000ULL);
    deadline.tv_nsec = (long)((deadline_us % 1000000ULL) * 1000ULL);

    (void)pthread_cond_timedwait(&g_reorder.condition, &g_reorder.lock, &deadline);
}

/****************************** Worker ******************************/

/**
 * @brief 统一串行化有序交付和超时释放，不在持锁期间调用业务Handler。
 */
static void _linkg_transport_reorder_worker(linkg_thread_t *thread, void *user_data)
{
    linkg_transport_reorder_frame_t frames[LINKG_TRANSPORT_REORDER_BATCH_MAX];
    uint64_t                       deadline_us;
    uint32_t                       count;
    uint32_t                       index;
    uint8_t                        peer_node_id;

    (void)thread;
    (void)user_data;

    pthread_mutex_lock(&g_reorder.lock);

    while (g_reorder.running)
    {
        peer_node_id = 0U;
        count        = _linkg_transport_reorder_take_ready(frames, &peer_node_id, &deadline_us);

        if (count == 0U)
        {
            _linkg_transport_reorder_wait_locked(deadline_us);
            continue;
        }

        g_reorder.processing_peer_node_id = peer_node_id;
        pthread_mutex_unlock(&g_reorder.lock);

        g_reorder.emit(frames, count);

        for (index = 0U; index < count; index++)
        {
            _linkg_transport_reorder_release_frame(&frames[index]);
        }

        pthread_mutex_lock(&g_reorder.lock);
        g_reorder.processing_peer_node_id = 0U;
        pthread_cond_broadcast(&g_reorder.condition);
    }

    pthread_mutex_unlock(&g_reorder.lock);
}

/****************************** 生命周期 ******************************/

int linkg_transport_reorder_runtime_init(linkg_transport_reorder_emit_func_t emit)
{
    pthread_condattr_t attributes;
    int                ret;

    if (emit == NULL)
    {
        return -EINVAL;
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
        linkg_thread_deinit(&g_reorder.thread);
        pthread_cond_destroy(&g_reorder.condition);
        pthread_mutex_destroy(&g_reorder.lock);
        return ret;
    }

    return 0;
}

int linkg_transport_reorder_runtime_deinit(void)
{
    uint32_t peer_index;
    uint32_t class_index;
    int      ret;

    if (!g_reorder.initialized)
    {
        return 0;
    }

    pthread_mutex_lock(&g_reorder.lock);
    g_reorder.running = false;
    pthread_cond_broadcast(&g_reorder.condition);
    pthread_mutex_unlock(&g_reorder.lock);

    ret = linkg_thread_stop(&g_reorder.thread);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_reorder.lock);

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            _linkg_transport_reorder_clear_class(&g_reorder.classes[peer_index][class_index]);
        }
    }

    g_reorder.initialized = false;
    pthread_mutex_unlock(&g_reorder.lock);

    linkg_thread_deinit(&g_reorder.thread);
    pthread_cond_destroy(&g_reorder.condition);
    pthread_mutex_destroy(&g_reorder.lock);

    return 0;
}

/**
 * @brief 判断调用方是否为Reorder自身的交付线程，防止Handler回调中执行阻塞式Peer重置。
 */
bool linkg_transport_reorder_is_worker_thread(void)
{
    if (!g_reorder.initialized || !linkg_thread_is_started(&g_reorder.thread))
    {
        return false;
    }

    return pthread_equal(pthread_self(), g_reorder.thread.tid) != 0;
}

int linkg_transport_reorder_reset_peer(uint8_t peer_node_id)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        class_index;
    uint32_t                        peer_index;

    if (!g_reorder.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_reorder.lock);

    if (g_reorder.processing_peer_node_id == peer_node_id &&
        pthread_equal(pthread_self(), g_reorder.thread.tid))
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return -EDEADLK;
    }

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            current = &g_reorder.classes[peer_index][class_index];

            if (current->peer_node_id == peer_node_id)
            {
                _linkg_transport_reorder_clear_class(current);
            }
        }
    }

    while (g_reorder.processing_peer_node_id == peer_node_id)
    {
        pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
    }

    pthread_cond_broadcast(&g_reorder.condition);
    pthread_mutex_unlock(&g_reorder.lock);

    return 0;
}

/****************************** 数据接收 ******************************/

/**
 * @brief SINGLE模式仅同步序号基线并清理上一轮冗余缓存，不接管Packet。
 *
 * @note 若Worker正在交付同一Peer的数据，则等待交付结束后切换到同步接收路径。
 */
int linkg_transport_reorder_note_single(linkg_transport_peer_t *peer, uint32_t peer_epoch, uint8_t peer_node_id, linkg_transport_class_t traffic_class, uint32_t sequence)
{
    linkg_transport_reorder_class_t *current;
    uint32_t                        last_sequence;
    int                             peer_index;
    int                             ret;

    if (peer == NULL || peer_node_id == 0U || !linkg_transport_class_valid(traffic_class))
    {
        return -EINVAL;
    }

    peer_index = _linkg_transport_reorder_peer_index(peer);
    if (peer_index < 0)
    {
        return peer_index;
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

    if (!linkg_transport_peer_epoch_matches(peer, peer_epoch))
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return -ESTALE;
    }

    if (g_reorder.processing_peer_node_id == peer_node_id &&
        pthread_equal(pthread_self(), g_reorder.thread.tid))
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return -EDEADLK;
    }

    while (g_reorder.processing_peer_node_id == peer_node_id)
    {
        ret = pthread_cond_wait(&g_reorder.condition, &g_reorder.lock);
        if (ret != 0)
        {
            pthread_mutex_unlock(&g_reorder.lock);
            return -ret;
        }
    }

    if (!g_reorder.running || !linkg_transport_peer_epoch_matches(peer, peer_epoch))
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return -ESTALE;
    }

    current = &g_reorder.classes[peer_index][traffic_class];

    if (current->peer_epoch != peer_epoch || current->peer_node_id != peer_node_id)
    {
        _linkg_transport_reorder_clear_class(current);
        current->peer_epoch   = peer_epoch;
        current->peer_node_id = peer_node_id;
    }

    last_sequence = sequence;

    if (current->single_initialized &&
        _linkg_transport_reorder_before(last_sequence, current->last_single_sequence))
    {
        last_sequence = current->last_single_sequence;
    }

    if (current->redundant)
    {
        if (current->expected_initialized &&
            _linkg_transport_reorder_before(last_sequence, current->expected_sequence - 1U))
        {
            last_sequence = current->expected_sequence - 1U;
        }

        if (current->realtime_initialized &&
            _linkg_transport_reorder_before(last_sequence, current->realtime_highest))
        {
            last_sequence = current->realtime_highest;
        }

        // 丢弃上一轮冗余未交付的帧，避免其晚于SINGLE直通数据交付。
        _linkg_transport_reorder_clear_class(current);
        current->peer_epoch   = peer_epoch;
        current->peer_node_id = peer_node_id;
    }

    current->last_single_sequence = last_sequence;
    current->single_initialized   = true;
    current->redundant            = false;

    if (traffic_class == LINKG_TRANSPORT_CLASS_REALTIME)
    {
        current->realtime_highest     = last_sequence;
        current->realtime_initialized = true;
    }

    pthread_mutex_unlock(&g_reorder.lock);

    return 0;
}

/**
 * @brief 只有冗余模式的本机USER_DATA允许提交Reorder。
 */
int linkg_transport_reorder_submit(const linkg_transport_reorder_frame_t *frame)
{
    linkg_transport_reorder_class_t *current;
    linkg_transport_reorder_frame_t *slot;
    uint32_t                        distance;
    uint32_t                        index;
    int                             peer_index;
    int                             ret;

    if (frame == NULL || frame->packet == NULL || frame->header.type != LINKG_TRANSPORT_TYPE_USER_DATA ||
        !frame->redundant || !linkg_transport_class_valid(frame->traffic_class))
    {
        return -EINVAL;
    }

    peer_index = _linkg_transport_reorder_peer_index(frame->peer);
    if (peer_index < 0)
    {
        return peer_index;
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

    if (!linkg_transport_peer_epoch_matches(frame->peer, frame->peer_epoch))
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return -ESTALE;
    }

    current = &g_reorder.classes[peer_index][frame->traffic_class];

    if (current->peer_epoch != frame->peer_epoch || current->peer_node_id != frame->peer_node_id)
    {
        _linkg_transport_reorder_clear_class(current);
        current->peer_epoch   = frame->peer_epoch;
        current->peer_node_id = frame->peer_node_id;
    }

    if (frame->traffic_class == LINKG_TRANSPORT_CLASS_REALTIME)
    {
        current->redundant = true;

        if (current->direct_count >= LINKG_TRANSPORT_REORDER_DIRECT_QUEUE_SIZE)
        {
            pthread_mutex_unlock(&g_reorder.lock);
            return -ENOBUFS;
        }

        index = (current->direct_head + current->direct_count) % LINKG_TRANSPORT_REORDER_DIRECT_QUEUE_SIZE;
        current->direct[index] = *frame;
        linkg_packet_retain(frame->packet);
        current->direct_count++;
    }
    else
    {
        if (!current->redundant)
        {
            current->redundant           = true;
            current->wait_start_us       = 0U;
            current->expected_initialized = false;
        }

        if (!current->expected_initialized)
        {
            if (current->single_initialized)
            {
                current->expected_sequence = current->last_single_sequence + 1U;
            }
            else
            {
                // 每个新Peer/Class的USER发送序号从1开始；不能把首个乱序到达帧当作起点。
                current->expected_sequence = 1U;
            }

            current->expected_initialized = true;
        }

        if (_linkg_transport_reorder_before(frame->header.sequence, current->expected_sequence))
        {
            pthread_mutex_unlock(&g_reorder.lock);
            return -ERANGE;
        }

        distance = frame->header.sequence - current->expected_sequence;
        if (distance >= LINKG_TRANSPORT_REORDER_WINDOW_SIZE)
        {
            _linkg_transport_reorder_advance_window(current, frame->header.sequence);
        }

        index = frame->header.sequence & (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U);
        slot  = &current->slots[index];

        if (slot->packet != NULL)
        {
            pthread_mutex_unlock(&g_reorder.lock);
            return -EALREADY;
        }

        *slot = *frame;
        linkg_packet_retain(frame->packet);
        current->slot_count++;
    }

    pthread_cond_signal(&g_reorder.condition);
    pthread_mutex_unlock(&g_reorder.lock);

    return 0;
}

/**
 * @brief 串行Worker过滤冗余REALTIME完整Packet的旧序号。
 */
bool linkg_transport_reorder_realtime_accept(const linkg_transport_reorder_frame_t *frame, uint32_t complete_sequence)
{
    linkg_transport_reorder_class_t *current;
    bool                            accepted;
    int                             peer_index;

    if (frame == NULL || frame->traffic_class != LINKG_TRANSPORT_CLASS_REALTIME)
    {
        return false;
    }

    peer_index = _linkg_transport_reorder_peer_index(frame->peer);
    if (peer_index < 0)
    {
        return false;
    }

    pthread_mutex_lock(&g_reorder.lock);

    current = &g_reorder.classes[peer_index][LINKG_TRANSPORT_CLASS_REALTIME];

    if (current->peer_epoch != frame->peer_epoch || current->peer_node_id != frame->peer_node_id)
    {
        pthread_mutex_unlock(&g_reorder.lock);
        return false;
    }

    accepted = !current->realtime_initialized || _linkg_transport_reorder_before(current->realtime_highest, complete_sequence);

    if (accepted && (!current->realtime_initialized || _linkg_transport_reorder_before(current->realtime_highest, complete_sequence)))
    {
        current->realtime_highest     = complete_sequence;
        current->realtime_initialized = true;
    }

    pthread_mutex_unlock(&g_reorder.lock);

    return accepted;
}
