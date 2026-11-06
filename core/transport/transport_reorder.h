/**
 * @file transport_reorder.h
 * @brief LinkG本机USER_DATA接收顺序管理内部接口
 */

#ifndef TRANSPORT_REORDER_H
#define TRANSPORT_REORDER_H

#include "transport_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_REORDER_SUBMIT_BATCH_MAX  32U // 单次最大输入帧数
#define LINKG_TRANSPORT_REORDER_EMIT_BATCH_MAX    32U // 单次最大输出帧数

/****************************** Reorder帧 ******************************/

/**
 * @brief Transport Reorder输入及输出帧。
 *
 * @note Packet保持完整Transport Wire格式。
 *       Reorder仅管理Sequence顺序，不修改Wire Frame内容。
 */
typedef struct
{
    linkg_packet_t                    *packet;          // Transport Wire Frame
    linkg_transport_peer_t            *peer;            // 当前直接Peer固定槽位，仅借用
    linkg_transport_header_t           header;          // 已验证的Transport基础头
    linkg_transport_fragment_header_t  fragment_header; // 已验证的分片扩展头

    uint32_t                           ingress_link_id; // 物理接收Link ID
    uint32_t                           peer_epoch;      // Peer生命周期代际

    linkg_transport_class_t            traffic_class;   // 业务类别
    uint8_t                            peer_node_id;    // 直接Peer节点编号
} linkg_transport_reorder_frame_t;

/****************************** 提交结果 ******************************/

/**
 * @brief 单个Wire Frame经过Reorder后的处理结果。
 */
typedef enum
{
    LINKG_TRANSPORT_REORDER_RESULT_UNPROCESSED = 0, // 尚未处理
    LINKG_TRANSPORT_REORDER_RESULT_READY,           // 可以由当前RX批次直接交付
    LINKG_TRANSPORT_REORDER_RESULT_CACHED,          // 已进入Reorder缓存
    LINKG_TRANSPORT_REORDER_RESULT_EMITTED,         // 已通过Emit回调交付
    LINKG_TRANSPORT_REORDER_RESULT_TOO_OLD,         // 已落后于交付基线
    LINKG_TRANSPORT_REORDER_RESULT_STALE            // Peer生命周期失效
} linkg_transport_reorder_result_t;

/****************************** 数据交付 ******************************/

/**
 * @brief Reorder有序帧输出回调。
 *
 * @note 仅输出已经满足交付条件的Wire Frame。
 *       回调期间Packet引用由Reorder保证有效。
 */
typedef void (*linkg_transport_reorder_emit_func_t)(const linkg_transport_reorder_frame_t *frames, uint32_t count);

/****************************** 生命周期 ******************************/

int  linkg_transport_reorder_runtime_init(linkg_transport_reorder_emit_func_t emit);
int  linkg_transport_reorder_runtime_deinit(void);
int  linkg_transport_reorder_reset_peer(uint8_t peer_node_id);
bool linkg_transport_reorder_is_worker_thread(void);

/****************************** 批量接收 ******************************/

/**
 * @brief 批量提交本机USER_DATA Wire Frame。
 *
 * @param frames  输入Wire Frame数组
 * @param count   输入帧数量
 * @param results 每帧独立处理结果
 *
 * @return 0表示批次处理完成，负值表示批次级错误。
 *
 * @note ACCEPTED表示Reorder已经接管该帧，
 *       后续可能直接交付，也可能缓存等待前序Sequence。
 */
int linkg_transport_reorder_submit_batch(const linkg_transport_reorder_frame_t *frames, uint32_t count, linkg_transport_reorder_result_t *results);

/****************************** 接收交付 ******************************/

void linkg_transport_rx_emit_reordered(const linkg_transport_reorder_frame_t *frames, uint32_t count);

#endif
