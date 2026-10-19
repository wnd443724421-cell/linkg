
/**
 * @file wifi_wire.h
 * @brief LinkG Wi-Fi私有Wire头定义
 */

#ifndef WIFI_WIRE_H
#define WIFI_WIRE_H

#include <stdint.h>

#include "linkg_network_ops.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** Wire格式 ******************************/

#define LINKG_WIFI_WIRE_VERSION 1U // Wi-Fi私有Wire协议版本

/**
 * @brief Wi-Fi链路私有头。
 *
 * Session与Sequence使用网络字节序。
 * source_mac为发送端wlan0 MAC，用于接收端验证Peer身份。
 */
typedef struct __attribute__((packed))
{
    uint32_t session_id;                                       // Wi-Fi TX实例会话ID
    uint32_t sequence;                                         // 当前对端业务类别Sequence
    uint8_t  version;                                          // Wi-Fi私有Wire版本
    uint8_t  source_mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH];     // 发送端Wi-Fi MAC
    uint8_t  reserved;                                         // 保留字段，必须为0
} linkg_wifi_wire_header_t;

#define LINKG_WIFI_WIRE_HEADER_SIZE ((uint32_t)sizeof(linkg_wifi_wire_header_t))

#ifdef __cplusplus
static_assert(sizeof(linkg_wifi_wire_header_t) == 16U, "invalid Wi-Fi wire header size");
#else
_Static_assert(sizeof(linkg_wifi_wire_header_t) == 16U, "invalid Wi-Fi wire header size");
#endif

#ifdef __cplusplus
}
#endif

#endif
