/**
 * @file test_scan_state.c
 * @brief 扫描结果跨服务重载清理及活动任务保护回归
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */
#include "linkg_network_scan.h"
#include "linkg_network_task.h"
#include "linkg_wifi_scan.h"
#include "wifi_scan.h"

#include <assert.h>
#include <errno.h>
#include <string.h>

static linkg_network_wifi_task_finish_t completed;
static int submit_error;

/**
 * @brief 任务只记录完成回调，不操作无线硬件。
 */
int linkg_network_wifi_task_start(linkg_network_wifi_task_run_t run, linkg_network_wifi_task_finish_t finish, void *context)
{
    (void)run;
    (void)context;
    completed = finish;
    return submit_error;
}

int linkg_network_wifi_task_cancel(void)
{
    return 0;
}

void linkg_wifi_scan_result_init(linkg_wifi_scan_result_t *result)
{
    wifi_scan_result_init(result);
}

int linkg_wifi_scan_execute(linkg_thread_t *thread, linkg_wifi_scan_result_t *result, linkg_wifi_scan_progress_t progress, void *context, bool *released)
{
    (void)thread;
    (void)result;
    (void)progress;
    (void)context;
    *released = true;
    return 0;
}

/**
 * @brief 活动期间拒绝清理，结束后清除旧结果和编号，提交失败仍保留旧结果。
 */
int main(void)
{
    linkg_network_scan_status_t status;
    uint32_t job;

    assert(linkg_network_scan_reset() == 0);
    assert(linkg_network_scan_start(&job) == 0);
    assert(linkg_network_scan_reset() == -EBUSY);
    completed(NULL, 0, 0);
    assert(linkg_network_scan_get(&status) == 0 && status.state == LINKG_NETWORK_SCAN_COMPLETED);
    submit_error = -EBUSY;
    assert(linkg_network_scan_start(&job) == -EBUSY);
    assert(linkg_network_scan_get(&status) == 0 && status.state == LINKG_NETWORK_SCAN_COMPLETED);
    assert(linkg_network_scan_reset() == 0);
    assert(linkg_network_scan_get(&status) == 0);
    assert(status.job_id == 0U && !status.active && status.state == LINKG_NETWORK_SCAN_IDLE);
    assert(status.result.bss_count == 0U && status.result.channels[4].channel == 165U);
    return 0;
}
