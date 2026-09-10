/**
 * @file linkg_transport_types.h
 * @brief LinkG传输协议公共类型定义
 */

#ifndef LINKG_TRANSPORT_TYPES_H
#define LINKG_TRANSPORT_TYPES_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 协议定义 ******************************/

#define LINKG_TRANSPORT_WIRE_MAGIC                 0x4C4B4732U // 协议魔数，ASCII: LKG2
#define LINKG_TRANSPORT_WIRE_VERSION               1U          // 当前协议版本
#define LINKG_TRANSPORT_WIRE_HEADER_SIZE           16U         // 固定Transport基础头长度
#define LINKG_TRANSPORT_WIRE_FRAGMENT_HEADER_SIZE  8U          // Transport分片扩展头长度
#define LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE       24U         // Transport最大头部长度
#define LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE        1452U       // 单个Transport帧最大长度
#define LINKG_TRANSPORT_PAYLOAD_MAX_SIZE           (LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE - LINKG_TRANSPORT_WIRE_HEADER_SIZE) // 非分片最大载荷长度
#define LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE  (LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE - LINKG_TRANSPORT_WIRE_HEADER_SIZE - LINKG_TRANSPORT_WIRE_FRAGMENT_HEADER_SIZE) // 分片最大载荷长度
#define LINKG_TRANSPORT_PACKET_MAX_SIZE            1500U       // Transport支持的最大原始完整数据包长度

/****************************** 帧标志 ******************************/

#define LINKG_TRANSPORT_FLAG_NONE                  0x0000U                         // 无附加标志
#define LINKG_TRANSPORT_FLAG_FRAGMENT              0x0001U                         // 当前帧属于LinkG内部分片
#define LINKG_TRANSPORT_FLAG_VALID_MASK            LINKG_TRANSPORT_FLAG_FRAGMENT   // 当前有效帧标志掩码

/****************************** 业务类别 ******************************/

typedef enum
{
    LINKG_TRANSPORT_CLASS_REALTIME = 0, // 实时业务
    LINKG_TRANSPORT_CLASS_VIDEO,        // 视频业务
    LINKG_TRANSPORT_CLASS_DATA,         // 普通数据
    LINKG_TRANSPORT_CLASS_COUNT         // 业务类别数量
} linkg_transport_class_t;


/****************************** 帧类型 ******************************/

typedef enum
{
    LINKG_TRANSPORT_TYPE_NONE       = 0, // 无效类型
    LINKG_TRANSPORT_TYPE_USER_DATA,      // 用户业务数据
    LINKG_TRANSPORT_TYPE_CONTROL,        // 控制数据
    LINKG_TRANSPORT_TYPE_STATISTICS,     // 统计数据
    LINKG_TRANSPORT_TYPE_PING,           // 链路探测数据
    LINKG_TRANSPORT_TYPE_SWITCH,         // 链路切换数据
    LINKG_TRANSPORT_TYPE_COUNT           // 类型数量
} linkg_transport_type_t;

/****************************** 帧头 ******************************/

/**
 * @brief Transport基础帧头。
 *
 * source_node_id表示原始发送节点，
 * destination_node_id表示最终目标节点，与当前物理下一跳Peer无关。
 */
typedef struct
{
    uint32_t magic;               // 协议魔数
    uint8_t  version;             // 协议版本
    uint8_t  type;                // 数据类型
    uint16_t flags;               // 帧标志
    uint32_t sequence;            // 当前一跳序列号
    uint8_t  source_node_id;      // 原始发送节点编号
    uint8_t  destination_node_id; // 最终目标节点编号
    uint16_t reserved;            // 保留字段，发送时必须置零
} linkg_transport_header_t;

/**
 * @brief Transport分片扩展头。
 */
typedef struct
{
    uint32_t packet_id;       // 原始完整数据包编号
    uint16_t fragment_offset; // 当前分片在原始数据包中的字节偏移
    uint16_t packet_length;   // 原始完整数据包长度
} linkg_transport_fragment_header_t;

/****************************** 协议约束 ******************************/

_Static_assert(sizeof(linkg_transport_header_t) == LINKG_TRANSPORT_WIRE_HEADER_SIZE, "invalid transport header size");
_Static_assert(sizeof(linkg_transport_fragment_header_t) == LINKG_TRANSPORT_WIRE_FRAGMENT_HEADER_SIZE, "invalid transport fragment header size");
_Static_assert(LINKG_TRANSPORT_PAYLOAD_MAX_SIZE == 1436U, "invalid transport payload size");
_Static_assert(LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE == 1428U, "invalid transport fragment payload size");

#ifdef __cplusplus
}
#endif

#endif
