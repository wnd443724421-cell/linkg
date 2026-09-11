/**
 * @file wifi_wire.h
 * @brief LinkG Wi-Fi私有Wire头定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-11
 */

#ifndef WIFI_WIRE_H
#define WIFI_WIRE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** Wire格式 ******************************/

/**
 * @brief Wi-Fi链路私有头。
 *
 * sequence是同一对端全部Wi-Fi业务共享的32位链路序号，Wire使用网络字节序。
 * 该序号只在报文真正提交Wi-Fi UDP Socket时消耗，用于接收端统计Wi-Fi链路级丢包。
 */
typedef struct __attribute__((packed))
{
    uint32_t sequence;
} linkg_wifi_wire_header_t;

#define LINKG_WIFI_WIRE_HEADER_SIZE ((uint32_t)sizeof(linkg_wifi_wire_header_t))

#ifdef __cplusplus
static_assert(sizeof(linkg_wifi_wire_header_t) == 4U, "invalid Wi-Fi wire header size");
#else
_Static_assert(sizeof(linkg_wifi_wire_header_t) == 4U, "invalid Wi-Fi wire header size");
#endif

#ifdef __cplusplus
}
#endif

#endif
