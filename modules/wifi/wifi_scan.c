/**
 * @file wifi_scan.c
 * @brief LinkG Wi-Fi独占扫描入口及结果初始化
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "linkg_wifi_scan.h"

#include <errno.h>

#include "wifi_scan.h"

/****************************** 扫描接口 ******************************/

/**
 * @brief 初始化用户信道和缺失测量字段。
 */
void linkg_wifi_scan_result_init(linkg_wifi_scan_result_t *result)
{
    wifi_scan_result_init(result);
}

/**
 * @brief 原Owner退出后初始化测量结果，并调用平台完成独占测量与清理。
 */
int linkg_wifi_scan_execute(linkg_thread_t *thread, linkg_wifi_scan_result_t *result, linkg_wifi_scan_progress_t progress, void *context, bool *released)
{
    if (thread == NULL || result == NULL || released == NULL)
    {
        return -EINVAL;
    }

    wifi_scan_result_init(result);
    if (progress != NULL)
    {
        progress(context, result);
    }

    return wifi_scan_platform_execute(thread, result, progress, context, released);
}
