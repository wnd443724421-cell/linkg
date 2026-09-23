/**
 * @file linkg_ethernet.h
 * @brief LinkG Ethernet运行控制接口
 */

#ifndef LINKG_ETHERNET_H
#define LINKG_ETHERNET_H

#include "linkg_network_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_ethernet_init(const linkg_network_ipv4_config_t *config);
int linkg_ethernet_start(void);
int linkg_ethernet_stop(void);
int linkg_ethernet_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
