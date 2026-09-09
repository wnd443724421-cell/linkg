/**
 * @file wifi_tx_queue.h
 * @brief LinkG Wi-Fi发送等待队列接口
 */

#ifndef WIFI_TX_QUEUE_H
#define WIFI_TX_QUEUE_H

#include <stdint.h>

#include "linkg_path.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_packet        linkg_packet_t;
typedef struct linkg_wifi_tx_queue linkg_wifi_tx_queue_t;

/****************************** 队列元素 ******************************/

typedef struct
{
    linkg_packet_t       *packet;      // 待发送数据包，Queue持有一个引用
    linkg_path_t         *path;        // 数据包所属Path，Queue持有一个引用
    linkg_path_endpoint_t destination; // 入队时下一跳端点快照
    uint64_t              enqueue_us;  // 入队单调时钟时间
} linkg_wifi_tx_queue_item_t;

/****************************** 生命周期 ******************************/

linkg_wifi_tx_queue_t *linkg_wifi_tx_queue_create(uint32_t capacity);
void                   linkg_wifi_tx_queue_destroy(linkg_wifi_tx_queue_t *queue);
void                   linkg_wifi_tx_queue_clear(linkg_wifi_tx_queue_t *queue);

/****************************** 状态查询 ******************************/

uint32_t linkg_wifi_tx_queue_count(const linkg_wifi_tx_queue_t *queue);
uint32_t linkg_wifi_tx_queue_capacity(const linkg_wifi_tx_queue_t *queue);
uint32_t linkg_wifi_tx_queue_available(const linkg_wifi_tx_queue_t *queue);

/****************************** 队列操作 ******************************/

int      linkg_wifi_tx_queue_push_batch(linkg_wifi_tx_queue_t *queue, linkg_packet_t *const *packets, uint32_t count, linkg_path_t *path, const linkg_path_endpoint_t *destination, uint64_t enqueue_us, uint32_t *pushed_count);
uint32_t linkg_wifi_tx_queue_peek_batch(const linkg_wifi_tx_queue_t *queue, linkg_wifi_tx_queue_item_t *items, uint32_t capacity);
int      linkg_wifi_tx_queue_discard_batch(linkg_wifi_tx_queue_t *queue, uint32_t count);

#ifdef __cplusplus
}
#endif

#endif
