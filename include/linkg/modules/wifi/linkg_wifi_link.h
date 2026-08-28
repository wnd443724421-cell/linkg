/**
 * @file linkg_wifi_link.h
 * @brief LinkG Wi-Fi数据链路接口
 */

#ifndef LINKG_WIFI_LINK_H
#define LINKG_WIFI_LINK_H

#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 链路配置 ******************************/

typedef struct
{
    uint16_t data_port;                  // 全网固定普通数据UDP端口，主机字节序
    uint16_t realtime_port;              // 全网固定实时数据UDP端口，主机字节序
    uint16_t video_port;                 // 全网固定视频数据UDP端口，主机字节序
    uint32_t data_send_buffer_size;      // 普通数据UDP发送缓冲区
    uint32_t video_send_buffer_size;     // 视频UDP发送缓冲区
    uint32_t realtime_send_buffer_size;  // 实时UDP发送缓冲区
} linkg_wifi_link_config_t;

/****************************** 生命周期 ******************************/

int linkg_wifi_link_create(const linkg_link_config_t *link_config, const linkg_wifi_link_config_t *wifi_config, linkg_link_t **out);

#ifdef __cplusplus
}
#endif

#endif
