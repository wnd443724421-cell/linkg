/**
 * @file linkg_tun.h
 * @brief LinkG TUN数据面接口
 */

#ifndef LINKG_TUN_H
#define LINKG_TUN_H

#include "linkg_network_config.h"
#include "linkg_packet_pool.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_tun_init(linkg_packet_pool_t *packet_pool);
int linkg_tun_start(void);
int linkg_tun_stop(void);
int linkg_tun_deinit(void);

/****************************** 动态配置 ******************************/

int linkg_tun_update_traffic_config(const linkg_network_traffic_config_t *traffic);

#ifdef __cplusplus
}
#endif

#endif
