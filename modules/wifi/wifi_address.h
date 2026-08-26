/**
 * @file wifi_address.h
 * @brief LinkG Wi-Fi IPv4地址派生接口
 */

#ifndef WIFI_ADDRESS_H
#define WIFI_ADDRESS_H

#include <stdint.h>

#include "linkg_network_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 地址派生 ******************************/

int wifi_address_from_node_id(uint8_t node_id, linkg_network_ipv4_config_t *config);

#ifdef __cplusplus
}
#endif

#endif
