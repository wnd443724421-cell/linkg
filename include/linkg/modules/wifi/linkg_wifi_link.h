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
 * @brief 指定对端Wi-Fi链路累计接收统计。
 *
 * received_packets只统计首次成功接收的唯一Wi-Fi Wire Packet；
 * confirmed_lost_packets只统计64包滑动窗口推出后仍未到达的确认丢包。
 * 两个字段均为进程生命周期内累计值，loss rate、时间窗口和Report ID由上层统计模块计算。
 */
typedef struct
{
    uint64_t received_packets;
    uint64_t confirmed_lost_packets;
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
