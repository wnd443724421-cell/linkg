/**
 * @file rg255_rf.h
 * @brief RG255工厂模式强发命令接口
 */

#ifndef RG255_RF_H
#define RG255_RF_H

#include <stdbool.h>
#include <stdint.h>

#include "at_channel.h"

/****************************** 射频测试 ******************************/

int rg255_rf_get_mode(at_channel_t *channel, unsigned int *mode);
int rg255_rf_set_mode(at_channel_t *channel, bool enabled);
int rg255_rf_set_tx(at_channel_t *channel, uint32_t rat, uint32_t band, uint32_t bandwidth, uint32_t arfcn, bool enabled);

#endif
