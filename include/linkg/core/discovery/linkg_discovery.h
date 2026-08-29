/**
 * @file linkg_discovery.h
 * @brief LinkG设备发现接口
 */

#ifndef LINKG_DISCOVERY_H
#define LINKG_DISCOVERY_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int linkg_discovery_init(void);
int linkg_discovery_start(void);
int linkg_discovery_stop(void);
int linkg_discovery_deinit(void);

#ifdef __cplusplus
}
#endif

#endif
