/**
 * @file wifi_wire.h
 * @brief LinkG Wi-Fi私有Wire头定义
 * @author Dawn
 * @version 1.1.0
 * @date 2026-09-12
 */

#ifndef WIFI_WIRE_H
#define WIFI_WIRE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** Wire格式 ******************************/

/**
 * @brief Wi-Fi链路私有头。
 *
 * session_id标识当前发送端Wi-Fi TX实例会话，整个TX对象生命周期内保持不变，
 * 所有对端Node和全部业务类别共享。sequence为同一对端全部Wi-Fi业务共享的
 * 32位链路序号。两个字段Wire均使用网络字节序。
 */
typedef struct __attribute__((packed))
{
    uint32_t session_id; // 当前发送端Wi-Fi TX实例会话ID
    uint32_t sequence;   // 当前对端独立Wi-Fi链路Sequence
} linkg_wifi_wire_header_t;

#define LINKG_WIFI_WIRE_HEADER_SIZE ((uint32_t)sizeof(linkg_wifi_wire_header_t))

#ifdef __cplusplus
static_assert(sizeof(linkg_wifi_wire_header_t) == 8U, "invalid Wi-Fi wire header size");
#else
_Static_assert(sizeof(linkg_wifi_wire_header_t) == 8U, "invalid Wi-Fi wire header size");
#endif

#ifdef __cplusplus
}
#endif

#endif
