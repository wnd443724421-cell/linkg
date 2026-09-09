/**
 * @file linkg_nat.h
 * @brief LinkG虚拟网络NAT管理接口
 */

#ifndef LINKG_NAT_H
#define LINKG_NAT_H

#include "linkg_network_config.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_nat_init(const linkg_network_config_t *network_config, bool uplink_enabled);
int linkg_nat_start(void);
int linkg_nat_stop(void);
int linkg_nat_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
