/**
 * @file transport_wire.c
 * @brief LinkG传输协议头处理
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-29
 */

#include "transport_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_system_resources.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 检查Transport节点编号是否合法。
 */
static bool _linkg_transport_wire_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN &&
           node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 检查Transport帧标志是否合法。
 */
static bool _linkg_transport_wire_flags_valid(uint16_t flags)
{
    return (flags & (uint16_t)~LINKG_TRANSPORT_FLAG_VALID_MASK) == 0U;
}

/**
 * @brief 检查固定两片分片布局是否合法。
 */
static bool _linkg_transport_wire_fragment_layout_valid(uint16_t fragment_offset, uint16_t packet_length, uint32_t fragment_length)
{
    if (packet_length <= LINKG_TRANSPORT_PAYLOAD_MAX_SIZE ||
        packet_length > LINKG_TRANSPORT_PACKET_MAX_SIZE)
    {
        return false;
    }

    if (fragment_offset == 0U)
    {
        return fragment_length == LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
    }

    if (fragment_offset != LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE)
    {
        return false;
    }

    return fragment_length == (uint32_t)packet_length - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;
}

/****************************** 协议封装 ******************************/

/**
 * @brief 在Packet前部添加Transport基础头。
 *
 * @note 多字节协议字段转换为网络字节序，节点编号为单字节无需转换。
 *       reserved由本函数统一置零。
 */
int linkg_transport_wire_encode(linkg_packet_t *packet, const linkg_transport_header_t *header)
{
    linkg_transport_header_t wire;
    void                    *data;

    if (packet == NULL || header == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (!linkg_transport_type_valid((linkg_transport_type_t)header->type) ||
        !_linkg_transport_wire_flags_valid(header->flags) ||
        !_linkg_transport_wire_node_id_valid(header->source_node_id) ||
        !_linkg_transport_wire_node_id_valid(header->destination_node_id))
    {
        return -EINVAL;
    }

    if (packet->data_length + LINKG_TRANSPORT_WIRE_HEADER_SIZE > LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE)
    {
        return -EMSGSIZE;
    }

    if (linkg_packet_headroom(packet) < LINKG_TRANSPORT_WIRE_HEADER_SIZE)
    {
        return -ENOSPC;
    }

    memset(&wire, 0, sizeof(wire));

    wire.magic               = htonl(LINKG_TRANSPORT_WIRE_MAGIC);
    wire.version             = LINKG_TRANSPORT_WIRE_VERSION;
    wire.type                = header->type;
    wire.flags               = htons(header->flags);
    wire.sequence            = htonl(header->sequence);
    wire.source_node_id      = header->source_node_id;
    wire.destination_node_id = header->destination_node_id;
    wire.reserved            = 0U;

    data = linkg_packet_push(packet, LINKG_TRANSPORT_WIRE_HEADER_SIZE);
    if (data == NULL)
    {
        return -ENOSPC;
    }

    memcpy(data, &wire, sizeof(wire));

    return 0;
}

/**
 * @brief 在Packet前部添加Transport分片扩展头。
 *
 * @note 调用时Packet只包含当前分片载荷，必须先添加分片扩展头，
 *       再添加Transport基础头。
 */
int linkg_transport_wire_fragment_encode(linkg_packet_t *packet, const linkg_transport_fragment_header_t *header)
{
    linkg_transport_fragment_header_t wire;
    void                             *data;

    if (packet == NULL || header == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (header->packet_id == LINKG_TRANSPORT_PACKET_ID_INVALID ||
        packet->data_length == 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_transport_wire_fragment_layout_valid(header->fragment_offset,
                                                     header->packet_length,
                                                     packet->data_length))
    {
        return -EINVAL;
    }

    if (packet->data_length +
        LINKG_TRANSPORT_WIRE_FRAGMENT_HEADER_SIZE +
        LINKG_TRANSPORT_WIRE_HEADER_SIZE >
        LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE)
    {
        return -EMSGSIZE;
    }

    if (linkg_packet_headroom(packet) < LINKG_TRANSPORT_WIRE_FRAGMENT_HEADER_SIZE)
    {
        return -ENOSPC;
    }

    wire.packet_id       = htonl(header->packet_id);
    wire.fragment_offset = htons(header->fragment_offset);
    wire.packet_length   = htons(header->packet_length);

    data = linkg_packet_push(packet, LINKG_TRANSPORT_WIRE_FRAGMENT_HEADER_SIZE);
    if (data == NULL)
    {
        return -ENOSPC;
    }

    memcpy(data, &wire, sizeof(wire));

    return 0;
}

/****************************** 协议解析 ******************************/

/**
 * @brief 解析Packet中的Transport基础头。
 *
 * @note 成功后多字节字段转换为主机字节序，节点编号为单字节无需转换。
 */
int linkg_transport_wire_decode(const linkg_packet_t *packet, linkg_transport_header_t *header)
{
    linkg_transport_header_t wire;
    linkg_transport_header_t decoded;

    if (packet == NULL || header == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length < LINKG_TRANSPORT_WIRE_HEADER_SIZE ||
        packet->data_length > LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE)
    {
        return -EMSGSIZE;
    }

    memcpy(&wire, linkg_packet_const_data(packet), sizeof(wire));

    decoded.magic               = ntohl(wire.magic);
    decoded.version             = wire.version;
    decoded.type                = wire.type;
    decoded.flags               = ntohs(wire.flags);
    decoded.sequence            = ntohl(wire.sequence);
    decoded.source_node_id      = wire.source_node_id;
    decoded.destination_node_id = wire.destination_node_id;
    decoded.reserved            = ntohs(wire.reserved);

    if (decoded.magic != LINKG_TRANSPORT_WIRE_MAGIC ||
        decoded.version != LINKG_TRANSPORT_WIRE_VERSION ||
        !linkg_transport_type_valid((linkg_transport_type_t)decoded.type) ||
        !_linkg_transport_wire_flags_valid(decoded.flags) ||
        !_linkg_transport_wire_node_id_valid(decoded.source_node_id) ||
        !_linkg_transport_wire_node_id_valid(decoded.destination_node_id) ||
        decoded.reserved != 0U)
    {
        return -EPROTO;
    }

    if ((decoded.flags & LINKG_TRANSPORT_FLAG_FRAGMENT) != 0U &&
        packet->data_length < LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE + 1U)
    {
        return -EPROTO;
    }

    *header = decoded;

    return 0;
}

/**
 * @brief 解析Packet中的Transport分片扩展头。
 *
 * @note Packet必须包含Transport基础头、分片扩展头和当前分片载荷；
 *       成功后字段均使用主机字节序。
 */
int linkg_transport_wire_fragment_decode(const linkg_packet_t *packet, linkg_transport_fragment_header_t *header)
{
    linkg_transport_fragment_header_t wire;
    linkg_transport_fragment_header_t decoded;
    const uint8_t                    *data;
    uint32_t                          fragment_length;

    if (packet == NULL || header == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length < LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE + 1U ||
        packet->data_length > LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE)
    {
        return -EMSGSIZE;
    }

    data = linkg_packet_const_data(packet);

    memcpy(&wire,
           data + LINKG_TRANSPORT_WIRE_HEADER_SIZE,
           sizeof(wire));

    decoded.packet_id       = ntohl(wire.packet_id);
    decoded.fragment_offset = ntohs(wire.fragment_offset);
    decoded.packet_length   = ntohs(wire.packet_length);

    fragment_length = packet->data_length - LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE;

    if (decoded.packet_id == LINKG_TRANSPORT_PACKET_ID_INVALID ||
        !_linkg_transport_wire_fragment_layout_valid(decoded.fragment_offset,
                                                     decoded.packet_length,
                                                     fragment_length))
    {
        return -EPROTO;
    }

    *header = decoded;

    return 0;
}

/****************************** 协议修改 ******************************/

/**
 * @brief 更新Packet中Transport基础头的逐跳序列号。
 *
 * @note 仅修改基础头sequence，原始发送节点、最终目标节点、
 *       分片扩展头和packet_id均保持不变。
 */
int linkg_transport_wire_update_sequence(linkg_packet_t *packet, uint32_t sequence)
{
    linkg_transport_header_t wire;
    uint16_t                 flags;

    if (packet == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length < LINKG_TRANSPORT_WIRE_HEADER_SIZE ||
        packet->data_length > LINKG_TRANSPORT_WIRE_FRAME_MAX_SIZE)
    {
        return -EMSGSIZE;
    }

    memcpy(&wire, linkg_packet_const_data(packet), sizeof(wire));

    flags = ntohs(wire.flags);

    if (ntohl(wire.magic) != LINKG_TRANSPORT_WIRE_MAGIC ||
        wire.version != LINKG_TRANSPORT_WIRE_VERSION ||
        !linkg_transport_type_valid((linkg_transport_type_t)wire.type) ||
        !_linkg_transport_wire_flags_valid(flags) ||
        !_linkg_transport_wire_node_id_valid(wire.source_node_id) ||
        !_linkg_transport_wire_node_id_valid(wire.destination_node_id) ||
        ntohs(wire.reserved) != 0U)
    {
        return -EPROTO;
    }

    wire.sequence = htonl(sequence);

    memcpy(linkg_packet_data(packet), &wire, sizeof(wire));

    return 0;
}
