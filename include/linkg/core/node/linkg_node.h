/**
 * @file linkg_node.h
 * @brief LinkG节点管理接口
 */

#ifndef LINKG_NODE_H
#define LINKG_NODE_H

#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 模块常量 ******************************/

#define LINKG_NODE_PEER_MAX 16U // 最大直接对端数量
#define LINKG_NODE_PATH_MAX 2U  // 单个直接对端最大路径数量

/****************************** 节点信息 ******************************/

typedef struct
{
    uint8_t             node_id; // LinkG节点编号
    linkg_device_role_t role;    // 节点角色
} linkg_node_info_t;

/****************************** 对端快照 ******************************/

typedef struct
{
    linkg_node_info_t info;       // 对端节点信息
    uint32_t          path_count; // 当前活动路径数量
} linkg_node_peer_snapshot_t;

/****************************** 接收统计 ******************************/

typedef struct
{
    linkg_path_endpoint_t source;       // 当前物理接收来源
    uint64_t              bytes;        // 当前来源批量接收字节数
    uint64_t              packets;      // 当前来源批量接收包数
    uint8_t               peer_node_id; // 输出：对应直接Peer节点编号
    int                   result;       // 当前元素处理结果
} linkg_node_path_rx_item_t;

/****************************** 生命周期 ******************************/

int linkg_node_init(const linkg_node_info_t *local);
int linkg_node_deinit(void);

/****************************** 本机查询 ******************************/

const linkg_node_info_t *linkg_node_get_local(void);

/****************************** 对端查询 ******************************/

int linkg_node_get_peer_snapshot(uint8_t node_id, linkg_node_peer_snapshot_t *snapshot);

/****************************** 对端管理 ******************************/

int linkg_node_register_peer(const linkg_node_info_t *info);
int linkg_node_unregister_peer(uint8_t node_id);

/****************************** 路径管理 ******************************/

int linkg_node_register_path(uint8_t node_id, uint32_t link_id, const linkg_path_endpoint_t *next_hop);
int linkg_node_unregister_path(uint8_t node_id, uint32_t link_id);
int linkg_node_acquire_path(uint8_t node_id, uint32_t link_id, linkg_path_t **path, linkg_path_endpoint_t *next_hop);
int linkg_node_acquire_path_batch(uint8_t node_id, uint32_t link_id, uint32_t reference_count, linkg_path_t **path, linkg_path_endpoint_t *next_hop);

/****************************** 接收统计 ******************************/

int linkg_node_account_path_rx(uint32_t link_id, const linkg_path_endpoint_t *source, uint64_t bytes, uint64_t packets, uint8_t *peer_node_id);
int linkg_node_account_path_rx_batch(uint32_t link_id, linkg_node_path_rx_item_t *items, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
