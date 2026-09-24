/**
 * @file network_wifi.h
 * @brief LinkG网络服务Wi-Fi管理接口
 */

#ifndef NETWORK_WIFI_H
#define NETWORK_WIFI_H

#include "linkg_thread.h"

/****************************** 生命周期 ******************************/

int _linkg_network_wifi_init(void);
int _linkg_network_wifi_start(void);
int _linkg_network_wifi_run(linkg_thread_t *owner_thread);
int _linkg_network_wifi_stop(void);
int _linkg_network_wifi_deinit(void);

/****************************** 独立重启 ******************************/

int _linkg_network_wifi_restart(void);

#endif
