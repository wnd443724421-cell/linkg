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

/****************************** 链路接收统计 ******************************/

/**
 * @brief 指定对端Cellular独立Wire累计接收统计。
 *
 * 统计由Cellular RX维护，三个业务类别使用独立的64帧Sequence窗口。
 */
typedef struct
{
    uint64_t received_packets;       // 三业务累计唯一接收帧数量
    uint64_t confirmed_lost_packets; // 三业务累计窗口确认丢失帧数量
} linkg_cellular_rx_stats_t;

/****************************** 生命周期 ******************************/

int linkg_cellular_link_create(const linkg_link_config_t *link_config, const linkg_cellular_link_config_t *cellular_config, linkg_link_t **out);

/****************************** 统计查询 ******************************/

int linkg_cellular_link_get_rx_stats(linkg_link_t *link, uint8_t peer_node_id, linkg_cellular_rx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
