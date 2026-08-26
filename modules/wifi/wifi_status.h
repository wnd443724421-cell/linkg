/**
 * @file wifi_status.h
 * @brief LinkG Wi-Fi运行状态采集接口
 */

#ifndef WIFI_STATUS_H
#define WIFI_STATUS_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"
#include "linkg_wifi_config.h"
#include "linkg_wifi_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 状态信息 ******************************/

typedef struct
{
    bool                         valid;           // 是否已经获得有效快照
    bool                         running;         // 状态采集线程是否正在运行
    uint64_t                     generation;      // 成功更新次数
    uint64_t                     last_attempt_ms; // 最近一次采集时间
    uint64_t                     last_success_ms; // 最近一次成功更新时间
    int                          last_error;      // 最近一次采集结果
    linkg_wifi_status_snapshot_t snapshot;        // 当前Wi-Fi状态快照
} wifi_status_info_t;

/****************************** 生命周期 ******************************/

int wifi_status_init(linkg_device_role_t role, const linkg_wifi_config_t *config);
int wifi_status_start(void);
int wifi_status_stop(void);
int wifi_status_deinit(void);

/****************************** 状态读取 ******************************/

int wifi_status_get_info(wifi_status_info_t *info);

#ifdef __cplusplus
}
#endif

#endif
