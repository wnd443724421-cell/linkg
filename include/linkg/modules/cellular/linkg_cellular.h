/**
 * @file linkg_cellular.h
 * @brief LinkG蜂窝网络运行控制接口
 */

#ifndef LINKG_CELLULAR_H
#define LINKG_CELLULAR_H

#include <stdbool.h>

#include "linkg_cellular_config.h"
#include "linkg_packet_pool.h"
#include "linkg_cellular_status.h"
#include "linkg_thread.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 可用性类型 ******************************/

typedef enum
{
    LINKG_CELLULAR_AVAILABLE_DATA = 0, // 本机Cellular IPv6数据通道就绪
    LINKG_CELLULAR_AVAILABLE_IPV4,     // IPv4公网可达
    LINKG_CELLULAR_AVAILABLE_IPV6      // IPv6公网可达
} linkg_cellular_available_type_t;

/****************************** 生命周期 ******************************/

int linkg_cellular_init(const linkg_cellular_config_t *config, bool path_enabled, linkg_packet_pool_t *packet_pool);
int linkg_cellular_start(void);
int linkg_cellular_run(linkg_thread_t *owner_thread);
int linkg_cellular_stop(void);
int linkg_cellular_deinit(void);

/****************************** 状态读取 ******************************/

int linkg_cellular_get_status(linkg_cellular_status_snapshot_t *snapshot);
int linkg_cellular_get_internet_available(linkg_cellular_available_type_t type, bool *available);

#ifdef __cplusplus
}
#endif

#endif
