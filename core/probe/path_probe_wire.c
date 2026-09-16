/**
 * @file path_probe_wire.c
 * @brief LinkG业务Path主动探测Wire协议处理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#include "path_probe_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

/****************************** 模块常量 ******************************/

#define LINKG_PATH_PROBE_WIRE_TYPE_OFFSET     3U // 类型字段偏移
#define LINKG_PATH_PROBE_WIRE_SEQUENCE_OFFSET 4U // 序列号字段偏移

/****************************** 协议Magic ******************************/

static const uint8_t g_path_probe_wire_magic[LINKG_PATH_PROBE_MAGIC_SIZE] =
{
    'L', 'G', 'P'
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验Packet视图边界，不依赖未对齐Wire结构体访问。
 */
static bool _linkg_path_probe_packet_valid(const linkg_packet_t *packet)
{
    if (packet == NULL || packet->pool == NULL || packet->slot == NULL)
    {
        return false;
    }

    if (packet->data_offset > packet->pool->slot_size)
    {
        return false;
    }

    return packet->data_length <= packet->pool->slot_size - packet->data_offset;
}

/**
 * @brief 判断Probe类型是否为请求或响应。
 */
static bool _linkg_path_probe_wire_type_valid(linkg_path_probe_wire_type_t type)
{
    return type == LINKG_PATH_PROBE_WIRE_TYPE_REQUEST || type == LINKG_PATH_PROBE_WIRE_TYPE_RESPONSE;
}

/****************************** 协议封装 ******************************/

/**
 * @brief 将8字节Probe写入空Packet，Wire序列号固定使用网络字节序。
 */
int linkg_path_probe_wire_encode(linkg_packet_t *packet, linkg_path_probe_wire_type_t type, uint32_t sequence)
{
    uint32_t  network_sequence;
    uint8_t  *data;

    if (!_linkg_path_probe_packet_valid(packet) || !_linkg_path_probe_wire_type_valid(type) || sequence == LINKG_PATH_PROBE_SEQUENCE_INVALID)
    {
        return -EINVAL;
    }

    if (packet->data_length != 0U)
    {
        return -EINVAL;
    }

    if (linkg_packet_capacity(packet) < LINKG_PATH_PROBE_WIRE_SIZE)
    {
        return -ENOSPC;
    }

    data = linkg_packet_data(packet);
    memcpy(data, g_path_probe_wire_magic, LINKG_PATH_PROBE_MAGIC_SIZE);
    data[LINKG_PATH_PROBE_WIRE_TYPE_OFFSET] = (uint8_t)type;

    network_sequence = htonl(sequence);
    memcpy(data + LINKG_PATH_PROBE_WIRE_SEQUENCE_OFFSET, &network_sequence, sizeof(network_sequence));
    packet->data_length = LINKG_PATH_PROBE_WIRE_SIZE;

    return 0;
}

/****************************** 协议解析 ******************************/

/**
 * @brief 严格解析8字节Probe，返回主机序sequence，不修改借用的Packet。
 */
int linkg_path_probe_wire_decode(const linkg_packet_t *packet, linkg_path_probe_wire_t *wire)
{
    linkg_path_probe_wire_type_t  type;
    const uint8_t                *data;
    uint32_t                      network_sequence;
    uint32_t                      sequence;

    if (wire == NULL || !_linkg_path_probe_packet_valid(packet))
    {
        return -EINVAL;
    }

    memset(wire, 0, sizeof(*wire));

    if (packet->data_length != LINKG_PATH_PROBE_WIRE_SIZE)
    {
        return -EMSGSIZE;
    }

    data = linkg_packet_const_data(packet);
    if (memcmp(data, g_path_probe_wire_magic, LINKG_PATH_PROBE_MAGIC_SIZE) != 0)
    {
        return -EPROTO;
    }

    type = (linkg_path_probe_wire_type_t)data[LINKG_PATH_PROBE_WIRE_TYPE_OFFSET];
    if (!_linkg_path_probe_wire_type_valid(type))
    {
        return -EPROTO;
    }

    memcpy(&network_sequence, data + LINKG_PATH_PROBE_WIRE_SEQUENCE_OFFSET, sizeof(network_sequence));
    sequence = ntohl(network_sequence);
    if (sequence == LINKG_PATH_PROBE_SEQUENCE_INVALID)
    {
        return -EPROTO;
    }

    memcpy(wire->magic, g_path_probe_wire_magic, LINKG_PATH_PROBE_MAGIC_SIZE);
    wire->type     = (uint8_t)type;
    wire->sequence = sequence;

    return 0;
}
