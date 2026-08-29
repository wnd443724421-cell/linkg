/**
 * @file linkg_transport_internal.h
 * @brief LinkG传输层内部定义
 */

#ifndef LINKG_TRANSPORT_INTERNAL_H
#define LINKG_TRANSPORT_INTERNAL_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_system_resources.h"
#include "linkg_transport.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 模块常量 ******************************/

#define LINKG_TRANSPORT_PEER_MAX                   LINKG_RESOURCE_NETWORK_STA_MAX                 // 最大直接Peer数量
#define LINKG_TRANSPORT_RX_WINDOW_BITS             512U                                           // 接收序列号去重窗口大小
#define LINKG_TRANSPORT_FORWARD_BATCH_MAX          32U                                            // 单次同步转发最大包数
#define LINKG_TRANSPORT_PACKET_ID_INVALID          0U                                             // 无效原始数据包编号
#define LINKG_TRANSPORT_FRAGMENT_COUNT_MAX         2U                                             // 当前协议最大分片数量
#define LINKG_TRANSPORT_REASSEMBLY_SET_COUNT       64U                                            // 重组缓存组数
#define LINKG_TRANSPORT_REASSEMBLY_WAYS            2U                                             // 重组缓存组相联路数
#define LINKG_TRANSPORT_REASSEMBLY_TTL_US          20000ULL                                       // 不完整数据包最大保留时间
#define LINKG_TRANSPORT_REASSEMBLY_GC_INTERVAL_US  5000ULL                                        // 重组缓存清理周期
#define LINKG_TRANSPORT_RX_WINDOW_WORDS            (LINKG_TRANSPORT_RX_WINDOW_BITS / 64U)         // 接收窗口位图字数量

/****************************** 接收窗口 ******************************/

typedef enum
{
    LINKG_TRANSPORT_WINDOW_INVALID   = -1, // 无效参数
    LINKG_TRANSPORT_WINDOW_ACCEPT    = 0,  // 首次收到，允许处理
    LINKG_TRANSPORT_WINDOW_DUPLICATE,      // 重复帧
    LINKG_TRANSPORT_WINDOW_TOO_OLD         // 超出窗口的旧帧
} linkg_transport_window_result_t;

typedef struct
{
    uint64_t received_bitmap[LINKG_TRANSPORT_RX_WINDOW_WORDS]; // 滑动窗口接收位图
    uint32_t highest_sequence;                                 // 当前最高序列号
    bool     initialized;                                      // 是否已经接收过数据
} linkg_transport_rx_window_t;

/****************************** Peer状态 ******************************/

typedef struct
{
    linkg_transport_rx_window_t  rx_window;    // 当前Peer接收去重窗口
    linkg_transport_peer_stats_t stats;        // 当前Peer逻辑累计统计
    uint32_t                     tx_sequence;  // 当前Peer逐跳发送序列号
    uint8_t                      peer_node_id; // 直接Peer节点编号
    bool                         valid;        // Peer状态是否有效
} linkg_transport_peer_t;

/****************************** 类型交付 ******************************/

typedef struct
{
    linkg_transport_handler_func_t handler;   // 类型处理函数
    void                          *user_data; // 处理私有数据
} linkg_transport_handler_t;

/****************************** 分片重组 ******************************/

typedef struct
{
    linkg_packet_t         *first_packet;         // 首片Packet，持有一个引用并作为最终完整数据包载体
    linkg_packet_t         *tail_packet;          // 尾片先到时临时缓存，持有一个引用
    uint64_t                expires_at_us;        // 重组项过期时间
    uint32_t                packet_id;            // 原始完整数据包编号
    uint16_t                packet_length;        // 原始完整数据包长度
    linkg_transport_type_t  type;                 // Transport数据类型
    uint8_t                 source_node_id;       // 原始发送节点编号
    uint8_t                 destination_node_id;  // 最终目标节点编号
    uint8_t                 received_mask;        // 已收到分片位图
    bool                    valid;                // 重组项是否有效
} linkg_transport_reassembly_entry_t;

typedef struct
{
    pthread_mutex_t                    lock;                                                        // 重组缓存保护锁
    linkg_transport_reassembly_entry_t entries[LINKG_TRANSPORT_REASSEMBLY_SET_COUNT][LINKG_TRANSPORT_REASSEMBLY_WAYS]; // 固定重组缓存
    uint64_t                           last_gc_us;                                                   // 最近一次全局清理时间
    bool                               initialized;                                                  // 重组资源是否已初始化
} linkg_transport_reassembly_runtime_t;

typedef struct
{
    const linkg_transport_header_t          *header;            // Transport基础头
    const linkg_transport_fragment_header_t *fragment_header;   // Transport分片扩展头
    linkg_packet_t                          *packet;            // 当前Transport分片帧
    linkg_packet_t                          *completed_packet;  // 完整重组Packet，result为1时有效
    int                                      result;            // 1完成，0等待，<0非法
} linkg_transport_reassembly_submit_item_t;

/****************************** AP转发 ******************************/

typedef struct
{
    linkg_packet_t        *packet;              // 完整Transport帧，同步借用Link RX基础引用
    uint32_t               payload_length;      // 当前帧实际Transport载荷长度
    linkg_transport_type_t type;                // Transport数据类型
    uint8_t                destination_node_id; // 最终目标节点编号
} linkg_transport_forward_item_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t                      lock;                                 // Transport状态保护锁
    linkg_transport_peer_t               peers[LINKG_TRANSPORT_PEER_MAX];      // 直接Peer协议状态
    linkg_transport_handler_t            handlers[LINKG_TRANSPORT_TYPE_COUNT]; // 类型处理函数
    linkg_transport_global_stats_t       stats;                                // 全局异常统计
    linkg_transport_reassembly_runtime_t reassembly;                           // 本机分片重组资源
    uint32_t                             next_packet_id;                       // 下一个原始完整数据包编号
    uint32_t                             peer_count;                           // 当前有效Peer数量
    linkg_device_role_t                  local_role;                           // 本机角色
    uint8_t                              local_node_id;                        // 本机节点编号
    bool                                 initialized;                          // 模块是否已初始化
} linkg_transport_context_t;

extern linkg_transport_context_t g_transport;

/****************************** 协议处理 ******************************/

int linkg_transport_wire_encode(linkg_packet_t *packet, const linkg_transport_header_t *header);
int linkg_transport_wire_decode(const linkg_packet_t *packet, linkg_transport_header_t *header);
int linkg_transport_wire_fragment_encode(linkg_packet_t *packet, const linkg_transport_fragment_header_t *header);
int linkg_transport_wire_fragment_decode(const linkg_packet_t *packet, linkg_transport_fragment_header_t *header);
int linkg_transport_wire_update_sequence(linkg_packet_t *packet, uint32_t sequence);

/****************************** 窗口处理 ******************************/

void                            linkg_transport_window_reset(linkg_transport_rx_window_t *window);
linkg_transport_window_result_t linkg_transport_window_accept(linkg_transport_rx_window_t *window, uint32_t sequence);

/****************************** 分片重组 ******************************/

int linkg_transport_reassembly_runtime_init(void);
int linkg_transport_reassembly_runtime_deinit(void);
int linkg_transport_reassembly_submit(const linkg_transport_header_t *header, const linkg_transport_fragment_header_t *fragment_header, linkg_packet_t *packet, linkg_packet_t **completed_packet);
int linkg_transport_reassembly_submit_batch(linkg_transport_reassembly_submit_item_t *items, uint32_t count);

/****************************** 内部辅助 ******************************/

linkg_transport_peer_t *linkg_transport_find_peer_locked(uint8_t peer_node_id);
bool                    linkg_transport_type_valid(linkg_transport_type_t type);

/****************************** 发送辅助 ******************************/

int  linkg_transport_tx_prepare(uint8_t destination_node_id, uint8_t *next_hop_node_id, uint32_t *sequence);
void linkg_transport_tx_record(uint8_t peer_node_id, linkg_transport_type_t type, uint32_t payload_length, bool success);

/****************************** 转发辅助 ******************************/

int linkg_transport_forward_batch(const linkg_transport_forward_item_t *items, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
