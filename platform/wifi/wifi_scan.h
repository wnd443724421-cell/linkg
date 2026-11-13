/**
 * @file wifi_scan.h
 * @brief LinkG独占无线被动扫描接口
 */

#ifndef WIFI_SCAN_H
#define WIFI_SCAN_H

#include <stdbool.h>

#include "linkg_thread.h"
#include "linkg_wifi_scan_types.h"

/****************************** 类型定义 ******************************/

typedef void (*wifi_scan_progress_t)(void *context, const linkg_wifi_scan_result_t *result);

/****************************** 数据解析 ******************************/

void wifi_scan_result_init(linkg_wifi_scan_result_t *result);
int  wifi_scan_parse_survey(linkg_wifi_scan_result_t *result, const char *text);
int  wifi_scan_parse_bss(linkg_wifi_scan_result_t *result, const char *text);

/****************************** 独占扫描 ******************************/

int wifi_scan_platform_execute(linkg_thread_t *thread, linkg_wifi_scan_result_t *result, wifi_scan_progress_t progress, void *context, bool *released);

#endif
