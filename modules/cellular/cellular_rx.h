/**
 * @file cellular_rx.h
 * @brief LinkG蜂窝链路接收模块接口
 */

#ifndef CELLULAR_RX_H
#define CELLULAR_RX_H

#include <stdint.h>

#include "linkg_link.h"
#include "linkg_packet_pool.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_cellular_rx linkg_cellular_rx_t;

/****************************** 生命周期 ******************************/

linkg_cellular_rx_t *linkg_cellular_rx_create(uint32_t capacity, linkg_packet_pool_t *packet_pool, int *socket_fds, const uint16_t *service_ports, const char *interface_name);
void                 linkg_cellular_rx_destroy(linkg_cellular_rx_t *rx);
int                  linkg_cellular_rx_start(linkg_cellular_rx_t *rx);
int                  linkg_cellular_rx_stop(linkg_cellular_rx_t *rx);

/****************************** 数据接收 ******************************/

int linkg_cellular_rx_get_fd(linkg_cellular_rx_t *rx);
int linkg_cellular_rx_receive_batch(linkg_cellular_rx_t *rx, linkg_link_rx_item_t *items, uint32_t capacity);

#ifdef __cplusplus
}
#endif

#endif
