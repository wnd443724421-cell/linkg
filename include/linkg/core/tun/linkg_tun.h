/**
 * @file linkg_tun.h
 * @brief LinkG TUN数据面接口
 */

#ifndef LINKG_TUN_H
#define LINKG_TUN_H

#include "linkg_packet_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_tun_init(linkg_packet_pool_t *packet_pool);
int linkg_tun_start(void);
int linkg_tun_stop(void);
int linkg_tun_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
