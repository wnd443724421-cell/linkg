/**
 * @file wifi_rx.h
 * @brief LinkG Wi-Fi接收模块接口
 */

#ifndef WIFI_RX_H
#define WIFI_RX_H

#include <stdint.h>

#include "linkg_link.h"
#include "linkg_packet_pool.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 类型定义 ******************************/

typedef struct linkg_wifi_rx linkg_wifi_rx_t; // Wi-Fi接收模块实例

/****************************** 生命周期 ******************************/

linkg_wifi_rx_t *linkg_wifi_rx_create(uint32_t capacity, linkg_packet_pool_t *packet_pool, int *socket_fds, const uint16_t *service_ports);
void             linkg_wifi_rx_destroy(linkg_wifi_rx_t *rx);
int              linkg_wifi_rx_start(linkg_wifi_rx_t *rx);
int              linkg_wifi_rx_stop(linkg_wifi_rx_t *rx);

/****************************** 数据接收 ******************************/

int linkg_wifi_rx_get_fd(linkg_wifi_rx_t *rx);
int linkg_wifi_rx_receive_batch(linkg_wifi_rx_t *rx, linkg_link_rx_item_t *items, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
