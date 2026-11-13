/**
 * @file network_scan.c
 * @brief LinkG信道扫描任务状态及网络独占维护接入
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "linkg_network_scan.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>
#include <time.h>

#include "linkg_network_task.h"

#include "linkg_wifi_scan.h"

/****************************** 全局上下文 ******************************/

static pthread_mutex_t             g_scan_lock = PTHREAD_MUTEX_INITIALIZER; // 扫描快照锁
static linkg_network_scan_status_t g_scan_status;                          // 最近一次任务及测量快照

/****************************** 任务回调 ******************************/

/**
 * @brief 发布单轮测量快照，不在状态锁内执行驱动操作。
 */
static void _linkg_network_scan_progress(void *context, const linkg_wifi_scan_result_t *result)
{
    (void)context;
    pthread_mutex_lock(&g_scan_lock);
    g_scan_status.state = LINKG_NETWORK_SCAN_RUNNING;
    g_scan_status.result = *result;
    pthread_mutex_unlock(&g_scan_lock);
}

/**
 * @brief 在独占线程完成硬件测量，正常Owner已经退出。
 */
static int _linkg_network_scan_run(linkg_thread_t *thread, void *context, bool *released)
{
    linkg_wifi_scan_result_t  result;
    int                       ret;

    (void)context;
    linkg_wifi_scan_result_init(&result);
    _linkg_network_scan_progress(NULL, &result);
    ret = linkg_wifi_scan_execute(thread, &result, _linkg_network_scan_progress, NULL, released);
    pthread_mutex_lock(&g_scan_lock);
    g_scan_status.result = result;
    g_scan_status.scan_error = ret;
    g_scan_status.state = LINKG_NETWORK_SCAN_RESTORING;
    pthread_mutex_unlock(&g_scan_lock);
    return ret;
}

/**
 * @brief 分别记录扫描结果与恢复结果，仅完整收尾后结束任务。
 */
static void _linkg_network_scan_finish(void *context, int task_error, int restore_error)
{
    (void)context;
    pthread_mutex_lock(&g_scan_lock);
    g_scan_status.scan_error = task_error;
    g_scan_status.restore_error = restore_error;
    g_scan_status.active = false;
    if (restore_error != 0)
    {
        g_scan_status.state = LINKG_NETWORK_SCAN_FAILED;
    }
    else if (task_error == -ECANCELED)
    {
        g_scan_status.state = LINKG_NETWORK_SCAN_CANCELLED;
    }
    else
    {
        g_scan_status.state = task_error == 0 ? LINKG_NETWORK_SCAN_COMPLETED : LINKG_NETWORK_SCAN_FAILED;
    }
    pthread_mutex_unlock(&g_scan_lock);
}

/****************************** 任务操作 ******************************/

/**
 * @brief Network重新初始化时清除上一轮服务的测量，不清理活动任务。
 */
int linkg_network_scan_reset(void)
{
    pthread_mutex_lock(&g_scan_lock);
    if (g_scan_status.active)
    {
        pthread_mutex_unlock(&g_scan_lock);
        return -EBUSY;
    }
    memset(&g_scan_status, 0, sizeof(g_scan_status));
    linkg_wifi_scan_result_init(&g_scan_status.result);
    pthread_mutex_unlock(&g_scan_lock);
    return 0;
}


/**
 * @brief 异步提交一次五信道扫描，不改变用户配置或自动选择信道。
 */
int linkg_network_scan_start(uint32_t *job_id)
{
    linkg_network_scan_status_t  previous;
    uint32_t                     next_id;
    int                          ret;

    if (job_id == NULL)
    {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_scan_lock);
    if (g_scan_status.active)
    {
        pthread_mutex_unlock(&g_scan_lock);
        return -EBUSY;
    }
    next_id = g_scan_status.job_id + 1U;
    if (next_id == 0U)
    {
        next_id = 1U;
    }
    previous = g_scan_status;
    // 先发布任务状态；提交成功后回调可能立即执行。
    memset(&g_scan_status, 0, sizeof(g_scan_status));
    linkg_wifi_scan_result_init(&g_scan_status.result);
    g_scan_status.job_id = next_id;
    g_scan_status.started_at = (uint64_t)time(NULL);
    g_scan_status.active = true;
    g_scan_status.state = LINKG_NETWORK_SCAN_STARTING;
    ret = linkg_network_wifi_task_start(_linkg_network_scan_run, _linkg_network_scan_finish, NULL);
    if (ret != 0)
    {
        g_scan_status = previous;
    }
    if (ret == 0)
    {
        *job_id = next_id;
    }
    pthread_mutex_unlock(&g_scan_lock);
    return ret;
}

/**
 * @brief 返回任务与测量结果的一致性快照。
 */
int linkg_network_scan_get(linkg_network_scan_status_t *status)
{
    if (status == NULL)
    {
        return -EINVAL;
    }
    pthread_mutex_lock(&g_scan_lock);
    *status = g_scan_status;
    if (status->job_id == 0U)
    {
        linkg_wifi_scan_result_init(&status->result);
    }
    pthread_mutex_unlock(&g_scan_lock);
    return 0;
}

/**
 * @brief 只取消指定任务，过期页面不得取消后来启动的扫描。
 */
int linkg_network_scan_cancel(uint32_t job_id)
{
    int ret;

    pthread_mutex_lock(&g_scan_lock);
    if (job_id == 0U || g_scan_status.job_id != job_id)
    {
        pthread_mutex_unlock(&g_scan_lock);
        return -ESTALE;
    }
    if (!g_scan_status.active)
    {
        pthread_mutex_unlock(&g_scan_lock);
        return -ENOENT;
    }
    ret = linkg_network_wifi_task_cancel();
    pthread_mutex_unlock(&g_scan_lock);
    return ret;
}

/**
 * @brief 查询扫描是否仍在停止、测量或恢复阶段。
 */
bool linkg_network_scan_active(void)
{
    bool active;

    pthread_mutex_lock(&g_scan_lock);
    active = g_scan_status.active;
    pthread_mutex_unlock(&g_scan_lock);
    return active;
}
