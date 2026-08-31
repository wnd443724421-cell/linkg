/**
 * @file cellular_link_heartbeat.h
 * @brief LinkG蜂窝业务链路心跳内部接口
 */

#ifndef CELLULAR_LINK_HEARTBEAT_H
#define CELLULAR_LINK_HEARTBEAT_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 前置声明 ******************************/

typedef struct linkg_cellular_link_heartbeat linkg_cellular_link_heartbeat_t;

/****************************** 初始化配置 ******************************/

typedef struct
{
    int            *socket_fds;     // 借用Cellular Link三个业务Socket数组
    const uint16_t *service_ports;  // 借用Cellular Link三个业务端口数组
    const char     *interface_name; // 借用蜂窝出口接口名称
} linkg_cellular_link_heartbeat_config_t;

/****************************** 生命周期 ******************************/

int  linkg_cellular_link_heartbeat_create(const linkg_cellular_link_heartbeat_config_t *config, linkg_cellular_link_heartbeat_t **out);
int  linkg_cellular_link_heartbeat_start(linkg_cellular_link_heartbeat_t *heartbeat, uint32_t link_id);
int  linkg_cellular_link_heartbeat_stop(linkg_cellular_link_heartbeat_t *heartbeat);
void linkg_cellular_link_heartbeat_destroy(linkg_cellular_link_heartbeat_t *heartbeat);

/****************************** 报文识别 ******************************/

bool linkg_cellular_link_heartbeat_is_packet(const uint8_t *data, uint32_t length, linkg_link_tx_class_t tx_class);

#ifdef __cplusplus
}
#endif

#endif
