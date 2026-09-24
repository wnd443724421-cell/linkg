/**
 * @file network_manager.h
 * @brief LinkG网络服务管理线程接口
 */

#ifndef NETWORK_MANAGER_H
#define NETWORK_MANAGER_H

/****************************** 生命周期 ******************************/

int _linkg_network_manager_init(void);
int _linkg_network_manager_start(void);
int _linkg_network_manager_stop(void);
int _linkg_network_manager_deinit(void);

/****************************** 独立重启 ******************************/

int _linkg_network_manager_request_wifi_restart(void);
int _linkg_network_manager_request_cellular_restart(void);

#endif
