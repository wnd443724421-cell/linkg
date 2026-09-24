/**
 * @file network_cellular.h
 * @brief LinkG网络服务Cellular管理接口
 */

#ifndef NETWORK_CELLULAR_H
#define NETWORK_CELLULAR_H

#include "linkg_thread.h"

/****************************** 生命周期 ******************************/

int _linkg_network_cellular_init(void);
int _linkg_network_cellular_start(void);
int _linkg_network_cellular_run(linkg_thread_t *owner_thread);
int _linkg_network_cellular_stop(void);
int _linkg_network_cellular_deinit(void);

/****************************** 独立重启 ******************************/

int _linkg_network_cellular_restart(void);

#endif
