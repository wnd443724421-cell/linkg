
/**
 * @file network_manager.c
 * @brief LinkG网络服务管理线程实现
 */

#include "network_manager.h"

#include <errno.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_thread.h"

#include "network_cellular.h"
#include "network_internal.h"
#include "network_wifi.h"

/****************************** 模块常量 ******************************/

#define LINKG_NETWORK_MANAGER_THREAD_NAME  "network-mgr"  // Network管理线程名称
#define LINKG_NETWORK_WIFI_THREAD_NAME     "network-wifi" // Wi-Fi Owner线程名称
#define LINKG_NETWORK_CELLULAR_THREAD_NAME "network-cell" // Cellular Owner线程名称

#define LINKG_NETWORK_MANAGER_POLL_MS      1000          // Manager最长等待时间

/****************************** 管理上下文 ******************************/

typedef struct
{
    int  shutdown_result;            // Manager最近一次退出清理结果

    bool wifi_restart_requested;     // Wi-Fi独立重启请求
    bool cellular_restart_requested; // Cellular独立重启请求

} linkg_network_manager_context_t;

/****************************** 全局上下文 ******************************/

static linkg_network_manager_context_t g_network_manager;

/****************************** 内部辅助 ******************************/

/**
 * @brief 记录首个错误。
 */
static void _linkg_network_manager_record_error(int *first_error, int error)
{
    if (first_error != NULL && *first_error == 0 && error != 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 清理Worker当前模块资源。
 *
 * Stop成功后才执行Deinit，清理失败时保留模块初始化状态。
 */
static int _linkg_network_worker_cleanup(linkg_network_worker_t *worker)
{
    int ret;

    if (!worker->module_initialized)
    {
        return 0;
    }

    ret = worker->stop();
    if (ret != 0)
    {
        return ret;
    }

    return worker->deinit();
}

/****************************** Worker线程 ******************************/

/**
 * @brief Worker Owner线程入口。
 *
 * 每轮Owner执行模块Init、Start、Run、Stop及Deinit。
 * 模块失败后退出，不在Owner内部自动重试。
 */
static void _linkg_network_worker_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_network_worker_t *worker;
    int                     start_error;
    int                     run_error;
    int                     cleanup_error;
    int                     ret;

    worker = user_data;

    if (thread == NULL || worker == NULL)
    {
        return;
    }

    start_error   = 0;
    run_error     = 0;
    cleanup_error = 0;

    /**
     * 上一轮存在未释放资源时，先尝试完成清理。
     * 不允许在旧模块仍已初始化时重复Init。
     */
    if (worker->module_initialized)
    {
        cleanup_error = _linkg_network_worker_cleanup(worker);
        if (cleanup_error != 0)
        {
            goto finish;
        }
    }

    if (!linkg_thread_is_running(thread))
    {
        goto finish;
    }

    start_error = worker->init();
    if (start_error != 0)
    {
        goto cleanup;
    }

    if (!linkg_thread_is_running(thread))
    {
        goto cleanup;
    }

    start_error = worker->start();
    if (start_error != 0)
    {
        goto cleanup;
    }

    /**
     * 当前Owner直接运行模块状态机。
     * Wi-Fi及Cellular内部负责各自的运行维护。
     */
    if (linkg_thread_is_running(thread))
    {
        run_error = worker->run(thread);

        if (linkg_thread_is_running(thread) && run_error == 0)
        {
            run_error = -EIO;
        }
    }

cleanup:
    cleanup_error = _linkg_network_worker_cleanup(worker);

finish:
    if (start_error != 0 && linkg_thread_is_running(thread))
    {
        LINKG_LOG_WARN("network worker startup failed, thread=%s, error=%d", thread->name, start_error);
    }

    if (run_error != 0 && linkg_thread_is_running(thread))
    {
        LINKG_LOG_WARN("network worker runtime failed, thread=%s, error=%d", thread->name, run_error);
    }

    if (cleanup_error != 0)
    {
        LINKG_LOG_WARN("network worker cleanup failed, thread=%s, error=%d", thread->name, cleanup_error);
    }

    /**
     * 保存本轮模块清理结果，供Manager完成join后读取。
     */
    pthread_mutex_lock(&g_network.lock);

    worker->stop_result = cleanup_error;

    pthread_mutex_unlock(&g_network.lock);

    /**
     * 通知Manager回收本轮Owner。
     * Owner不负责join自身，也不创建下一轮线程。
     */
    if (linkg_thread_is_started(&g_network.manager_thread))
    {
        ret = linkg_thread_wakeup(&g_network.manager_thread);
        if (ret != 0)
        {
            LINKG_LOG_WARN("network manager wakeup failed, error=%d", ret);
        }
    }
}

/****************************** Worker生命周期 ******************************/

/**
 * @brief 初始化Worker线程对象及生命周期回调。
 *
 * 不执行模块Init或Start。
 */
static int _linkg_network_worker_init(linkg_network_worker_t *worker, const char *name,
                                      linkg_network_worker_init_func_t init,
                                      linkg_network_worker_start_func_t start,
                                      linkg_network_worker_run_func_t run,
                                      linkg_network_worker_stop_func_t stop,
                                      linkg_network_worker_deinit_func_t deinit)
{
    int ret;

    if (worker == NULL || name == NULL || init == NULL || start == NULL || run == NULL || stop == NULL || deinit == NULL)
    {
        return -EINVAL;
    }

    if (worker->initialized)
    {
        return -EALREADY;
    }

    memset(worker, 0, sizeof(*worker));

    worker->init   = init;
    worker->start  = start;
    worker->run    = run;
    worker->stop   = stop;
    worker->deinit = deinit;

    ret = linkg_thread_init(&worker->thread, name, _linkg_network_worker_thread, worker);
    if (ret != 0)
    {
        memset(worker, 0, sizeof(*worker));
        return ret;
    }

    worker->initialized = true;

    return 0;
}

/**
 * @brief 启动Worker Owner线程。
 *
 * 只创建线程，不等待模块初始化或启动结果。
 */
static int _linkg_network_worker_start(linkg_network_worker_t *worker)
{
    if (worker == NULL || !worker->initialized)
    {
        return -ENODEV;
    }

    if (linkg_thread_is_started(&worker->thread))
    {
        return -EALREADY;
    }

    return linkg_thread_start(&worker->thread);
}

/**
 * @brief 停止并join指定Owner线程。
 *
 * 线程成功回收后返回本轮模块清理结果。
 */
static int _linkg_network_worker_stop(linkg_network_worker_t *worker)
{
    int stop_result;
    int ret;

    if (worker == NULL || !worker->initialized)
    {
        return 0;
    }

    if (!linkg_thread_is_started(&worker->thread))
    {
        return worker->module_initialized ? -EBUSY : 0;
    }

    ret = linkg_thread_stop(&worker->thread);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    stop_result = worker->stop_result;

    pthread_mutex_unlock(&g_network.lock);

    if (stop_result == 0 && worker->module_initialized)
    {
        return -EBUSY;
    }

    return stop_result;
}

/**
 * @brief 销毁已经停止的Worker线程对象。
 */
static int _linkg_network_worker_deinit(linkg_network_worker_t *worker)
{
    if (worker == NULL || !worker->initialized)
    {
        return 0;
    }

    if (linkg_thread_is_started(&worker->thread) || worker->module_initialized)
    {
        return -EBUSY;
    }

    linkg_thread_deinit(&worker->thread);

    memset(worker, 0, sizeof(*worker));

    return 0;
}

/****************************** Worker调度 ******************************/

/**
 * @brief 处理单个Worker的首次启动、退出回收及主动重启。
 *
 * 返回true表示本轮重启请求已经处理；
 * 返回false表示旧Owner尚未回收，需要保留重启请求。
 *
 * 首次启动只尝试一次，失败退出后不自动重新创建Owner。
 */
static bool _linkg_network_manager_process_worker(linkg_network_worker_t *worker, bool enabled, bool restart)
{
    int ret;

    if (worker == NULL || !worker->initialized)
    {
        return true;
    }

    /**
     * 主动重启重新给予本轮一次启动机会。
     */
    if (restart)
    {
        worker->start_attempted = false;
    }

    /**
     * 正常运行中的Owner不处理。
     * 禁用、主动重启或Owner退出时才停止并回收。
     */
    if (linkg_thread_is_started(&worker->thread))
    {
        if (!enabled || restart || !linkg_thread_is_running(&worker->thread))
        {
            ret = _linkg_network_worker_stop(worker);
            if (ret != 0)
            {
                LINKG_LOG_WARN("network worker stop failed, thread=%s, error=%d", worker->thread.name, ret);
            }

            /**
             * 旧Owner尚未join，不允许创建新Owner。
             * 主动重启请求保留至后续管理周期。
             */
            if (linkg_thread_is_started(&worker->thread))
            {
                return false;
            }

            /**
             * 线程已回收但模块清理失败，不自动重试。
             * 后续人工重启时再尝试清理残留资源。
             */
            if (ret != 0)
            {
                worker->start_attempted = true;
                return true;
            }
        }
    }

    /**
     * 禁用、已经尝试启动或仍存在Owner时，不创建线程。
     */
    if (!enabled || worker->start_attempted || linkg_thread_is_started(&worker->thread))
    {
        return true;
    }

    /**
     * 先记录尝试，再创建Owner。
     * 无论线程创建、模块Init或Start是否失败，均不自动重试。
     */
    worker->start_attempted = true;

    ret = _linkg_network_worker_start(worker);
    if (ret != 0)
    {
        LINKG_LOG_WARN("network worker start failed, thread=%s, error=%d", worker->thread.name, ret);
    }

    return true;
}

/****************************** Manager等待 ******************************/

/**
 * @brief 等待Owner退出通知、重启请求或周期检查。
 */
static int _linkg_network_manager_wait(linkg_thread_t *thread)
{
    struct pollfd descriptor;
    int           wakeup_fd;
    int           ret;

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    memset(&descriptor, 0, sizeof(descriptor));

    descriptor.fd     = wakeup_fd;
    descriptor.events = POLLIN;

    do
    {
        ret = poll(&descriptor, 1U, LINKG_NETWORK_MANAGER_POLL_MS);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (!linkg_thread_is_running(thread))
    {
        return 0;
    }

    if (ret < 0)
    {
        return -errno;
    }

    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    if ((descriptor.revents & POLLIN) != 0)
    {
        return linkg_thread_clear_wakeup(thread);
    }

    return 0;
}

/****************************** Manager线程 ******************************/

/**
 * @brief Network长期管理线程。
 *
 * 管理两个Owner的启动、回收及独立重启，不执行模块耗时生命周期。
 */
static void _linkg_network_manager_thread(linkg_thread_t *thread, void *user_data)
{
    bool wifi_enabled;
    bool cellular_enabled;
    bool wifi_restart;
    bool cellular_restart;
    bool processed;
    int  first_error;
    int  ret;

    (void)user_data;

    if (thread == NULL)
    {
        return;
    }

    first_error = 0;

    while (linkg_thread_is_running(thread))
    {
        /**
         * 读取并消费管理请求。
         * 生命周期操作不得持有Network状态锁。
         */
        pthread_mutex_lock(&g_network.lock);

        wifi_enabled     = g_network.wifi_config.enabled;
        cellular_enabled = g_network.cellular_config.enabled;

        wifi_restart     = g_network_manager.wifi_restart_requested;
        cellular_restart = g_network_manager.cellular_restart_requested;

        g_network_manager.wifi_restart_requested     = false;
        g_network_manager.cellular_restart_requested = false;

        pthread_mutex_unlock(&g_network.lock);

        processed = _linkg_network_manager_process_worker(&g_network.wifi_worker, wifi_enabled, wifi_restart);

        /**
         * 旧Owner尚未回收时，保留未完成的Wi-Fi重启请求。
         * 不覆盖处理期间新到达的请求。
         */
        if (!processed && wifi_restart)
        {
            pthread_mutex_lock(&g_network.lock);

            g_network_manager.wifi_restart_requested = true;

            pthread_mutex_unlock(&g_network.lock);
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        processed = _linkg_network_manager_process_worker(&g_network.cellular_worker, cellular_enabled, cellular_restart);

        /**
         * 旧Owner尚未回收时，保留未完成的Cellular重启请求。
         */
        if (!processed && cellular_restart)
        {
            pthread_mutex_lock(&g_network.lock);

            g_network_manager.cellular_restart_requested = true;

            pthread_mutex_unlock(&g_network.lock);
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        ret = _linkg_network_manager_wait(thread);
        if (ret != 0)
        {
            first_error = ret;

            LINKG_LOG_WARN("network manager wait failed, error=%d", ret);

            break;
        }
    }

    /**
     * Manager退出前必须回收两个Owner。
     * 不能在Owner仍访问Network上下文时释放公共资源。
     */
    ret = _linkg_network_worker_stop(&g_network.cellular_worker);
    _linkg_network_manager_record_error(&first_error, ret);

    ret = _linkg_network_worker_stop(&g_network.wifi_worker);
    _linkg_network_manager_record_error(&first_error, ret);

    pthread_mutex_lock(&g_network.lock);

    g_network_manager.shutdown_result = first_error;

    if (first_error != 0 &&
        (g_network.state == LINKG_NETWORK_STATE_RUNNING ||
         g_network.state == LINKG_NETWORK_STATE_STARTING))
    {
        g_network.state = LINKG_NETWORK_STATE_FAILED;
    }

    pthread_mutex_unlock(&g_network.lock);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Network Manager及两个Owner线程对象。
 *
 * 不执行Wi-Fi或Cellular模块的耗时初始化。
 */
int _linkg_network_manager_init(void)
{
    int ret;

    if (g_network.manager_thread.initialized)
    {
        return -EALREADY;
    }

    memset(&g_network_manager, 0, sizeof(g_network_manager));

    ret = _linkg_network_worker_init(&g_network.wifi_worker,
                                     LINKG_NETWORK_WIFI_THREAD_NAME,
                                     _linkg_network_wifi_init,
                                     _linkg_network_wifi_start,
                                     _linkg_network_wifi_run,
                                     _linkg_network_wifi_stop,
                                     _linkg_network_wifi_deinit);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_network_worker_init(&g_network.cellular_worker,
                                     LINKG_NETWORK_CELLULAR_THREAD_NAME,
                                     _linkg_network_cellular_init,
                                     _linkg_network_cellular_start,
                                     _linkg_network_cellular_run,
                                     _linkg_network_cellular_stop,
                                     _linkg_network_cellular_deinit);
    if (ret != 0)
    {
        (void)_linkg_network_worker_deinit(&g_network.wifi_worker);
        return ret;
    }

    ret = linkg_thread_init(&g_network.manager_thread, LINKG_NETWORK_MANAGER_THREAD_NAME, _linkg_network_manager_thread, NULL);
    if (ret != 0)
    {
        (void)_linkg_network_worker_deinit(&g_network.cellular_worker);
        (void)_linkg_network_worker_deinit(&g_network.wifi_worker);
        return ret;
    }

    return 0;
}

/**
 * @brief 启动Network长期管理线程。
 *
 * 不等待Wi-Fi、Cellular初始化或启动。
 */
int _linkg_network_manager_start(void)
{
    int ret;

    if (!g_network.manager_thread.initialized)
    {
        return -ENODEV;
    }

    if (linkg_thread_is_started(&g_network.manager_thread))
    {
        return -EALREADY;
    }

    /**
     * Network重新Start时，两个Owner重新获得首次启动机会。
     */
    pthread_mutex_lock(&g_network.lock);

    memset(&g_network_manager, 0, sizeof(g_network_manager));

    g_network.wifi_worker.start_attempted     = false;
    g_network.cellular_worker.start_attempted = false;

    pthread_mutex_unlock(&g_network.lock);

    ret = linkg_thread_start(&g_network.manager_thread);

    return ret;
}

/**
 * @brief 停止Manager并等待两个Owner完成清理。
 */
int _linkg_network_manager_stop(void)
{
    int ret;

    if (!g_network.manager_thread.initialized)
    {
        return 0;
    }

    if (!linkg_thread_is_started(&g_network.manager_thread))
    {
        return 0;
    }

    ret = linkg_thread_stop(&g_network.manager_thread);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    ret = g_network_manager.shutdown_result;

    pthread_mutex_unlock(&g_network.lock);

    return ret;
}

/**
 * @brief 释放Manager及两个Owner线程对象。
 */
int _linkg_network_manager_deinit(void)
{
    int ret;

    ret = _linkg_network_manager_stop();
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_network_worker_deinit(&g_network.cellular_worker);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_network_worker_deinit(&g_network.wifi_worker);
    if (ret != 0)
    {
        return ret;
    }

    if (g_network.manager_thread.initialized)
    {
        linkg_thread_deinit(&g_network.manager_thread);
    }

    memset(&g_network_manager, 0, sizeof(g_network_manager));

    return 0;
}

/****************************** 独立重启 ******************************/

/**
 * @brief 请求指定模块独立完整重启。
 *
 * 只提交请求并唤醒Manager，不在调用线程执行模块生命周期。
 */
static int _linkg_network_manager_request_restart(bool wifi)
{
    int ret;

    pthread_mutex_lock(&g_network.lock);

    if (g_network.state != LINKG_NETWORK_STATE_RUNNING ||
        !linkg_thread_is_started(&g_network.manager_thread) ||
        !linkg_thread_is_running(&g_network.manager_thread))
    {
        pthread_mutex_unlock(&g_network.lock);
        return -ESHUTDOWN;
    }

    if (wifi)
    {
        g_network_manager.wifi_restart_requested = true;
    }
    else
    {
        g_network_manager.cellular_restart_requested = true;
    }

    pthread_mutex_unlock(&g_network.lock);

    ret = linkg_thread_wakeup(&g_network.manager_thread);

    return ret;
}

/**
 * @brief 请求Wi-Fi独立重启。
 */
int _linkg_network_manager_request_wifi_restart(void)
{
    return _linkg_network_manager_request_restart(true);
}

/**
 * @brief 请求Cellular独立重启。
 */
int _linkg_network_manager_request_cellular_restart(void)
{
    return _linkg_network_manager_request_restart(false);
}
