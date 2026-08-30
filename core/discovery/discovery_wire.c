/**
 * @file discovery_wire.c
 * @brief LinkG设备发现Wire协议编解码实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-14
 */

#include "discovery_wire.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <string.h>

/****************************** 整数编解码 ******************************/

/**
 * @brief 写入16位网络字节序整数。
 */
static void _linkg_discovery_wire_write_u16(uint8_t *buffer, uint16_t value)
{
    buffer[0] = (uint8_t)(value >> 8);
    buffer[1] = (uint8_t)value;
}

/**
 * @brief 读取16位网络字节序整数。
 */
static uint16_t _linkg_discovery_wire_read_u16(const uint8_t *buffer)
{
    return ((uint16_t)buffer[0] << 8) |
           (uint16_t)buffer[1];
}

/**
 * @brief 写入32位网络字节序整数。
 */
static void _linkg_discovery_wire_write_u32(uint8_t *buffer, uint32_t value)
{
    buffer[0] = (uint8_t)(value >> 24);
    buffer[1] = (uint8_t)(value >> 16);
    buffer[2] = (uint8_t)(value >> 8);
    buffer[3] = (uint8_t)value;
}

/**
 * @brief 读取32位网络字节序整数。
 */
static uint32_t _linkg_discovery_wire_read_u32(const uint8_t *buffer)
{
    return ((uint32_t)buffer[0] << 24) |
           ((uint32_t)buffer[1] << 16) |
           ((uint32_t)buffer[2] << 8) |
           (uint32_t)buffer[3];
}

/**
 * @brief 写入64位网络字节序整数。
 */
static void _linkg_discovery_wire_write_u64(uint8_t *buffer, uint64_t value)
{
    buffer[0] = (uint8_t)(value >> 56);
    buffer[1] = (uint8_t)(value >> 48);
    buffer[2] = (uint8_t)(value >> 40);
    buffer[3] = (uint8_t)(value >> 32);
    buffer[4] = (uint8_t)(value >> 24);
    buffer[5] = (uint8_t)(value >> 16);
    buffer[6] = (uint8_t)(value >> 8);
    buffer[7] = (uint8_t)value;
}

/**
 * @brief 读取64位网络字节序整数。
 */
static uint64_t _linkg_discovery_wire_read_u64(const uint8_t *buffer)
{
    return ((uint64_t)buffer[0] << 56) |
           ((uint64_t)buffer[1] << 48) |
           ((uint64_t)buffer[2] << 40) |
           ((uint64_t)buffer[3] << 32) |
           ((uint64_t)buffer[4] << 24) |
           ((uint64_t)buffer[5] << 16) |
           ((uint64_t)buffer[6] << 8) |
           (uint64_t)buffer[7];
}

/****************************** 端点编解码 ******************************/

/**
 * @brief 将路径端点编码为Wire端点。
 *
 * length为0表示当前路径端点尚未就绪或已经失效。
 */
static int _linkg_discovery_wire_encode_endpoint(const linkg_path_endpoint_t *endpoint, uint8_t *buffer)
{
    const struct sockaddr_in6 *ipv6;
    const struct sockaddr_in  *ipv4;

    if (endpoint == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    memset(buffer, 0, LINKG_DISCOVERY_WIRE_ENDPOINT_SIZE);

    if (endpoint->length == 0U)
    {
        buffer[0] = LINKG_DISCOVERY_ADDRESS_INVALID;
        return 0;
    }

    if (endpoint->address.ss_family == AF_INET)
    {
        if (endpoint->length < sizeof(struct sockaddr_in))
        {
            return -EINVAL;
        }

        ipv4 = (const struct sockaddr_in *)&endpoint->address;

        buffer[0] = LINKG_DISCOVERY_ADDRESS_IPV4;

        _linkg_discovery_wire_write_u16(&buffer[1], ntohs(ipv4->sin_port));
        memcpy(&buffer[3], &ipv4->sin_addr, sizeof(ipv4->sin_addr));

        return 0;
    }

    if (endpoint->address.ss_family == AF_INET6)
    {
        if (endpoint->length < sizeof(struct sockaddr_in6))
        {
            return -EINVAL;
        }

        ipv6 = (const struct sockaddr_in6 *)&endpoint->address;

        buffer[0] = LINKG_DISCOVERY_ADDRESS_IPV6;

        _linkg_discovery_wire_write_u16(&buffer[1], ntohs(ipv6->sin6_port));
        memcpy(&buffer[3], &ipv6->sin6_addr, sizeof(ipv6->sin6_addr));

        return 0;
    }

    return -EAFNOSUPPORT;
}

/**
 * @brief 将Wire端点解码为路径端点。
 *
 * INVALID端点解码后保持length为0。
 */
static int _linkg_discovery_wire_decode_endpoint(const uint8_t *buffer, linkg_path_endpoint_t *endpoint)
{
    struct sockaddr_in6 *ipv6;
    struct sockaddr_in  *ipv4;
    uint16_t             port;

    if (buffer == NULL || endpoint == NULL)
    {
        return -EINVAL;
    }

    memset(endpoint, 0, sizeof(*endpoint));

    switch (buffer[0])
    {
        case LINKG_DISCOVERY_ADDRESS_INVALID:
            return 0;

        case LINKG_DISCOVERY_ADDRESS_IPV4:
            port = _linkg_discovery_wire_read_u16(&buffer[1]);

            ipv4 = (struct sockaddr_in *)&endpoint->address;

            ipv4->sin_family = AF_INET;
            ipv4->sin_port   = htons(port);

            memcpy(&ipv4->sin_addr, &buffer[3], sizeof(ipv4->sin_addr));

            endpoint->length = sizeof(*ipv4);

            return 0;

        case LINKG_DISCOVERY_ADDRESS_IPV6:
            port = _linkg_discovery_wire_read_u16(&buffer[1]);

            ipv6 = (struct sockaddr_in6 *)&endpoint->address;

            ipv6->sin6_family = AF_INET6;
            ipv6->sin6_port   = htons(port);

            memcpy(&ipv6->sin6_addr, &buffer[3], sizeof(ipv6->sin6_addr));

            endpoint->length = sizeof(*ipv6);

            return 0;

        default:
            return -EPROTO;
    }
}

/****************************** 状态编解码 ******************************/

/**
 * @brief 将设备完整状态编码为Wire Payload。
 *
 * Wire布局：
 * session_id         8B
 * revision           8B
 * node_id            1B
 * role               1B
 * path_flags         1B
 * wifi_endpoint     19B
 * cellular_endpoint 19B
 */
int linkg_discovery_wire_encode_report(const linkg_discovery_report_t *report, uint8_t *buffer, uint32_t capacity)
{
    uint32_t offset;
    int      ret;

    if (report == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    if (capacity < LINKG_DISCOVERY_WIRE_REPORT_SIZE)
    {
        return -ENOSPC;
    }

    memset(buffer, 0, LINKG_DISCOVERY_WIRE_REPORT_SIZE);

    offset = 0U;

    _linkg_discovery_wire_write_u64(&buffer[offset], report->session_id);
    offset += sizeof(uint64_t);

    _linkg_discovery_wire_write_u64(&buffer[offset], report->revision);
    offset += sizeof(uint64_t);

    buffer[offset++] = report->node.node_id;
    buffer[offset++] = (uint8_t)report->node.role;
    buffer[offset++] = report->path_flags;

    ret = _linkg_discovery_wire_encode_endpoint(&report->wifi_endpoint, &buffer[offset]);
    if (ret != 0)
    {
        return ret;
    }

    offset += LINKG_DISCOVERY_WIRE_ENDPOINT_SIZE;

    ret = _linkg_discovery_wire_encode_endpoint(&report->cellular_endpoint, &buffer[offset]);
    if (ret != 0)
    {
        return ret;
    }

    offset += LINKG_DISCOVERY_WIRE_ENDPOINT_SIZE;

    if (offset != LINKG_DISCOVERY_WIRE_REPORT_SIZE)
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 将Wire Payload解码为设备完整状态。
 */
int linkg_discovery_wire_decode_report(const uint8_t *buffer, uint32_t length, linkg_discovery_report_t *report)
{
    uint32_t offset;
    int      ret;

    if (buffer == NULL || report == NULL)
    {
        return -EINVAL;
    }

    if (length != LINKG_DISCOVERY_WIRE_REPORT_SIZE)
    {
        return -EMSGSIZE;
    }

    memset(report, 0, sizeof(*report));

    offset = 0U;

    report->session_id = _linkg_discovery_wire_read_u64(&buffer[offset]);
    offset += sizeof(uint64_t);

    report->revision = _linkg_discovery_wire_read_u64(&buffer[offset]);
    offset += sizeof(uint64_t);

    report->node.node_id = buffer[offset++];
    report->node.role    = (linkg_device_role_t)buffer[offset++];
    report->path_flags   = buffer[offset++];

    ret = _linkg_discovery_wire_decode_endpoint(&buffer[offset], &report->wifi_endpoint);
    if (ret != 0)
    {
        return ret;
    }

    offset += LINKG_DISCOVERY_WIRE_ENDPOINT_SIZE;

    ret = _linkg_discovery_wire_decode_endpoint(&buffer[offset], &report->cellular_endpoint);
    if (ret != 0)
    {
        return ret;
    }

    offset += LINKG_DISCOVERY_WIRE_ENDPOINT_SIZE;

    if (offset != LINKG_DISCOVERY_WIRE_REPORT_SIZE)
    {
        return -EIO;
    }

    return 0;
}

/****************************** 报文头编解码 ******************************/

/**
 * @brief 将Discovery报文头编码为Wire Header。
 */
int linkg_discovery_wire_encode_header(const linkg_discovery_wire_header_t *header, uint8_t *buffer, uint32_t capacity)
{
    uint32_t offset;

    if (header == NULL || buffer == NULL)
    {
        return -EINVAL;
    }

    if (capacity < LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return -ENOSPC;
    }

    offset = 0U;

    _linkg_discovery_wire_write_u32(&buffer[offset], header->magic);
    offset += sizeof(uint32_t);

    buffer[offset++] = header->version;
    buffer[offset++] = header->type;

    _linkg_discovery_wire_write_u16(&buffer[offset], header->payload_length);
    offset += sizeof(uint16_t);

    if (offset != LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 将Wire Header解码为Discovery报文头。
 */
int linkg_discovery_wire_decode_header(const uint8_t *buffer, uint32_t length, linkg_discovery_wire_header_t *header)
{
    uint32_t offset;

    if (buffer == NULL || header == NULL)
    {
        return -EINVAL;
    }

    if (length < LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return -EMSGSIZE;
    }

    memset(header, 0, sizeof(*header));

    offset = 0U;

    header->magic = _linkg_discovery_wire_read_u32(&buffer[offset]);
    offset += sizeof(uint32_t);

    header->version = buffer[offset++];
    header->type    = buffer[offset++];

    header->payload_length = _linkg_discovery_wire_read_u16(&buffer[offset]);
    offset += sizeof(uint16_t);

    if (offset != LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return -EIO;
    }

    return 0;
}

/****************************** STA状态报文 ******************************/

/**
 * @brief 编码完整STA状态上报报文。
 */
int linkg_discovery_wire_encode_sta_report(const linkg_discovery_report_t *report, uint8_t *buffer, uint32_t capacity, uint32_t *length)
{
    linkg_discovery_wire_header_t header;
    int                           ret;

    if (report == NULL || buffer == NULL || length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (capacity < LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE)
    {
        return -ENOSPC;
    }

    memset(&header, 0, sizeof(header));

    header.magic          = LINKG_DISCOVERY_WIRE_MAGIC;
    header.version        = LINKG_DISCOVERY_WIRE_VERSION;
    header.type           = LINKG_DISCOVERY_MESSAGE_STA_REPORT;
    header.payload_length = LINKG_DISCOVERY_WIRE_REPORT_SIZE;

    ret = linkg_discovery_wire_encode_header(&header, buffer, capacity);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_wire_encode_report(report,
                                             &buffer[LINKG_DISCOVERY_WIRE_HEADER_SIZE],
                                             capacity - LINKG_DISCOVERY_WIRE_HEADER_SIZE);
    if (ret != 0)
    {
        return ret;
    }

    *length = LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE;

    return 0;
}

/**
 * @brief 解码完整STA状态上报报文。
 */
int linkg_discovery_wire_decode_sta_report(const uint8_t *buffer, uint32_t length, linkg_discovery_report_t *report)
{
    linkg_discovery_wire_header_t header;
    uint32_t                      expected_length;
    int                           ret;

    if (buffer == NULL || report == NULL)
    {
        return -EINVAL;
    }

    if (length < LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return -EMSGSIZE;
    }

    ret = linkg_discovery_wire_decode_header(buffer, length, &header);
    if (ret != 0)
    {
        return ret;
    }

    if (header.magic != LINKG_DISCOVERY_WIRE_MAGIC ||
        header.version != LINKG_DISCOVERY_WIRE_VERSION ||
        header.type != LINKG_DISCOVERY_MESSAGE_STA_REPORT)
    {
        return -EPROTO;
    }

    if (header.payload_length != LINKG_DISCOVERY_WIRE_REPORT_SIZE)
    {
        return -EMSGSIZE;
    }

    expected_length = LINKG_DISCOVERY_WIRE_HEADER_SIZE + (uint32_t)header.payload_length;

    if (length != expected_length)
    {
        return -EMSGSIZE;
    }

    return linkg_discovery_wire_decode_report(&buffer[LINKG_DISCOVERY_WIRE_HEADER_SIZE],
                                              header.payload_length,
                                              report);
}

/****************************** AP同步报文 ******************************/

/**
 * @brief 编码完整AP同步报文。
 *
 * Payload布局：
 * AP Report            57B
 * topology_revision     8B
 * node_count            4B
 * node_ids[]            NB
 */
int linkg_discovery_wire_encode_ap_sync(const linkg_discovery_ap_sync_t *sync, uint8_t *buffer, uint32_t capacity, uint32_t *length)
{
    linkg_discovery_wire_header_t header;
    uint32_t                      payload_length;
    uint32_t                      packet_length;
    uint32_t                      offset;
    uint32_t                      index;
    int                           ret;

    if (sync == NULL || buffer == NULL || length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (sync->node_count > LINKG_NODE_PEER_MAX)
    {
        return -EINVAL;
    }

    payload_length = LINKG_DISCOVERY_WIRE_AP_SYNC_BASE_SIZE +
                     sync->node_count * LINKG_DISCOVERY_WIRE_TOPOLOGY_NODE_SIZE;

    packet_length = LINKG_DISCOVERY_WIRE_HEADER_SIZE + payload_length;

    if (capacity < packet_length)
    {
        return -ENOSPC;
    }

    memset(&header, 0, sizeof(header));

    header.magic          = LINKG_DISCOVERY_WIRE_MAGIC;
    header.version        = LINKG_DISCOVERY_WIRE_VERSION;
    header.type           = LINKG_DISCOVERY_MESSAGE_AP_SYNC;
    header.payload_length = (uint16_t)payload_length;

    ret = linkg_discovery_wire_encode_header(&header, buffer, capacity);
    if (ret != 0)
    {
        return ret;
    }

    offset = LINKG_DISCOVERY_WIRE_HEADER_SIZE;

    ret = linkg_discovery_wire_encode_report(&sync->ap, &buffer[offset], capacity - offset);
    if (ret != 0)
    {
        return ret;
    }

    offset += LINKG_DISCOVERY_WIRE_REPORT_SIZE;

    _linkg_discovery_wire_write_u64(&buffer[offset], sync->topology_revision);
    offset += sizeof(uint64_t);

    _linkg_discovery_wire_write_u32(&buffer[offset], sync->node_count);
    offset += sizeof(uint32_t);

    for (index = 0U; index < sync->node_count; index++)
    {
        buffer[offset++] = sync->node_ids[index];
    }

    if (offset != packet_length)
    {
        return -EIO;
    }

    *length = packet_length;

    return 0;
}

/**
 * @brief 解码完整AP同步报文。
 */
int linkg_discovery_wire_decode_ap_sync(const uint8_t *buffer, uint32_t length, linkg_discovery_ap_sync_t *sync)
{
    linkg_discovery_wire_header_t header;
    uint32_t                      expected_payload_length;
    uint32_t                      expected_packet_length;
    uint32_t                      offset;
    uint32_t                      index;
    int                           ret;

    if (buffer == NULL || sync == NULL)
    {
        return -EINVAL;
    }

    if (length < LINKG_DISCOVERY_WIRE_HEADER_SIZE)
    {
        return -EMSGSIZE;
    }

    ret = linkg_discovery_wire_decode_header(buffer, length, &header);
    if (ret != 0)
    {
        return ret;
    }

    if (header.magic != LINKG_DISCOVERY_WIRE_MAGIC ||
        header.version != LINKG_DISCOVERY_WIRE_VERSION ||
        header.type != LINKG_DISCOVERY_MESSAGE_AP_SYNC)
    {
        return -EPROTO;
    }

    if (header.payload_length < LINKG_DISCOVERY_WIRE_AP_SYNC_BASE_SIZE)
    {
        return -EMSGSIZE;
    }

    expected_packet_length = LINKG_DISCOVERY_WIRE_HEADER_SIZE +
                             (uint32_t)header.payload_length;

    if (length != expected_packet_length)
    {
        return -EMSGSIZE;
    }

    memset(sync, 0, sizeof(*sync));

    offset = LINKG_DISCOVERY_WIRE_HEADER_SIZE;

    ret = linkg_discovery_wire_decode_report(&buffer[offset],
                                             LINKG_DISCOVERY_WIRE_REPORT_SIZE,
                                             &sync->ap);
    if (ret != 0)
    {
        return ret;
    }

    offset += LINKG_DISCOVERY_WIRE_REPORT_SIZE;

    sync->topology_revision = _linkg_discovery_wire_read_u64(&buffer[offset]);
    offset += sizeof(uint64_t);

    sync->node_count = _linkg_discovery_wire_read_u32(&buffer[offset]);
    offset += sizeof(uint32_t);

    if (sync->node_count > LINKG_NODE_PEER_MAX)
    {
        return -EPROTO;
    }

    expected_payload_length = LINKG_DISCOVERY_WIRE_AP_SYNC_BASE_SIZE +
                              sync->node_count * LINKG_DISCOVERY_WIRE_TOPOLOGY_NODE_SIZE;

    if ((uint32_t)header.payload_length != expected_payload_length)
    {
        return -EMSGSIZE;
    }

    for (index = 0U; index < sync->node_count; index++)
    {
        sync->node_ids[index] = buffer[offset++];
    }

    if (offset != expected_packet_length)
    {
        return -EIO;
    }

    return 0;
}

/****************************** Peer注销报文 ******************************/

/**
 * @brief 编码Peer主动注销报文。
 *
 * Payload布局：
 * session_id 8B
 * revision   8B
 * node_id    1B
 */
int linkg_discovery_wire_encode_peer_leave(const linkg_discovery_leave_t *leave, uint8_t *buffer, uint32_t capacity, uint32_t *length)
{
    linkg_discovery_wire_header_t header;
    uint32_t                      offset;
    int                           ret;

    if (leave == NULL || buffer == NULL || length == NULL)
    {
        return -EINVAL;
    }

    *length = 0U;

    if (capacity < LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE)
    {
        return -ENOSPC;
    }

    memset(&header, 0, sizeof(header));

    header.magic          = LINKG_DISCOVERY_WIRE_MAGIC;
    header.version        = LINKG_DISCOVERY_WIRE_VERSION;
    header.type           = LINKG_DISCOVERY_MESSAGE_PEER_LEAVE;
    header.payload_length = LINKG_DISCOVERY_WIRE_LEAVE_PAYLOAD_SIZE;

    ret = linkg_discovery_wire_encode_header(&header, buffer, capacity);
    if (ret != 0)
    {
        return ret;
    }

    offset = LINKG_DISCOVERY_WIRE_HEADER_SIZE;

    _linkg_discovery_wire_write_u64(&buffer[offset], leave->session_id);
    offset += sizeof(uint64_t);

    _linkg_discovery_wire_write_u64(&buffer[offset], leave->revision);
    offset += sizeof(uint64_t);

    buffer[offset++] = leave->node_id;

    if (offset != LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE)
    {
        return -EIO;
    }

    *length = offset;

    return 0;
}

/**
 * @brief 解码Peer主动注销报文。
 */
int linkg_discovery_wire_decode_peer_leave(const uint8_t *buffer, uint32_t length, linkg_discovery_leave_t *leave)
{
    linkg_discovery_wire_header_t header;
    uint32_t                      offset;
    int                           ret;

    if (buffer == NULL || leave == NULL)
    {
        return -EINVAL;
    }

    if (length != LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE)
    {
        return -EMSGSIZE;
    }

    ret = linkg_discovery_wire_decode_header(buffer, length, &header);
    if (ret != 0)
    {
        return ret;
    }

    if (header.magic != LINKG_DISCOVERY_WIRE_MAGIC ||
        header.version != LINKG_DISCOVERY_WIRE_VERSION ||
        header.type != LINKG_DISCOVERY_MESSAGE_PEER_LEAVE)
    {
        return -EPROTO;
    }

    if (header.payload_length != LINKG_DISCOVERY_WIRE_LEAVE_PAYLOAD_SIZE)
    {
        return -EMSGSIZE;
    }

    memset(leave, 0, sizeof(*leave));

    offset = LINKG_DISCOVERY_WIRE_HEADER_SIZE;

    leave->session_id = _linkg_discovery_wire_read_u64(&buffer[offset]);
    offset += sizeof(uint64_t);

    leave->revision = _linkg_discovery_wire_read_u64(&buffer[offset]);
    offset += sizeof(uint64_t);

    leave->node_id = buffer[offset++];

    if (offset != LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE)
    {
        return -EIO;
    }

    return 0;
}
