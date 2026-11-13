/**
 * @file linkg_wifi_scan.h
 * @brief LinkG Wi-Fi独占扫描接口
 */
#ifndef LINKG_WIFI_SCAN_H
#define LINKG_WIFI_SCAN_H

#include "linkg_thread.h"
#include "linkg_wifi_scan_types.h"

typedef void (*linkg_wifi_scan_progress_t)(void *context, const linkg_wifi_scan_result_t *result);

void linkg_wifi_scan_result_init(linkg_wifi_scan_result_t *result);
int  linkg_wifi_scan_execute(linkg_thread_t *thread, linkg_wifi_scan_result_t *result, linkg_wifi_scan_progress_t progress, void *context, bool *released);

#endif
