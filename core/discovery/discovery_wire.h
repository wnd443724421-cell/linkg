/**
 * @file discovery_wire.h
 * @brief LinkG设备发现Wire协议定义
 */

#ifndef DISCOVERY_WIRE_H
#define DISCOVERY_WIRE_H

#include <stdint.h>

#include "linkg_system_resources.h"

#include "discovery_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 协议常量 ******************************/

#define LINKG_DISCOVERY_WIRE_MAGIC                 0x4C474453U // Discovery协议标识"LGDS"
#define LINKG_DISCOVERY_WIRE_VERSION               2U          // Discovery协议版本
#define LINKG_DISCOVERY_WIRE_HEADER_SIZE           8U          // Wire公共头固定长度
#define LINKG_DISCOVERY_WIRE_ENDPOINT_ADDRESS_SIZE 16U         // Wire端点地址固定长度
#define LINKG_DISCOVERY_WIRE_ENDPOINT_SIZE         19U         // Wire端点固定长度
#define LINKG_DISCOVERY_WIRE_REPORT_SIZE           57U         // 设备完整状态Payload固定长度
#define LINKG_DISCOVERY_WIRE_TOPOLOGY_NODE_SIZE    1U          // 单个拓扑Node ID固定长度
#define LINKG_DISCOVERY_WIRE_AP_SYNC_BASE_SIZE     69U         // AP同步Payload固定部分长度
#define LINKG_DISCOVERY_WIRE_LEAVE_PAYLOAD_SIZE    17U         // Peer注销Payload固定长度
#define LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE       (LINKG_DISCOVERY_WIRE_HEADER_SIZE + LINKG_DISCOVERY_WIRE_REPORT_SIZE)
#define LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE       (LINKG_DISCOVERY_WIRE_HEADER_SIZE + LINKG_DISCOVERY_WIRE_LEAVE_PAYLOAD_SIZE)
#define LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE      (LINKG_DISCOVERY_WIRE_HEADER_SIZE + LINKG_DISCOVERY_WIRE_AP_SYNC_BASE_SIZE + LINKG_RESOURCE_NETWORK_STA_MAX * LINKG_DISCOVERY_WIRE_TOPOLOGY_NODE_SIZE)

/****************************** 消息类型 ******************************/

typedef enum
{
    LINKG_DISCOVERY_MESSAGE_INVALID    = 0, // 非法消息
    LINKG_DISCOVERY_MESSAGE_STA_REPORT = 1, // STA完整状态上报
    LINKG_DISCOVERY_MESSAGE_AP_SYNC    = 2, // AP完整状态及拓扑同步
    LINKG_DISCOVERY_MESSAGE_PEER_LEAVE = 3  // Peer主动离开
} linkg_discovery_message_type_t;

/****************************** 地址类型 ******************************/

typedef enum
{
    LINKG_DISCOVERY_ADDRESS_INVALID = 0, // 无有效端点
    LINKG_DISCOVERY_ADDRESS_IPV4    = 1, // IPv4端点
    LINKG_DISCOVERY_ADDRESS_IPV6    = 2  // IPv6端点
} linkg_discovery_address_type_t;

/****************************** 报文头 ******************************/

typedef struct
{
    uint32_t magic;          // Discovery协议标识
    uint8_t  version;        // Discovery协议版本
    uint8_t  type;           // Discovery消息类型
    uint16_t payload_length; // Payload长度，不包含公共头
} linkg_discovery_wire_header_t;

/****************************** 状态编解码 ******************************/

int linkg_discovery_wire_encode_report(const linkg_discovery_report_t *report, uint8_t *buffer, uint32_t capacity);
int linkg_discovery_wire_decode_report(const uint8_t *buffer, uint32_t length, linkg_discovery_report_t *report);

/****************************** 报文头编解码 ******************************/

int linkg_discovery_wire_encode_header(const linkg_discovery_wire_header_t *header, uint8_t *buffer, uint32_t capacity);
int linkg_discovery_wire_decode_header(const uint8_t *buffer, uint32_t length, linkg_discovery_wire_header_t *header);

/****************************** STA状态报文 ******************************/

int linkg_discovery_wire_encode_sta_report(const linkg_discovery_report_t *report, uint8_t *buffer, uint32_t capacity, uint32_t *length);
int linkg_discovery_wire_decode_sta_report(const uint8_t *buffer, uint32_t length, linkg_discovery_report_t *report);

/****************************** AP同步报文 ******************************/

int linkg_discovery_wire_encode_ap_sync(const linkg_discovery_ap_sync_t *sync, uint8_t *buffer, uint32_t capacity, uint32_t *length);
int linkg_discovery_wire_decode_ap_sync(const uint8_t *buffer, uint32_t length, linkg_discovery_ap_sync_t *sync);

/****************************** Peer注销报文 ******************************/

int linkg_discovery_wire_encode_peer_leave(const linkg_discovery_leave_t *leave, uint8_t *buffer, uint32_t capacity, uint32_t *length);
int linkg_discovery_wire_decode_peer_leave(const uint8_t *buffer, uint32_t length, linkg_discovery_leave_t *leave);

#ifdef __cplusplus
}
#endif

#endif
