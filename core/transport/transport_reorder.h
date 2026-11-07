/**
 * @file transport_reorder.h
 * @brief LinkG本机USER_DATA接收顺序管理内部接口
 */

#ifndef TRANSPORT_REORDER_H
#define TRANSPORT_REORDER_H

#include "transport_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX 32U // 单次最大输入帧数
#define LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX   32U // 单次最大回调输出帧数
#define LINKG_TRANSPORT_REORDER_WINDOW_SIZE      64U // 每Peer/Class独立Sequence窗口

/****************************** Reorder帧 ******************************/

typedef struct
{
    linkg_packet_t                    *packet;          // 完整Transport Wire Frame，输入时由RX持有基础引用
    linkg_transport_peer_t            *peer;            // 当前直接Peer固定槽位，仅借用
    linkg_transport_header_t           header;          // 已验证的Transport基础头
    linkg_transport_fragment_header_t  fragment_header; // 已验证的分片扩展头
    uint32_t                           ingress_link_id; // 物理接收Link ID
    uint32_t                           peer_epoch;      // RX Window确认的Peer生命周期代际
    linkg_transport_class_t            traffic_class;   // 业务类别
    uint8_t                            peer_node_id;    // 直接Peer节点编号
} linkg_transport_reorder_frame_t;

/****************************** 提交结果 ******************************/

typedef enum
{
    LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED   = 0, // 尚未处理，仅用于结果初始化
    LINKG_TRANSPORT_REORDER_RESULT_READY,             // 由当前RX快速路径继续交付，须在Handler完成后调用complete_batch
    LINKG_TRANSPORT_REORDER_RESULT_CACHED,            // Reorder已保留Packet独立引用，当前RX不可再次交付
    LINKG_TRANSPORT_REORDER_RESULT_EMITTED,           // submit_batch返回前已执行同步Emit回调，当前RX不可再次交付
    LINKG_TRANSPORT_REORDER_RESULT_TOO_OLD,           // 已交付、已跳过或已缓存的重复Sequence
    LINKG_TRANSPORT_REORDER_RESULT_STALE,             // Peer代际或节点身份失效
    LINKG_TRANSPORT_REORDER_RESULT_OUT_OF_WINDOW      // 前向Sequence不在64槽窗口内，未接管Packet
} linkg_transport_reorder_result_t;

/****************************** 数据交付 ******************************/

typedef void (*linkg_transport_reorder_emit_func_t)(const linkg_transport_reorder_frame_t *frames, uint32_t count);

/****************************** 生命周期 ******************************/

int  linkg_transport_reorder_runtime_init(linkg_transport_reorder_emit_func_t emit);
int  linkg_transport_reorder_runtime_deinit(void);
int  linkg_transport_reorder_reset_peer(uint8_t peer_node_id);
int  linkg_transport_reorder_check_lifecycle_context(void);
bool linkg_transport_reorder_is_worker_thread(void);

/****************************** 批量接收 ******************************/

int linkg_transport_reorder_submit_batch(const linkg_transport_reorder_frame_t *frames, uint32_t count, linkg_transport_reorder_result_t *results);

/****************************** 交付完成 ******************************/

/**
 * @brief 当前RX批次中READY帧的Handler交付完成后释放Peer/Class交付占用。
 * @note 必须在原始RX的本机Handler全部返回后调用；不得在submit_batch返回后立即调用。
 */
int linkg_transport_reorder_complete_batch(const linkg_transport_reorder_frame_t *frames, const linkg_transport_reorder_result_t *results, uint32_t count);

/****************************** 接收交付 ******************************/

void linkg_transport_rx_emit_reordered(const linkg_transport_reorder_frame_t *frames, uint32_t count);

#endif
