/**
 * @file linkg_network_scan.h
 * @brief LinkG信道扫描任务接口
 */

#ifndef LINKG_NETWORK_SCAN_H
#define LINKG_NETWORK_SCAN_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_wifi_scan_types.h"

/****************************** 任务状态 ******************************/

typedef enum
{
    LINKG_NETWORK_SCAN_IDLE = 0,       // 尚未执行扫描
    LINKG_NETWORK_SCAN_STARTING,       // 等待原Owner停止
    LINKG_NETWORK_SCAN_RUNNING,        // 正在被动扫描
    LINKG_NETWORK_SCAN_RESTORING,      // 正在恢复原服务
    LINKG_NETWORK_SCAN_COMPLETED,      // 扫描与服务恢复均成功
    LINKG_NETWORK_SCAN_CANCELLED,      // 已取消并完成收尾
    LINKG_NETWORK_SCAN_FAILED          // 扫描或恢复失败
} linkg_network_scan_state_t;

typedef struct
{
    linkg_wifi_scan_result_t    result;        // 本次测量结果，允许部分信道缺失
    linkg_network_scan_state_t  state;         // 任务当前状态
    uint32_t                    job_id;        // 本进程内递增任务编号
    uint64_t                    started_at;    // 开始时间，Unix秒
    int                         scan_error;    // 扫描操作结果，0表示成功
    int                         restore_error; // 原Wi-Fi服务恢复结果
    bool                        active;        // 是否仍在执行或恢复
} linkg_network_scan_status_t;

/****************************** 任务操作 ******************************/

int  linkg_network_scan_reset(void);
int  linkg_network_scan_start(uint32_t *job_id);
int  linkg_network_scan_get(linkg_network_scan_status_t *status);
int  linkg_network_scan_cancel(uint32_t job_id);
bool linkg_network_scan_active(void);

#endif
