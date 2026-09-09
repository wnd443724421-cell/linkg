/**
 * @file linkg_udhcp.h
 * @brief LinkG Ethernet DHCP服务管理接口
 */

#ifndef LINKG_UDHCP_H
#define LINKG_UDHCP_H

#include "linkg_network_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_udhcp_init(const linkg_network_config_t *network_config);
int linkg_udhcp_start(void);
int linkg_udhcp_stop(void);
int linkg_udhcp_deinit(void);

/****************************** 配置接口 ******************************/

int linkg_udhcp_reload(const linkg_network_config_t *network_config);

#ifdef __cplusplus
}
#endif

#endif
