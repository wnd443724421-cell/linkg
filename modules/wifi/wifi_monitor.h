/**
 * @file wifi_monitor.h
 * @brief LinkG Wi-Fi STA连接状态机接口
 */

#ifndef WIFI_MONITOR_H
#define WIFI_MONITOR_H

#include <stdint.h>

#include "wifi_runtime.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 生命周期 ******************************/

int  wifi_monitor_init(void);
int  wifi_monitor_start(uint64_t now_ms, wifi_runtime_event_t *event);
void wifi_monitor_stop(void);
void wifi_monitor_deinit(void);

/****************************** 事件监听 ******************************/

int wifi_monitor_get_event_fd(void);
int wifi_monitor_handle_event_fd(uint64_t now_ms, wifi_runtime_event_t *event);
int wifi_monitor_handle_event_channel_error(uint64_t now_ms, wifi_runtime_event_t *event);

/****************************** 定时处理 ******************************/

uint64_t wifi_monitor_get_deadline(void);
int      wifi_monitor_process(uint64_t now_ms, wifi_runtime_event_t *event);

#ifdef __cplusplus
}
#endif

#endif
