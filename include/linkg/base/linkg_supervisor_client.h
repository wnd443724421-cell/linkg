/**
 * @file linkg_supervisor_client.h
 * @brief LinkG进程守护通知客户端接口
 */

#ifndef LINKG_SUPERVISOR_CLIENT_H
#define LINKG_SUPERVISOR_CLIENT_H

#include <stdbool.h>

/****************************** 生命周期 ******************************/

int  linkg_supervisor_client_init(void);
void linkg_supervisor_client_deinit(void);

/****************************** 状态通知 ******************************/

int linkg_supervisor_client_ready(void);
int linkg_supervisor_client_restarting(void);
bool linkg_supervisor_client_can_upgrade(void);

#endif
