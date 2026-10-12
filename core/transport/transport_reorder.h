/**
 * @file transport_reorder.h
 * @brief LinkG本机用户数据接收重排内部接口
 */

#ifndef TRANSPORT_REORDER_H
#define TRANSPORT_REORDER_H

#include "transport_internal.h"

/****************************** Reorder帧 ******************************/

typedef struct
{
    linkg_packet_t                    *packet;          // 完整Transport Wire Frame，提交时由Reorder保留引用
    linkg_transport_peer_t            *peer;            // 当前直接Peer固定槽位，仅借用
    linkg_transport_header_t           header;          // 已验证的基础头快照
    linkg_transport_fragment_header_t  fragment_header; // 分片扩展头快照
    linkg_transport_class_t            traffic_class;   // 业务类别
    uint32_t                           ingress_link_id; // 物理接收Link ID
    uint32_t                           peer_epoch;      // 接收时的Peer代际
    uint8_t                            peer_node_id;    // 直接Peer节点编号
    bool                               fragmented;      // 是否分片
    bool                               redundant;       // 提交时采用冗余计划
} linkg_transport_reorder_frame_t;

typedef void (*linkg_transport_reorder_emit_func_t)(const linkg_transport_reorder_frame_t *frames, uint32_t count);

/****************************** 生命周期 ******************************/

int  linkg_transport_reorder_runtime_init(linkg_transport_reorder_emit_func_t emit);
int  linkg_transport_reorder_runtime_deinit(void);
int  linkg_transport_reorder_reset_peer(uint8_t peer_node_id);
bool linkg_transport_reorder_is_worker_thread(void);
int  linkg_transport_reorder_note_single(linkg_transport_peer_t *peer, uint32_t peer_epoch, uint8_t peer_node_id, linkg_transport_class_t traffic_class, uint32_t sequence);

/****************************** 数据接收 ******************************/

void linkg_transport_rx_emit_reordered(const linkg_transport_reorder_frame_t *frames, uint32_t count);

int  linkg_transport_reorder_submit(const linkg_transport_reorder_frame_t *frame);
bool linkg_transport_reorder_realtime_accept(const linkg_transport_reorder_frame_t *frame, uint32_t complete_sequence);

#endif
