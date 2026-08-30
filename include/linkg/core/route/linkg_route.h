/**
 * @file linkg_route.h
 * @brief LinkG虚拟节点路由管理接口
 */

#ifndef LINKG_ROUTE_H
#define LINKG_ROUTE_H

#include <stdint.h>

#include "linkg_network_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_route_init(const linkg_network_config_t *network_config);
int linkg_route_start(void);
int linkg_route_stop(void);
int linkg_route_deinit(void);

/****************************** 路由管理 ******************************/

int linkg_route_add_node(uint8_t node_id);
int linkg_route_remove_node(uint8_t node_id);

#ifdef __cplusplus
}
#endif

#endif
