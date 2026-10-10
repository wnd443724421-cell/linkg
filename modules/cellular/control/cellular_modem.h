/**
 * @file cellular_modem.h
 * @brief LinkG RG255模组AT通道及基础配置管理接口
 *
 * @note AT通道由Modem模块持有，其他模块只能在Owner串行执行期间借用。
 */

#ifndef CELLULAR_MODEM_H
#define CELLULAR_MODEM_H

#include "linkg_cellular_config.h"

#include "at_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int  cellular_modem_start(const linkg_cellular_config_t *config);
void cellular_modem_stop(void);

/****************************** 运行配置 ******************************/

int cellular_modem_enable_runtime_urcs(void);

/****************************** 通道查询 ******************************/

at_channel_t *cellular_modem_get_channel(void);

#ifdef __cplusplus
}
#endif

#endif
