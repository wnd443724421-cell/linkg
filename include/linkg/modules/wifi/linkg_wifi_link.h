/**
 * @file linkg_wifi_link.h
 * @brief LinkG Wi-Fi数据链路接口
 */

#ifndef LINKG_WIFI_LINK_H
#define LINKG_WIFI_LINK_H

#include <netinet/in.h>
#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 链路配置 ******************************/

typedef struct
{
    uint16_t data_port;                      // 全网固定普通数据UDP端口，主机字节序
    uint16_t realtime_port;                  // 全网固定实时数据UDP端口，主机字节序
    uint16_t video_port;                     // 全网固定视频数据UDP端口，主机字节序
    uint32_t data_send_buffer_size;          // 普通数据UDP发送缓冲区
    uint32_t video_send_buffer_size;         // 视频UDP发送缓冲区
    uint32_t realtime_send_buffer_size;      // 实时UDP发送缓冲区
    uint32_t data_receive_buffer_size;       // 普通数据UDP接收缓冲区
    uint32_t video_receive_buffer_size;      // 视频UDP接收缓冲区
    uint32_t realtime_receive_buffer_size;   // 实时UDP接收缓冲区
} linkg_wifi_link_config_t;

/****************************** 链路接收统计 ******************************/

/**
 * @brief 单个业务类别的Wi-Fi Wire累计接收统计。
 *
 * lost_packets为64包Sequence窗口确认丢失的Packet数量。
 * sample_packets为成功接收和确认丢失的Packet数量之和。
 * 两个字段均为累计值，不在底层计算丢包率。
 */
typedef struct
{
    uint64_t lost_packets;   // 当前业务累计确认丢失Packet数
    uint64_t sample_packets; // 当前业务累计接收与丢失Packet总数
} linkg_wifi_rx_class_counters_t;


/**
 * @brief 指定对端Wi-Fi链路累计接收统计。
 *
 * REALTIME、VIDEO、DATA分别使用独立Sequence窗口。
 * classes按LINKG_LINK_TX_CLASS_*索引保存独立业务统计。
 * 保留原有汇总字段，兼容尚未改造的Switch观测接口。
 */
typedef struct
{
    uint64_t received_packets;       // 三类业务累计成功接收Packet总数
    uint64_t confirmed_lost_packets; // 三类业务累计确认丢失Packet总数

    linkg_wifi_rx_class_counters_t classes[LINKG_LINK_TX_CLASS_COUNT]; // 三类业务独立Wire接收统计
} linkg_wifi_rx_stats_t;

/****************************** 生命周期 ******************************/

int linkg_wifi_link_create(const linkg_link_config_t *link_config, const linkg_wifi_link_config_t *wifi_config, linkg_link_t **out);

/****************************** 统计查询 ******************************/

/**
 * @brief 获取指定对端节点的Wi-Fi链路接收累计统计。
 */
int linkg_wifi_link_get_rx_stats(linkg_link_t *link, uint8_t peer_node_id, linkg_wifi_rx_stats_t *stats);

#ifdef __cplusplus
}
#endif

#endif
