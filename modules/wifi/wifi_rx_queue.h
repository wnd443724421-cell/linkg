/**
 * @file wifi_rx_queue.h
 * @brief LinkG Wi-Fi接收等待队列接口
 */

#ifndef WIFI_RX_QUEUE_H
#define WIFI_RX_QUEUE_H

#include <stdint.h>

#include "linkg_path.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_packet        linkg_packet_t;
typedef struct linkg_wifi_rx_queue linkg_wifi_rx_queue_t;

/****************************** 队列元素 ******************************/

typedef struct
{
    linkg_packet_t        *packet; // 已接收数据包，Queue持有基础引用
    linkg_path_endpoint_t  source; // 数据来源Endpoint
} linkg_wifi_rx_queue_item_t;

/****************************** 生命周期 ******************************/

linkg_wifi_rx_queue_t *linkg_wifi_rx_queue_create(uint32_t capacity);
void                   linkg_wifi_rx_queue_destroy(linkg_wifi_rx_queue_t *queue);
void                   linkg_wifi_rx_queue_clear(linkg_wifi_rx_queue_t *queue);

/****************************** 状态查询 ******************************/

uint32_t linkg_wifi_rx_queue_count(const linkg_wifi_rx_queue_t *queue);
uint32_t linkg_wifi_rx_queue_capacity(const linkg_wifi_rx_queue_t *queue);
uint32_t linkg_wifi_rx_queue_available(const linkg_wifi_rx_queue_t *queue);

/****************************** 队列操作 ******************************/

uint32_t linkg_wifi_rx_queue_push_batch(linkg_wifi_rx_queue_t *queue, const linkg_wifi_rx_queue_item_t *items, uint32_t count);
uint32_t linkg_wifi_rx_queue_pop_batch(linkg_wifi_rx_queue_t *queue, linkg_wifi_rx_queue_item_t *items, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
