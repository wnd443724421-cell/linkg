/**
 * @file rg255_runtime_urc.h
 * @brief RG255运行期URC配置接口
 */

#ifndef RG255_RUNTIME_URC_H
#define RG255_RUNTIME_URC_H

#include <stdbool.h>

#include "at_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** URC配置 ******************************/

int rg255_cmd_set_eps_registration_urc(at_channel_t *channel, bool enable);
int rg255_cmd_set_5g_registration_urc(at_channel_t *channel, bool enable);
int rg255_cmd_set_signal_urc(at_channel_t *channel, bool enable);

#ifdef __cplusplus
}
#endif

#endif
