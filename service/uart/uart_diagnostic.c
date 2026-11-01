/**
 * @file uart_diagnostic.c
 * @brief UART硬件诊断任务、资源接管与业务恢复
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-31
 */

#include "linkg_uart_diagnostic.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_gpio_manager.h"
#include "linkg_log.h"
#include "linkg_thread.h"
#include "linkg_time.h"
#include "linkg_uart_manager.h"

#include "uart_diagnostic_internal.h"
#include "uart_internal.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t                    lifecycle_lock;  // 串行化线程生命周期
    pthread_mutex_t                    lock;            // 保护诊断任务和结果
    linkg_thread_t                     worker;          // 常驻诊断线程，空闲时阻塞在eventfd
    bool                               initialized;     // 诊断模块是否初始化
    bool                               started;         // 是否接受新的诊断任务
    bool                               stopping;        // 正在停止，拒绝重新启动和提交
    bool                               busy;            // 是否存在运行任务
    bool                               cancel;          // 任务取消请求
    uint32_t                           next_job_id;     // 下一个任务编号
    linkg_uart_diagnostic_params_t     params;          // 当前任务参数
    linkg_uart_diagnostic_status_t     status;          // 当前任务和最终结果
} uart_diagnostic_context_t;

/****************************** 全局上下文 ******************************/

static uart_diagnostic_context_t g_uart_diagnostic =
{
    .lifecycle_lock = PTHREAD_MUTEX_INITIALIZER, // 线程生命周期锁
    .lock = PTHREAD_MUTEX_INITIALIZER            // 诊断任务锁
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 读取UART Manager当前生命周期状态。
 */
static linkg_uart_life_t _uart_diagnostic_manager_state(void)
{
    linkg_uart_life_t state;

    pthread_mutex_lock(&g_uart_manager.life_lock);
    state = g_uart_manager.state;
    pthread_mutex_unlock(&g_uart_manager.life_lock);

    return state;
}

/**
 * @brief 更新诊断任务状态。
 */
static void _uart_diagnostic_set_state(linkg_uart_diagnostic_state_t state)
{
    pthread_mutex_lock(&g_uart_diagnostic.lock);
    g_uart_diagnostic.status.state = state;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);
}

/**
 * @brief 将尚未执行的测试项目标记为跳过。
 */
static void _uart_diagnostic_skip_remaining(uint32_t start_index)
{
    uint32_t index;

    pthread_mutex_lock(&g_uart_diagnostic.lock);

    for (index = start_index; index < g_uart_diagnostic.status.test_count; index++)
    {
        if (g_uart_diagnostic.status.tests[index].state == LINKG_UART_DIAGNOSTIC_TEST_NOT_RUN)
        {
            g_uart_diagnostic.status.tests[index].state = LINKG_UART_DIAGNOSTIC_TEST_SKIPPED;
        }
    }

    pthread_mutex_unlock(&g_uart_diagnostic.lock);
}

/**
 * @brief 根据模式构造测试项目，UART0单测包含普通路径和SBUS路径。
 */
static uint32_t _uart_diagnostic_build_plan(const linkg_uart_diagnostic_params_t *params, linkg_uart_diagnostic_test_result_t *tests)
{
    uint32_t count;

    memset(tests, 0, sizeof(linkg_uart_diagnostic_test_result_t) * LINKG_UART_DIAGNOSTIC_MAX_TESTS);
    count = 0U;

    if (params->mode == LINKG_UART_DIAGNOSTIC_MODE_ALL)
    {
        tests[count].uart_num = 0U;
        tests[count++].path = LINKG_UART_DIAGNOSTIC_PATH_NORMAL;
        tests[count].uart_num = 1U;
        tests[count++].path = LINKG_UART_DIAGNOSTIC_PATH_NORMAL;
        tests[count].uart_num = 2U;
        tests[count++].path = LINKG_UART_DIAGNOSTIC_PATH_NORMAL;
        tests[count].uart_num = 0U;
        tests[count++].path = LINKG_UART_DIAGNOSTIC_PATH_SBUS;
    }
    else
    {
        tests[count].uart_num = params->uart_num;
        tests[count++].path = LINKG_UART_DIAGNOSTIC_PATH_NORMAL;

        if (params->uart_num == 0U)
        {
            tests[count].uart_num = 0U;
            tests[count++].path = LINKG_UART_DIAGNOSTIC_PATH_SBUS;
        }
    }

    return count;
}

/**
 * @brief 查询当前任务是否已经收到取消请求。
 */
bool uart_diagnostic_cancel_requested(void)
{
    bool cancel;

    pthread_mutex_lock(&g_uart_diagnostic.lock);
    cancel = g_uart_diagnostic.cancel;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    return cancel;
}

/**
 * @brief 将单项测试的最新结果复制到任务快照。
 */
void uart_diagnostic_result_update(uint32_t index, const linkg_uart_diagnostic_test_result_t *result)
{
    if (result == NULL || index >= LINKG_UART_DIAGNOSTIC_MAX_TESTS)
    {
        return;
    }

    pthread_mutex_lock(&g_uart_diagnostic.lock);
    if (index < g_uart_diagnostic.status.test_count)
    {
        g_uart_diagnostic.status.tests[index] = *result;
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lock);
}

/**
 * @brief 恢复PD16和UART Manager原有生命周期，不操作由UART驱动管理的UART_EN#。
 */
static int _uart_diagnostic_restore(linkg_uart_life_t initial_state)
{
    linkg_uart_life_t state;
    int               first_error;
    int               ret;

    first_error = linkg_gpio_manager_set_sbus_enabled(false);
    (void)linkg_time_sleep_ms(5U);

    state = _uart_diagnostic_manager_state();
    if (state == LINKG_UART_LIFE_STOPPING)
    {
        ret = uart_manager_stop();
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }
            return first_error;
        }

        state = _uart_diagnostic_manager_state();
    }

    if (state == LINKG_UART_LIFE_RUNNING && initial_state != LINKG_UART_LIFE_RUNNING)
    {
        ret = uart_manager_stop();
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }
            return first_error;
        }

        state = _uart_diagnostic_manager_state();
    }

    if (initial_state == LINKG_UART_LIFE_RUNNING && state == LINKG_UART_LIFE_STOPPED)
    {
        ret = uart_manager_start(false);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }

        state = _uart_diagnostic_manager_state();
    }

    if (state != initial_state && first_error == 0)
    {
        first_error = -EIO;
    }

    return first_error;
}

/**
 * @brief 按测试路径切换PD16并等待MCU完成模式采样。
 */
static int _uart_diagnostic_set_path(linkg_uart_diagnostic_path_t path)
{
    bool enabled;
    int  ret;

    enabled = path == LINKG_UART_DIAGNOSTIC_PATH_SBUS;
    ret = linkg_gpio_manager_set_sbus_enabled(enabled);
    if (ret != 0)
    {
        return ret;
    }

    return linkg_time_sleep_ms(5U);
}

/**
 * @brief 执行诊断任务，测试结束后无条件尝试恢复原UART生命周期。
 */
static void _uart_diagnostic_process_job(void)
{
    linkg_uart_diagnostic_test_result_t plan[LINKG_UART_DIAGNOSTIC_MAX_TESTS];
    linkg_uart_diagnostic_test_result_t result;
    linkg_uart_life_t                   initial_state;
    uint64_t                            started_ms;
    uint32_t                            test_count;
    uint32_t                            index;
    bool                                was_initialized;
    bool                                was_running;
    bool                                test_started;
    bool                                cancelled;
    bool                                transaction_locked;
    bool                                tests_passed;
    int                                 first_error;
    int                                 restore_error;
    int                                 ret;

    started_ms     = linkg_time_elapsed_ms();
    first_error    = 0;
    restore_error  = 0;
    test_started       = false;
    cancelled          = false;
    transaction_locked = false;
    tests_passed       = false;

    // 即使在准备阶段取消或事务申请失败，也要保留真实的诊断前状态。
    initial_state   = _uart_diagnostic_manager_state();
    was_initialized = initial_state != LINKG_UART_LIFE_UNINITIALIZED;
    was_running     = initial_state == LINKG_UART_LIFE_RUNNING;
    pthread_mutex_lock(&g_uart_diagnostic.lock);
    g_uart_diagnostic.status.uart_was_initialized = was_initialized;
    g_uart_diagnostic.status.uart_was_running = was_running;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    if (uart_diagnostic_cancel_requested())
    {
        cancelled = true;
        goto finalize_status;
    }

    ret = linkg_config_transaction_begin();
    if (ret != 0)
    {
        first_error = ret;
        goto finalize_status;
    }
    transaction_locked = true;

    pthread_mutex_lock(&g_uart_diagnostic.lock);
    test_count = _uart_diagnostic_build_plan(&g_uart_diagnostic.params, plan);
    g_uart_diagnostic.status.test_count = test_count;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    if (initial_state == LINKG_UART_LIFE_STOPPING)
    {
        first_error   = -EBUSY;
        restore_error = -EBUSY;
        goto finalize_status;
    }

    if (was_running)
    {
        ret = uart_manager_stop();
        if (ret != 0)
        {
            first_error = ret;
            goto restoring;
        }
    }

    test_started = true;
    _uart_diagnostic_set_state(LINKG_UART_DIAGNOSTIC_STATE_RUNNING);

    for (index = 0U; index < test_count; index++)
    {
        if (uart_diagnostic_cancel_requested())
        {
            cancelled = true;
            _uart_diagnostic_skip_remaining(index);
            break;
        }

        pthread_mutex_lock(&g_uart_diagnostic.lock);
        g_uart_diagnostic.status.current_test = index;
        g_uart_diagnostic.status.tests[index].state = LINKG_UART_DIAGNOSTIC_TEST_RUNNING;
        pthread_mutex_unlock(&g_uart_diagnostic.lock);

        memset(&result, 0, sizeof(result));
        result.uart_num = plan[index].uart_num;
        result.path     = plan[index].path;
        result.state    = LINKG_UART_DIAGNOSTIC_TEST_RUNNING;
        result.first_error_offset = UINT32_MAX;

        ret = _uart_diagnostic_set_path(result.path);
        if (ret == 0)
        {
            ret = uart_diagnostic_test_port(result.uart_num, result.path, &result);
        }
        else
        {
            result.error = ret;
            result.state = LINKG_UART_DIAGNOSTIC_TEST_FAILED;
        }

        uart_diagnostic_result_update(index, &result);

        if (ret == -ECANCELED)
        {
            cancelled = true;
            _uart_diagnostic_skip_remaining(index + 1U);
            break;
        }
    }

restoring:
    _uart_diagnostic_set_state(LINKG_UART_DIAGNOSTIC_STATE_RESTORING);

    ret = _uart_diagnostic_restore(initial_state);
    if (ret != 0 && restore_error == 0)
    {
        restore_error = ret;
    }

finalize_status:
    // 终态必须表示独占权已经释放；否则GET终态后立即START仍可能返回EBUSY。
    _uart_diagnostic_set_state(LINKG_UART_DIAGNOSTIC_STATE_RESTORING);
    if (transaction_locked)
    {
        linkg_config_transaction_end();
    }
    uart_manager_diagnostic_release();

    pthread_mutex_lock(&g_uart_diagnostic.lock);
    tests_passed = g_uart_diagnostic.status.test_count > 0U;
    for (index = 0U; index < g_uart_diagnostic.status.test_count; index++)
    {
        if (g_uart_diagnostic.status.tests[index].state == LINKG_UART_DIAGNOSTIC_TEST_NOT_RUN)
        {
            g_uart_diagnostic.status.tests[index].state = LINKG_UART_DIAGNOSTIC_TEST_SKIPPED;
        }
        if (g_uart_diagnostic.status.tests[index].state != LINKG_UART_DIAGNOSTIC_TEST_PASSED)
        {
            tests_passed = false;
        }
    }

    g_uart_diagnostic.status.uart_restored = restore_error == 0;
    g_uart_diagnostic.status.tests_passed = tests_passed;
    g_uart_diagnostic.status.error = first_error;
    g_uart_diagnostic.status.restore_error = restore_error;
    g_uart_diagnostic.status.elapsed_ms = linkg_time_elapsed_ms() - started_ms;

    if (restore_error != 0 || first_error != 0)
    {
        g_uart_diagnostic.status.state = LINKG_UART_DIAGNOSTIC_STATE_FAILED;
    }
    else if (cancelled || g_uart_diagnostic.cancel)
    {
        g_uart_diagnostic.status.state = LINKG_UART_DIAGNOSTIC_STATE_CANCELLED;
    }
    else if (!test_started)
    {
        g_uart_diagnostic.status.state = LINKG_UART_DIAGNOSTIC_STATE_FAILED;
    }
    else
    {
        g_uart_diagnostic.status.state = LINKG_UART_DIAGNOSTIC_STATE_COMPLETED;
    }

    g_uart_diagnostic.busy = false;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);
}

/**
 * @brief 常驻线程复用更新模块的eventfd模式，空闲时无限期阻塞。
 * 停止时仍处理已接受任务的取消和资源恢复，再退出线程。
 */
static void _uart_diagnostic_worker(linkg_thread_t *thread, void *user_data)
{
    struct pollfd descriptor;
    bool          pending;
    int           ret;

    (void)user_data;
    descriptor.fd = linkg_thread_get_wakeup_fd(thread);
    descriptor.events = POLLIN;

    while (true)
    {
        pthread_mutex_lock(&g_uart_diagnostic.lock);
        pending = g_uart_diagnostic.busy &&
                  g_uart_diagnostic.status.state == LINKG_UART_DIAGNOSTIC_STATE_STARTING;
        pthread_mutex_unlock(&g_uart_diagnostic.lock);

        if (pending)
        {
            _uart_diagnostic_process_job();
            continue;
        }
        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        do
        {
            descriptor.revents = 0;
            ret = poll(&descriptor, 1U, -1);
        }
        while (ret < 0 && errno == EINTR);

        if (ret > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) == 0 &&
            (descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret == 0)
            {
                continue;
            }
        }
        else if (ret < 0)
        {
            ret = -errno;
        }
        else
        {
            ret = -EIO;
        }

        LINKG_LOG_ERROR("UART diagnostic: worker wait failed, error=%d", ret);
        pthread_mutex_lock(&g_uart_diagnostic.lock);
        g_uart_diagnostic.started = false;
        // 查询与提交可能刚好交错；唤醒故障时也必须释放已接受但尚未执行的任务。
        if (g_uart_diagnostic.busy)
        {
            uart_manager_diagnostic_release();
            for (uint32_t index = 0U; index < g_uart_diagnostic.status.test_count; index++)
            {
                g_uart_diagnostic.status.tests[index].state = LINKG_UART_DIAGNOSTIC_TEST_SKIPPED;
            }
            g_uart_diagnostic.status.error = ret;
            g_uart_diagnostic.status.uart_restored = true;
            g_uart_diagnostic.status.state = LINKG_UART_DIAGNOSTIC_STATE_FAILED;
            g_uart_diagnostic.busy = false;
        }
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        break;
    }
}

/**
 * @brief 关闭提交入口，取消当前任务并等待线程退出；调用方持有生命周期锁。
 */
static int _uart_diagnostic_stop_worker(void)
{
    int ret;

    pthread_mutex_lock(&g_uart_diagnostic.lock);
    if (!g_uart_diagnostic.initialized)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return 0;
    }
    g_uart_diagnostic.started = false;
    g_uart_diagnostic.stopping = true;
    if (g_uart_diagnostic.busy)
    {
        g_uart_diagnostic.cancel = true;
        g_uart_diagnostic.status.cancel_requested = true;
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    // 不持有任务锁等待join，Worker仍需持锁发布取消和恢复结果。
    ret = linkg_thread_stop(&g_uart_diagnostic.worker);
    pthread_mutex_lock(&g_uart_diagnostic.lock);
    if (ret == 0)
    {
        g_uart_diagnostic.stopping = false;
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lock);
    return ret;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化诊断模块和通用线程对象，暂不启动线程。
 */
int linkg_uart_diagnostic_init(void)
{
    int ret;

    pthread_mutex_lock(&g_uart_diagnostic.lifecycle_lock);
    pthread_mutex_lock(&g_uart_diagnostic.lock);
    if (g_uart_diagnostic.initialized)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        pthread_mutex_unlock(&g_uart_diagnostic.lifecycle_lock);
        return -EALREADY;
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    ret = linkg_thread_init(&g_uart_diagnostic.worker, "linkg_uart_diag", _uart_diagnostic_worker, NULL);
    if (ret == 0)
    {
        pthread_mutex_lock(&g_uart_diagnostic.lock);
        memset(&g_uart_diagnostic.status, 0, sizeof(g_uart_diagnostic.status));
        g_uart_diagnostic.initialized = true;
        g_uart_diagnostic.started = false;
        g_uart_diagnostic.stopping = false;
        g_uart_diagnostic.busy = false;
        g_uart_diagnostic.cancel = false;
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lifecycle_lock);
    return ret;
}

/**
 * @brief 启动常驻诊断线程，开始接受Web任务。
 */
int linkg_uart_diagnostic_start(void)
{
    int ret;

    pthread_mutex_lock(&g_uart_diagnostic.lifecycle_lock);
    pthread_mutex_lock(&g_uart_diagnostic.lock);
    if (!g_uart_diagnostic.initialized)
    {
        ret = -EINVAL;
    }
    else if (g_uart_diagnostic.stopping)
    {
        ret = -EBUSY;
    }
    else if (g_uart_diagnostic.started)
    {
        ret = -EALREADY;
    }
    else
    {
        ret = linkg_thread_start(&g_uart_diagnostic.worker);
        if (ret == 0)
        {
            g_uart_diagnostic.started = true;
        }
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lock);
    pthread_mutex_unlock(&g_uart_diagnostic.lifecycle_lock);
    return ret;
}

/**
 * @brief 取消并恢复当前任务后停止常驻线程，允许随后再次start。
 */
int linkg_uart_diagnostic_stop(void)
{
    int ret;

    pthread_mutex_lock(&g_uart_diagnostic.lifecycle_lock);
    ret = _uart_diagnostic_stop_worker();
    pthread_mutex_unlock(&g_uart_diagnostic.lifecycle_lock);
    return ret;
}

/**
 * @brief 停止常驻线程并释放线程对象；停止失败时保留所有资源供重试。
 */
int linkg_uart_diagnostic_deinit(void)
{
    int ret;

    pthread_mutex_lock(&g_uart_diagnostic.lifecycle_lock);
    ret = _uart_diagnostic_stop_worker();
    if (ret == 0)
    {
        pthread_mutex_lock(&g_uart_diagnostic.lock);
        if (g_uart_diagnostic.initialized)
        {
            linkg_thread_deinit(&g_uart_diagnostic.worker);
            g_uart_diagnostic.initialized = false;
            g_uart_diagnostic.started = false;
            g_uart_diagnostic.stopping = false;
            g_uart_diagnostic.busy = false;
            g_uart_diagnostic.cancel = false;
            memset(&g_uart_diagnostic.status, 0, sizeof(g_uart_diagnostic.status));
        }
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
    }
    pthread_mutex_unlock(&g_uart_diagnostic.lifecycle_lock);
    return ret;
}

/****************************** 任务控制 ******************************/

/**
 * @brief 接受单串口或全部串口诊断任务并异步执行。
 */
int linkg_uart_diagnostic_submit(const linkg_uart_diagnostic_params_t *params, uint32_t *job_id)
{
    linkg_uart_diagnostic_test_result_t plan[LINKG_UART_DIAGNOSTIC_MAX_TESTS];
    linkg_uart_diagnostic_status_t     previous_status;
    uint32_t                            test_count;
    int                                 ret;

    if (params == NULL || job_id == NULL)
    {
        return -EINVAL;
    }

    if (params->mode != LINKG_UART_DIAGNOSTIC_MODE_SINGLE &&
        params->mode != LINKG_UART_DIAGNOSTIC_MODE_ALL)
    {
        return -EINVAL;
    }

    if (params->mode == LINKG_UART_DIAGNOSTIC_MODE_SINGLE &&
        params->uart_num >= LINKG_UART_DIAGNOSTIC_UART_COUNT)
    {
        return -EINVAL;
    }

    if (params->mode == LINKG_UART_DIAGNOSTIC_MODE_ALL && params->uart_num != 0U)
    {
        return -EINVAL;
    }

    *job_id = 0U;
    pthread_mutex_lock(&g_uart_diagnostic.lock);

    if (!g_uart_diagnostic.initialized)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return -ENODEV;
    }

    if (!g_uart_diagnostic.started || g_uart_diagnostic.stopping ||
        !linkg_thread_is_running(&g_uart_diagnostic.worker))
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return -ESHUTDOWN;
    }

    if (g_uart_diagnostic.busy)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return -EBUSY;
    }

    ret = uart_manager_diagnostic_acquire();
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return ret;
    }

    test_count = _uart_diagnostic_build_plan(params, plan);

    previous_status = g_uart_diagnostic.status;
    memset(&g_uart_diagnostic.status, 0, sizeof(g_uart_diagnostic.status));
    g_uart_diagnostic.next_job_id++;
    if (g_uart_diagnostic.next_job_id == 0U)
    {
        g_uart_diagnostic.next_job_id = 1U;
    }

    g_uart_diagnostic.status.job_id        = g_uart_diagnostic.next_job_id;
    g_uart_diagnostic.status.mode          = params->mode;
    g_uart_diagnostic.status.state         = LINKG_UART_DIAGNOSTIC_STATE_STARTING;
    g_uart_diagnostic.status.selected_uart = params->uart_num;
    g_uart_diagnostic.status.test_count    = test_count;

    memcpy(g_uart_diagnostic.status.tests, plan, sizeof(plan));
    for (uint32_t index = 0U; index < test_count; index++)
    {
        g_uart_diagnostic.status.tests[index].state = LINKG_UART_DIAGNOSTIC_TEST_NOT_RUN;
        g_uart_diagnostic.status.tests[index].first_error_offset = UINT32_MAX;
    }

    g_uart_diagnostic.params = *params;
    g_uart_diagnostic.cancel = false;
    g_uart_diagnostic.busy   = true;

    ret = linkg_thread_wakeup(&g_uart_diagnostic.worker);
    if (ret != 0)
    {
        uart_manager_diagnostic_release();
        g_uart_diagnostic.status = previous_status;
        g_uart_diagnostic.cancel = previous_status.cancel_requested;
        g_uart_diagnostic.busy = false;
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return ret;
    }

    *job_id = g_uart_diagnostic.status.job_id;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    return 0;
}

/**
 * @brief 请求取消指定任务，job_id为0时取消当前任务。
 */
int linkg_uart_diagnostic_cancel(uint32_t job_id)
{
    pthread_mutex_lock(&g_uart_diagnostic.lock);

    if (!g_uart_diagnostic.busy)
    {
        if (job_id == 0U)
        {
            pthread_mutex_unlock(&g_uart_diagnostic.lock);
            return 0;
        }

        if (job_id != g_uart_diagnostic.status.job_id)
        {
            pthread_mutex_unlock(&g_uart_diagnostic.lock);
            return -ENOENT;
        }

        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return -EALREADY;
    }

    if (job_id != 0U && job_id != g_uart_diagnostic.status.job_id)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return -ENOENT;
    }

    g_uart_diagnostic.cancel = true;
    g_uart_diagnostic.status.cancel_requested = true;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    return 0;
}

/**
 * @brief 获取最后一次诊断结果，查询不会清除结果。
 */
int linkg_uart_diagnostic_get_status(uint32_t job_id, linkg_uart_diagnostic_status_t *out)
{
    if (out == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_uart_diagnostic.lock);

    if (job_id != 0U && job_id != g_uart_diagnostic.status.job_id)
    {
        pthread_mutex_unlock(&g_uart_diagnostic.lock);
        return -ENOENT;
    }

    *out = g_uart_diagnostic.status;
    pthread_mutex_unlock(&g_uart_diagnostic.lock);

    return 0;
}
