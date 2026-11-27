/**
 * @file test_network_task.c
 * @brief Network真实线程独占任务及恢复回归
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */
#include "network_internal.h"
#include "network_manager.h"
#include "linkg_network_task.h"
#include "linkg_rf_test.h"
#include "linkg_time.h"
#include "linkg_switch.h"
#include "linkg_config.h"
#include "linkg_wifi.h"

#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

linkg_network_context_t g_network;
static atomic_int wifi_starts;
static atomic_int cell_starts;
static atomic_int runs;
static atomic_int finishes;
static atomic_int task_result;
static atomic_int restore_result;
static atomic_bool hold_task;
static atomic_bool fail_restore;
static atomic_bool release_resources;
static atomic_int operation_error;
static atomic_bool ap_recovery_required;
static atomic_bool config_busy;
static atomic_int config_attempts;
static atomic_int cell_finishes;      // 结束的Cellular独占任务数
static atomic_int cell_recoveries;    // 执行的RF安全恢复次数
static atomic_int cell_recover_error; // 模拟安全恢复结果
static atomic_int wifi_maintenance_begins;
static atomic_int wifi_maintenance_ends;

/**
 * @brief 运行模块替身直到真实Owner线程收到停止请求。
 */
static int owner_run(linkg_thread_t *thread)
{
    while (linkg_thread_is_running(thread))
    {
        usleep(1000U);
    }
    return 0;
}

/**
 * @brief 模拟RF安全状态检查，覆盖Manager阻止Owner的行为。
 */
int linkg_rf_test_recover_pending(void)
{
    atomic_fetch_add(&cell_recoveries, 1);
    return atomic_load(&cell_recover_error);
}

/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_wifi_init(void)
{
    g_network.wifi_worker.module_initialized = true;
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_wifi_start(void)
{
    atomic_fetch_add(&wifi_starts, 1);
    return atomic_load(&fail_restore) ? -EIO : 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_wifi_run(linkg_thread_t *thread)
{
    return owner_run(thread);
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_wifi_stop(void)
{
    if (atomic_exchange(&ap_recovery_required, false))
    {
        assert(atomic_load(&wifi_maintenance_begins) > atomic_load(&wifi_maintenance_ends));
    }

    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_wifi_deinit(void)
{
    g_network.wifi_worker.module_initialized = false;
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_cellular_init(void)
{
    g_network.cellular_worker.module_initialized = true;
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_cellular_start(void)
{
    atomic_fetch_add(&cell_starts, 1);
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_cellular_run(linkg_thread_t *thread)
{
    return owner_run(thread);
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_cellular_stop(void)
{
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int _linkg_network_cellular_deinit(void)
{
    g_network.cellular_worker.module_initialized = false;
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int linkg_discovery_set_cellular_enabled(bool enabled)
{
    (void)enabled;
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int linkg_switch_begin_maintenance(linkg_link_access_t access)
{
    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        atomic_fetch_add(&wifi_maintenance_begins, 1);
    }
    return 0;
}
/**
 * @brief 提供硬件或维护协调替身，验证真实Manager线程的生命周期。
 */
int linkg_switch_end_maintenance(linkg_link_access_t access)
{
    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        atomic_fetch_add(&wifi_maintenance_ends, 1);
    }
    return 0;
}

/**
 * @brief 模拟AP健康恢复请求，停止旧Owner后由替身清除。
 */
bool linkg_wifi_ap_recovery_required(void)
{
    return atomic_load(&ap_recovery_required);
}

/**
 * @brief 注入配置事务占用，验证Manager不抢占配置操作。
 */
int linkg_config_transaction_begin(void)
{
    atomic_fetch_add(&config_attempts, 1);
    return atomic_load(&config_busy) ? -EBUSY : 0;
}

/**
 * @brief 释放模拟配置事务。
 */
void linkg_config_transaction_end(void)
{
}

/**
 * @brief 独占任务运行时，普通Wi-Fi已释放而蜂窝Owner仍运行。
 */
static int task(linkg_thread_t *thread, void *context, bool *released)
{
    (void)context;
    assert(!g_network.wifi_worker.module_initialized);
    assert(!linkg_thread_is_started(&g_network.wifi_worker.thread));
    assert(linkg_thread_is_running(&g_network.cellular_worker.thread));
    atomic_fetch_add(&runs, 1);
    while (atomic_load(&hold_task) && linkg_thread_is_running(thread))
    {
        usleep(1000U);
    }
    *released = atomic_load(&release_resources);
    return linkg_thread_is_running(thread) ? atomic_load(&operation_error) : -ECANCELED;
}

/**
 * @brief 完成回调记录扫描及恢复两个独立结果。
 */
static void finished(void *context, int error, int restore)
{
    (void)context;
    atomic_store(&task_result, error);
    atomic_store(&restore_result, restore);
    atomic_fetch_add(&finishes, 1);
}

/**
 * @brief 有界等待真实线程状态，避免测试用固定延时猜测进度。
 */
static void wait_count(atomic_int *counter, int expected)
{
    for (unsigned i = 0U; i < 10000U && atomic_load(counter) < expected; i++)
    {
        usleep(1000U);
    }
    if (atomic_load(counter) < expected)
    {
        fprintf(stderr, "timeout expected=%d wifi=%d cell=%d runs=%d finishes=%d task=%d restore=%d phaseactive=%d owner_started=%d owner_running=%d attempted=%d success=%d\n", expected, atomic_load(&wifi_starts), atomic_load(&cell_starts), atomic_load(&runs), atomic_load(&finishes), atomic_load(&task_result), atomic_load(&restore_result), linkg_network_wifi_task_active(), linkg_thread_is_started(&g_network.wifi_worker.thread), linkg_thread_is_running(&g_network.wifi_worker.thread), g_network.wifi_worker.start_attempted, g_network.wifi_worker.start_succeeded);
    }
    assert(atomic_load(counter) >= expected);
}

/**
 * @brief 模拟RF任务未能释放串口。
 */
static int cell_task_unreleased(linkg_thread_t *thread, void *context, bool *released)
{
    (void)thread;
    (void)context;
    assert(!linkg_thread_is_started(&g_network.cellular_worker.thread));
    assert(linkg_thread_is_running(&g_network.wifi_worker.thread));
    *released = false;
    return -EREMOTEIO;
}

/**
 * @brief 验证RF清理失败未误报为正常Owner恢复失败。
 */
static void cell_task_finished(void *context, int task_error, int restore_error, bool released)
{
    (void)context;
    assert(task_error == -EREMOTEIO && restore_error == 0 && !released);
    atomic_fetch_add(&cell_finishes, 1);
}

/**
 * @brief 覆盖成功、重复提交、独立蜂窝重启、取消、测量错误和恢复失败。
 */
int main(void)
{
    assert(linkg_time_init() == 0);
    memset(&g_network, 0, sizeof(g_network));
    assert(pthread_mutex_init(&g_network.lock, NULL) == 0);
    g_network.state = LINKG_NETWORK_STATE_RUNNING;
    g_network.wifi_config.enabled = true;
    g_network.cellular_config.enabled = true;
    atomic_store(&release_resources, true);
    assert(_linkg_network_manager_init() == 0);
    assert(_linkg_network_manager_start() == 0);
    wait_count(&wifi_starts, 1);
    wait_count(&cell_starts, 1);
    usleep(10000U);

    // AP健康故障必须等待配置事务，并先BEGIN Maintenance再停止旧Owner。
    atomic_store(&config_busy, true);
    atomic_store(&ap_recovery_required, true);
    assert(linkg_thread_wakeup(&g_network.manager_thread) == 0);
    wait_count(&config_attempts, 1);
    assert(atomic_load(&wifi_starts) == 1);
    assert(atomic_load(&wifi_maintenance_begins) == 0);

    atomic_store(&config_busy, false);
    assert(linkg_thread_wakeup(&g_network.manager_thread) == 0);
    wait_count(&wifi_starts, 2);
    wait_count(&wifi_maintenance_ends, 1);
    assert(atomic_load(&wifi_maintenance_begins) == 1);
    assert(atomic_load(&cell_starts) == 1);
    assert(!atomic_load(&ap_recovery_required));

    atomic_store(&hold_task, true);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == -EBUSY);
    assert(_linkg_network_manager_request_wifi_restart() == -EBUSY);
    wait_count(&runs, 1);
    assert(_linkg_network_manager_request_cellular_restart() == 0);
    wait_count(&cell_starts, 2);
    assert(linkg_network_wifi_task_cancel() == 0);
    wait_count(&finishes, 1);
    assert(atomic_load(&task_result) == -ECANCELED && atomic_load(&restore_result) == 0);
    assert(linkg_thread_is_running(&g_network.wifi_worker.thread));
    atomic_store(&hold_task, false);
    atomic_store(&operation_error, -ENODATA);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    wait_count(&finishes, 2);
    assert(atomic_load(&task_result) == -ENODATA && atomic_load(&restore_result) == 0);
    atomic_store(&operation_error, 0);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    wait_count(&finishes, 3);
    assert(atomic_load(&task_result) == 0 && atomic_load(&restore_result) == 0);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    assert(linkg_network_wifi_task_cancel() == 0);
    wait_count(&finishes, 4);
    assert(atomic_load(&task_result) == -ECANCELED && atomic_load(&restore_result) == 0);
    atomic_store(&fail_restore, true);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    wait_count(&finishes, 5);
    assert(atomic_load(&restore_result) != 0);
    assert(!linkg_thread_is_started(&g_network.wifi_worker.thread));
    atomic_store(&fail_restore, false);
    int previous_starts = atomic_load(&wifi_starts);
    assert(_linkg_network_manager_request_wifi_restart() == 0);
    wait_count(&wifi_starts, previous_starts + 1);
    usleep(10000U);
    int previous_cell_starts = atomic_load(&cell_starts);
    assert(linkg_network_cellular_task_start(cell_task_unreleased, cell_task_finished, NULL) == 0);
    wait_count(&cell_finishes, 1);
    assert(!linkg_thread_is_started(&g_network.cellular_worker.thread));
    atomic_store(&cell_recover_error, -ENODEV);
    assert(linkg_network_cellular_task_start(cell_task_unreleased, cell_task_finished, NULL) == -EINPROGRESS);
    wait_count(&cell_recoveries, 2);
    usleep(200000U);
    assert(atomic_load(&cell_starts) == previous_cell_starts);
    atomic_store(&cell_recover_error, 0);
    assert(linkg_network_cellular_task_start(cell_task_unreleased, cell_task_finished, NULL) == -EINPROGRESS);
    wait_count(&cell_recoveries, 3);
    wait_count(&cell_starts, previous_cell_starts + 1);
    assert(atomic_load(&cell_finishes) == 1);

    atomic_store(&hold_task, true);
    int previous_runs = atomic_load(&runs);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    wait_count(&runs, previous_runs + 1);
    assert(_linkg_network_manager_stop() == 0);
    wait_count(&finishes, 6);
    assert(atomic_load(&restore_result) == -ESHUTDOWN);
    assert(_linkg_network_manager_deinit() == 0);
    assert(_linkg_network_manager_init() == 0);
    assert(_linkg_network_manager_start() == 0);
    wait_count(&wifi_starts, previous_starts + 2);
    usleep(10000U);
    atomic_store(&hold_task, false);
    atomic_store(&release_resources, false);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == 0);
    wait_count(&finishes, 7);
    assert(atomic_load(&restore_result) == -EBUSY);
    assert(_linkg_network_manager_request_wifi_restart() == -EBUSY);
    assert(linkg_network_wifi_task_start(task, finished, NULL) == -EBUSY);
    assert(!linkg_thread_is_started(&g_network.wifi_worker.thread));
    assert(_linkg_network_manager_stop() == -EBUSY);
    assert(_linkg_network_manager_deinit() == -EBUSY);
    // 资源故障锁存由产品生命周期保留；替身未实际持有硬件，测试仅回收线程对象。
    linkg_thread_deinit(&g_network.manager_thread);
    linkg_thread_deinit(&g_network.wifi_worker.thread);
    linkg_thread_deinit(&g_network.cellular_worker.thread);
    assert(pthread_mutex_destroy(&g_network.lock) == 0);
    puts("network task lifecycle regression passed");
    return 0;
}
