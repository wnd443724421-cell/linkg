/**
 * @file cellular_tx.h
 * @brief LinkG蜂窝链路发送模块接口
 */

#ifndef CELLULAR_TX_H
#define CELLULAR_TX_H

#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_cellular_tx linkg_cellular_tx_t;

/****************************** 生命周期 ******************************/

linkg_cellular_tx_t *linkg_cellular_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports, const char *interface_name);
void                 linkg_cellular_tx_destroy(linkg_cellular_tx_t *tx);
int                  linkg_cellular_tx_start(linkg_cellular_tx_t *tx);
void                 linkg_cellular_tx_stop(linkg_cellular_tx_t *tx);

/****************************** 队列清理 ******************************/

int linkg_cellular_tx_purge_path(linkg_cellular_tx_t *tx, linkg_path_t *path, uint32_t *purged_count);

/****************************** 数据发送 ******************************/

int linkg_cellular_tx_submit(linkg_cellular_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results);

#ifdef __cplusplus
}
#endif

#endif
