/**
 * @file cellular_wire.h
 * @brief LinkG Cellular链路私有Wire头
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-16
 */

#ifndef CELLULAR_WIRE_H
#define CELLULAR_WIRE_H

#include <stdint.h>

/****************************** Wire格式 ******************************/

/**
 * @brief Cellular IPv6/UDP私有头。
 *
 * session_id标识TX生命周期，sequence是每个Peer/Class成功提交Socket后的独立序号。
 * 所有多字节字段采用网络字节序。
 */
typedef struct __attribute__((packed))
{
    uint32_t session_id; // 发送端TX实例会话ID
    uint32_t sequence;   // 对应Peer/Class的传输序号
} linkg_cellular_wire_header_t;

#define LINKG_CELLULAR_WIRE_HEADER_SIZE ((uint32_t)sizeof(linkg_cellular_wire_header_t))

_Static_assert(sizeof(linkg_cellular_wire_header_t) == 8U, "invalid Cellular wire header size");

#endif
