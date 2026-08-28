/**
 * @file linkg_cellular_link.h
 * @brief LinkG蜂窝IPv6数据链路接口
 */

#ifndef LINKG_CELLULAR_LINK_H
#define LINKG_CELLULAR_LINK_H

#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 配置定义 ******************************/

typedef struct
{
    uint16_t data_port;     // 普通数据IPv6 UDP端口
    uint16_t realtime_port; // 实时业务IPv6 UDP端口
    uint16_t video_port;    // 视频业务IPv6 UDP端口
} linkg_cellular_link_config_t;

/****************************** 生命周期 ******************************/

int linkg_cellular_link_create(const linkg_link_config_t *link_config, const linkg_cellular_link_config_t *cellular_config, linkg_link_t **out);

#ifdef __cplusplus
}
#endif

#endif
