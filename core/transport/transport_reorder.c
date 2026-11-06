
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

#include "linkg_packet_pool.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_REORDER_THREAD_NAME          "trans-reorder" // Reorder工作线程名称
#define LINKG_TRANSPORT_REORDER_WINDOW_SIZE          64U             // 每个Peer/Class的Sequence窗口大小
#define LINKG_TRANSPORT_REORDER_WINDOW_MASK          (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U) // 窗口索引掩码
#define LINKG_TRANSPORT_REORDER_WAIT_REALTIME_US     15000ULL        // REALTIME乱序最大等待时间
#define LINKG_TRANSPORT_REORDER_WAIT_VIDEO_US        60000ULL        // VIDEO乱序最大等待时间
#define LINKG_TRANSPORT_REORDER_WAIT_DATA_US         100000ULL       // DATA乱序最大等待时间

_Static_assert((LINKG_TRANSPORT_REORDER_WINDOW_SIZE &
                (LINKG_TRANSPORT_REORDER_WINDOW_SIZE - 1U)) == 0U,
               "reorder window size must be power of two");

/****************************** 内部类型 ******************************/

/**
 * @brief Reorder窗口缓存槽。
 */
typedef struct
{
    linkg_transport_reorder_frame_t frame;        // 缓存Wire Frame，Reorder持有一个Packet引用
    uint64_t                        expires_at_us; // 当前缓存帧的固定过期时间
} linkg_transport_reorder_slot_t;

/**
 * @brief 单个Peer/Class的Reorder状态。
 */
typedef struct
{
    linkg_transport_reorder_slot_t slots[LINKG_TRANSPORT_REORDER_WINDOW_SIZE]; // 64个Sequence缓存槽

    uint32_t expected_sequence;       // 下一个应当交付的Sequence
    uint32_t ready_first_sequence;    // 当前快速交付批次的首个Sequence
    uint32_t ready_count;             // 当前快速交付批次的帧数量
    uint32_t slot_count;              // 当前缓存帧数量
    uint32_t peer_epoch;              // 当前Peer生命周期代际

    uint8_t peer_node_id;             // 当前直接Peer节点编号

    bool initialized;                 // 当前Peer/Class状态是否初始化
    bool expected_initialized;        // expected_sequence是否有效
    bool delivery_in_progress;        // 是否存在尚未完成的快速交付批次

} linkg_transport_reorder_class_t;

/**
 * @brief Reorder模块全局运行状态。
 */
typedef struct
{
    pthread_mutex_t lock;                                                                           // Reorder状态保护锁
    pthread_cond_t condition;                                                                       // Worker唤醒条件
    linkg_thread_t thread;                                                                          // Reorder工作线程

    linkg_transport_reorder_class_t classes[LINKG_TRANSPORT_PEER_MAX][LINKG_TRANSPORT_CLASS_COUNT]; // Peer/Class独立状态

    linkg_transport_reorder_emit_func_t emit;                           // 有序帧异步交付回调

    uint32_t next_peer_index[LINKG_TRANSPORT_CLASS_COUNT];              // 各Class下一次扫描Peer位置

    uint8_t processing_peer_node_id;                                    // 当前Worker正在交付的Peer

    bool worker_processing;                                             // Worker是否正在执行交付回调
    bool initialized;                                                   // 模块是否已经初始化
    bool running;                                                       // Worker是否继续运行

} linkg_transport_reorder_runtime_t;

/****************************** 模块状态 ******************************/

static linkg_transport_reorder_runtime_t g_reorder;

/****************************** 内部辅助 ******************************/

static bool _linkg_transport_reorder_sequence_before(uint32_t left, uint32_t right);

static int _linkg_transport_reorder_peer_index(const linkg_transport_peer_t *peer);

static uint64_t _linkg_transport_reorder_get_timeout_us(linkg_transport_class_t traffic_class);

static bool _linkg_transport_reorder_frame_is_fragmented(const linkg_transport_reorder_frame_t *frame);

static void _linkg_transport_reorder_release_frame(linkg_transport_reorder_frame_t *frame);

static void _linkg_transport_reorder_clear_class(linkg_transport_reorder_class_t *current);

static void _linkg_transport_reorder_clear_peer(uint8_t peer_node_id);

static linkg_transport_reorder_slot_t *_linkg_transport_reorder_find_slot(linkg_transport_reorder_class_t *current, uint32_t sequence);

static uint32_t _linkg_transport_reorder_drain_class(linkg_transport_reorder_class_t *current,
                                                      linkg_transport_reorder_frame_t *output,
                                                      uint32_t capacity,
                                                      uint64_t now_us,
                                                      uint64_t *deadline_us);

static uint32_t _linkg_transport_reorder_take_ready(linkg_transport_reorder_frame_t *output,
                                                     uint32_t capacity,
                                                     uint8_t *peer_node_id,
                                                     uint64_t *deadline_us);

static void _linkg_transport_reorder_wait_locked(uint64_t deadline_us);

/****************************** Worker ******************************/

/**
 * @brief Reorder统一工作线程。
 */
static void _linkg_transport_reorder_worker(linkg_thread_t *thread, void *user_data)
{
    (void)thread;
    (void)user_data;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Reorder运行环境。
 */
int linkg_transport_reorder_runtime_init(linkg_transport_reorder_emit_func_t emit)
{
    (void)emit;

    return -ENOSYS;
}

/**
 * @brief 停止Reorder并释放全部缓存资源。
 */
int linkg_transport_reorder_runtime_deinit(void)
{
    return -ENOSYS;
}

/**
 * @brief 清理指定Peer的全部Class状态。
 */
int linkg_transport_reorder_reset_peer(uint8_t peer_node_id)
{
    (void)peer_node_id;

    return -ENOSYS;
}

/**
 * @brief 判断当前线程是否为Reorder Worker。
 */
bool linkg_transport_reorder_is_worker_thread(void)
{
    return false;
}

/****************************** 批量接收 ******************************/

/**
 * @brief 批量提交USER_DATA Wire Frame并返回逐帧处理结果。
 */
int linkg_transport_reorder_submit_batch(const linkg_transport_reorder_frame_t *frames, uint32_t count, linkg_transport_reorder_result_t *results)
{
    (void)frames;
    (void)count;
    (void)results;

    return -ENOSYS;
}

/****************************** 批量完成 ******************************/

/**
 * @brief 完成当前RX批次的READY快速交付并释放交付占用。
 */
int linkg_transport_reorder_complete_batch(const linkg_transport_reorder_frame_t *frames, const linkg_transport_reorder_result_t *results, uint32_t count)
{
    (void)frames;
    (void)results;
    (void)count;

    return -ENOSYS;
}
