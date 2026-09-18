/**
 * @file switch_rx.h
 * @brief LinkG链路切换Transport接收接口
 */

#ifndef SWITCH_RX_H
#define SWITCH_RX_H

#include <stdint.h>

#include "linkg_transport.h"

/****************************** Transport接收 ******************************/

int linkg_switch_transport_receive(const linkg_transport_delivery_t *items, uint32_t count, void *user_data);

#endif
