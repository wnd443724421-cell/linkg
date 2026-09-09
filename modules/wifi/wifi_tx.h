/**
 * @file wifi_tx.h
 * @brief LinkG Wi-Fi发送模块接口
 */

#ifndef WIFI_TX_H
#define WIFI_TX_H

#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_wifi_tx linkg_wifi_tx_t;

/****************************** 生命周期 ******************************/

// socket_fds和service_ports仅借用，生命周期由Wi-Fi Link保证。
linkg_wifi_tx_t *linkg_wifi_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports);
void             linkg_wifi_tx_destroy(linkg_wifi_tx_t *tx);
int              linkg_wifi_tx_start(linkg_wifi_tx_t *tx);
void             linkg_wifi_tx_stop(linkg_wifi_tx_t *tx);

/****************************** 数据发送 ******************************/

// results为0表示Wi-Fi已接受发送责任，Packet可能已提交Socket，也可能进入对应REALTIME/VIDEO/DATA等待队列；负数表示拒绝。
int linkg_wifi_tx_submit(linkg_wifi_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results);

#ifdef __cplusplus
}
#endif

#endif
