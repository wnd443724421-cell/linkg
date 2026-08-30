/**
 * @file discovery_cellular.h
 * @brief LinkG设备发现Cellular Channel内部接口
 */

#ifndef DISCOVERY_CELLULAR_H
#define DISCOVERY_CELLULAR_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_discovery_cellular_init(void);
int linkg_discovery_cellular_start(void);
int linkg_discovery_cellular_quiesce(void);
int linkg_discovery_cellular_stop(void);
int linkg_discovery_cellular_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
