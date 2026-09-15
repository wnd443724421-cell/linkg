/**
 * @file linkg_discovery.h
 * @brief LinkG设备发现接口
 */

#ifndef LINKG_DISCOVERY_H
#define LINKG_DISCOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/****************************** 生命周期 ******************************/

int linkg_discovery_init(void);
int linkg_discovery_start(void);
int linkg_discovery_stop(void);
int linkg_discovery_deinit(void);

/****************************** 状态查询 ******************************/

int linkg_discovery_get_network_node_count(uint32_t *count);

#ifdef __cplusplus
}
#endif

#endif
