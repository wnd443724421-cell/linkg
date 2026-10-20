
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
 * @brief 单个业务类别的Cellular Wire累计接收统计。
 *
 * lost_packets为64包Sequence窗口确认丢失的Packet数量。
 * sample_packets为成功接收和确认丢失的Packet数量之和。
 * 两个字段均为累计值，不在底层计算丢包率。
 */
typedef struct
{
    uint64_t lost_packets;   // 当前业务累计确认丢失Packet数
    uint64_t sample_packets; // 当前业务累计接收与丢失Packet总数
} linkg_cellular_rx_class_counters_t;

/**
 * @brief 指定对端Cellular链路累计接收统计。
 *
 * REALTIME、VIDEO、DATA分别使用独立Sequence窗口。
 * classes按LINKG_LINK_TX_CLASS_*索引保存独立业务统计。
 * 保留原有汇总字段，兼容尚未改造的Switch观测接口。
 */
typedef struct
{
    uint64_t received_packets;       // 三类业务累计成功接收Packet总数
    uint64_t confirmed_lost_packets; // 三类业务累计确认丢失Packet总数

    linkg_cellular_rx_class_counters_t classes[LINKG_LINK_TX_CLASS_COUNT]; // 三类业务独立Wire接收统计
} linkg_cellular_rx_stats_t;

/****************************** 生命周期 ******************************/

int linkg_cellular_link_create(const linkg_link_config_t *link_config, const linkg_cellular_link_config_t *cellular_config, linkg_link_t **out);

/****************************** 统计查询 ******************************/

/**
 * @brief 获取指定对端节点的Cellular链路累计接收统计。
 *
 * 返回三类业务独立统计及原有整链路汇总统计。
 * 仅返回累计数量，统计窗口和丢包率由上层计算。
 */
int linkg_cellular_link_get_rx_stats(linkg_link_t *link, uint8_t peer_node_id, linkg_cellular_rx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
