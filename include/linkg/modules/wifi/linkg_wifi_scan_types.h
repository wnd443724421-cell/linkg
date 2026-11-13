/**
 * @file linkg_wifi_scan_types.h
 * @brief LinkG信道扫描测量类型
 */

#ifndef LINKG_WIFI_SCAN_TYPES_H
#define LINKG_WIFI_SCAN_TYPES_H

#include <stdbool.h>
#include <stdint.h>

/****************************** 采样范围 ******************************/

#define LINKG_WIFI_SCAN_CHANNEL_COUNT 5U   // 普通用户开放信道数量
#define LINKG_WIFI_SCAN_ROUND_COUNT   20U  // 每次任务的被动扫描轮数
#define LINKG_WIFI_SCAN_BSS_MAX       128U // 最多记录的目标频段AP数量

/****************************** 测量结果 ******************************/

typedef struct
{
    uint32_t  channel;                                // 802.11信道编号
    uint32_t  frequency_mhz;                          // 中心频率，单位MHz
    uint32_t  samples;                                // 有效繁忙时间样本数量
    uint64_t  active_ms;                              // 累计有效采样时间，单位ms
    uint64_t  busy_ms;                                // 累计信道繁忙时间，单位ms
    double    peak_busy_percent;                      // 单轮最高繁忙率
    int32_t   noise_dbm[LINKG_WIFI_SCAN_ROUND_COUNT]; // 有效噪声样本，不计算dBm算术平均
    uint32_t  noise_samples;                          // 有效噪声样本数量
} linkg_wifi_scan_channel_t;

typedef struct
{
    char      bssid[18];     // AP的BSSID文本
    char      ssid[129];     // WPA返回的SSID文本，保留转义形式
    uint32_t  frequency_mhz; // AP频率，单位MHz
    int32_t   signal_dbm;    // AP信号强度，单位dBm
} linkg_wifi_scan_bss_t;

typedef struct
{
    linkg_wifi_scan_channel_t  channels[LINKG_WIFI_SCAN_CHANNEL_COUNT]; // 各信道测量结果
    linkg_wifi_scan_bss_t      bss[LINKG_WIFI_SCAN_BSS_MAX];            // 本次扫描观察到的AP
    uint32_t                   bss_count;                               // 有效AP记录数量
    uint32_t                   rounds;                                  // 已完成的扫描轮数
    uint32_t                   bss_rounds;                              // AP列表读取成功的轮数
    uint32_t                   survey_failures;                         // 无有效survey结果的轮数
    bool                       bss_truncated;                           // AP列表是否达到容量上限
} linkg_wifi_scan_result_t;

#endif
