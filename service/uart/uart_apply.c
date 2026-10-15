/**
 * @file uart_apply.c
 * @brief UART配置异步事务、生命周期串行化及失败回滚
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include "uart_internal.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <pthread.h>
#include <string.h>

#include "linkg_config.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    pthread_mutex_t           lock;      // 保护任务和公共生命周期入口，不在后台应用期间持有
    pthread_cond_t            idle;      // 通知生命周期调用方应用已经结束
    bool                      busy;      // 至多一个应用任务，完成前拒绝重复SET
    linkg_uart_config_t       candidate; // 后台线程持有的候选配置副本
    linkg_uart_apply_status_t status;    // 最后一次应用结果，查询时复制
} uart_apply_context_t;

/****************************** 全局上下文 ******************************/

static uart_apply_context_t g_uart_apply =
{
    .lock = PTHREAD_MUTEX_INITIALIZER, // 应用任务互斥锁
    .idle = PTHREAD_COND_INITIALIZER   // 生命周期等待条件
};

/****************************** 生命周期 ******************************/

/**
 * @brief 等待已接受的应用结束，返回时持有入口锁，防止生命周期与重载交错。
 */
static void _uart_apply_enter(void)
{
    pthread_mutex_lock(&g_uart_apply.lock);
    while (g_uart_apply.busy)
    {
        pthread_cond_wait(&g_uart_apply.idle, &g_uart_apply.lock);
    }
}

/**
 * @brief 初始化UART服务，与异步重载串行执行。
 */
int linkg_uart_manager_init(void)
{
    int ret;

    _uart_apply_enter();
    ret = uart_manager_init();
    pthread_mutex_unlock(&g_uart_apply.lock);

    return ret;
}

/**
 * @brief 启动UART服务，开机保留设备和网络异步恢复行为。
 */
int linkg_uart_manager_start(void)
{
    int ret;

    _uart_apply_enter();
    ret = uart_manager_start(false);
    pthread_mutex_unlock(&g_uart_apply.lock);

    return ret;
}

/**
 * @brief 等待正在进行的事务结束后停止UART，不释放未退出线程引用的资源。
 */
int linkg_uart_manager_stop(void)
{
    int ret;

    _uart_apply_enter();
    ret = uart_manager_stop();
    pthread_mutex_unlock(&g_uart_apply.lock);

    return ret;
}

/**
 * @brief 在已停止状态释放UART对象，与重载互斥。
 */
int linkg_uart_manager_deinit(void)
{
    int ret;

    _uart_apply_enter();
    ret = uart_manager_deinit();
    pthread_mutex_unlock(&g_uart_apply.lock);

    return ret;
}

/****************************** 配置校验 ******************************/

/**
 * @brief 拒绝当前接口的定向广播地址，实际监听占用由启动时bind确认。
 */
static int _uart_apply_validate_addresses(const linkg_uart_config_t *config)
{
    struct ifaddrs *interfaces;
    struct ifaddrs *item;
    uint32_t        port;
    uint32_t        route;
    int             ret;

    if (getifaddrs(&interfaces) != 0)
    {
        return -errno;
    }

    ret = 0;
    for (port = 0U; port < config->count; port++)
    {
        for (route = 0U; route < config->items[port].route_count; route++)
        {
            for (item = interfaces; item != NULL; item = item->ifa_next)
            {
                if ((item->ifa_flags & IFF_BROADCAST) != 0 && item->ifa_broadaddr != NULL &&
                    item->ifa_broadaddr->sa_family == AF_INET &&
                    ((struct sockaddr_in *)item->ifa_broadaddr)->sin_addr.s_addr == config->items[port].routes[route].ip.s_addr)
                {
                    ret = -EINVAL;
                }
            }
        }
    }

    freeifaddrs(interfaces);

    return ret;
}

/****************************** 应用事务 ******************************/

/**
 * @brief 恢复磁盘、内存及旧UART运行态；停止失败时保留全部对象供诊断或重试。
 */
static int _uart_apply_restore(const linkg_config_t *old, bool was_running, bool save_attempted)
{
    int first_error;
    int ret;

    first_error = uart_manager_stop();
    if (first_error == 0)
    {
        first_error = uart_manager_deinit();
    }

    ret = linkg_config_replace(old);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    // 原子写入可能在rename后因目录fsync失败；必须再次保存旧配置。
    if (save_attempted)
    {
        ret = linkg_config_save(old);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (g_uart_manager.state == LINKG_UART_LIFE_UNINITIALIZED)
    {
        ret = uart_manager_init();
        if (ret == 0 && was_running)
        {
            ret = uart_manager_start(false);
        }

        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/**
 * @brief 仅重建UART服务，文件持久化成功后才完成应用结果。
 *
 * busy排斥公共生命周期调用；配置事务锁排斥其他Web配置写入。
 * 不持有任务锁等待线程、设备或文件IO，状态查询和其他Web读取保持可用。
 */
static void *_uart_apply_thread(void *argument)
{
    linkg_config_t           old;
    linkg_config_t           next;
    linkg_uart_apply_state_t state;
    uint64_t                 started_ms;
    bool                     was_running;
    bool                     save_attempted;
    int                      restore_ret;
    int                      ret;

    (void)argument;
    started_ms     = uart_now_ms();
    state          = LINKG_UART_APPLY_FAILED;
    restore_ret    = 0;
    save_attempted = false;

    ret = linkg_config_transaction_begin();
    if (ret != 0)
    {
        goto finished;
    }

    ret = linkg_config_create_snapshot(&old);
    if (ret != 0)
    {
        goto unlock;
    }

    if (linkg_uart_config_equal(&old.uart, &g_uart_apply.candidate))
    {
        state = LINKG_UART_APPLY_NONE;
        goto unlock;
    }

    next      = old;
    next.uart = g_uart_apply.candidate;

    was_running = g_uart_manager.state == LINKG_UART_LIFE_RUNNING;
    ret         = uart_manager_stop();
    if (ret != 0)
    {
        goto rollback;
    }

    ret = uart_manager_deinit();
    if (ret == 0)
    {
        ret = linkg_config_replace(&next);
    }
    if (ret == 0)
    {
        ret = uart_manager_init();
    }
    if (ret == 0)
    {
        ret = uart_manager_start(true);
    }
    if (ret == 0 && !was_running)
    {
        ret = uart_manager_stop();
    }
    if (ret != 0)
    {
        goto rollback;
    }

    save_attempted = true;
    ret            = linkg_config_save(&next);
    if (ret != 0)
    {
        goto rollback;
    }

    state = LINKG_UART_APPLY_APPLIED;
    goto unlock;

rollback:
    restore_ret = _uart_apply_restore(&old, was_running, save_attempted);
    state       = restore_ret == 0 ? LINKG_UART_APPLY_ROLLED_BACK : LINKG_UART_APPLY_FAILED;

unlock:
    linkg_config_transaction_end();

finished:
    pthread_mutex_lock(&g_uart_apply.lock);
    g_uart_apply.status.state          = state;
    g_uart_apply.status.error          = ret;
    g_uart_apply.status.rollback_error = restore_ret;
    g_uart_apply.status.elapsed_ms     = uart_now_ms() - started_ms;
    g_uart_apply.busy                  = false;
    pthread_cond_broadcast(&g_uart_apply.idle);
    pthread_mutex_unlock(&g_uart_apply.lock);

    return NULL;
}

/**
 * @brief 接受完整UART快照，重复提交返回忙，相同配置直接返回无需应用。
 */
int linkg_uart_manager_apply(const linkg_uart_config_t *config, uint32_t *job_id)
{
    linkg_uart_config_t current;
    pthread_attr_t      attributes;
    pthread_t           thread;
    int                 ret;

    if (config == NULL || job_id == NULL || linkg_uart_config_validate(config) != 0)
    {
        return -EINVAL;
    }

    ret = _uart_apply_validate_addresses(config);
    if (ret != 0)
    {
        return ret;
    }

    *job_id = 0U;
    pthread_mutex_lock(&g_uart_apply.lock);
    if (g_uart_apply.busy)
    {
        pthread_mutex_unlock(&g_uart_apply.lock);
        return -EBUSY;
    }

    if (g_uart_manager.state == LINKG_UART_LIFE_UNINITIALIZED || g_uart_manager.state == LINKG_UART_LIFE_STOPPING)
    {
        pthread_mutex_unlock(&g_uart_apply.lock);
        return -ENODEV;
    }

    ret = linkg_config_get_uart(&current);
    if (ret != 0 || linkg_uart_config_equal(&current, config))
    {
        pthread_mutex_unlock(&g_uart_apply.lock);
        return ret;
    }

    ret = pthread_attr_init(&attributes);
    if (ret == 0)
    {
        ret = pthread_attr_setdetachstate(&attributes, PTHREAD_CREATE_DETACHED);
        if (ret != 0)
        {
            pthread_attr_destroy(&attributes);
        }
    }
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_uart_apply.lock);
        return -ret;
    }

    g_uart_apply.candidate             = *config;
    g_uart_apply.busy                  = true;
    g_uart_apply.status.state          = LINKG_UART_APPLY_RUNNING;
    g_uart_apply.status.error          = 0;
    g_uart_apply.status.rollback_error = 0;
    g_uart_apply.status.elapsed_ms     = 0U;
    g_uart_apply.status.job_id++;
    if (g_uart_apply.status.job_id == 0U)
    {
        g_uart_apply.status.job_id = 1U;
    }

    ret = pthread_create(&thread, &attributes, _uart_apply_thread, NULL);
    pthread_attr_destroy(&attributes);
    if (ret != 0)
    {
        g_uart_apply.busy         = false;
        g_uart_apply.status.state = LINKG_UART_APPLY_FAILED;
        g_uart_apply.status.error = -ret;
    }
    else
    {
        *job_id = g_uart_apply.status.job_id;
    }
    pthread_mutex_unlock(&g_uart_apply.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取最后一次应用结果，不等待后台任务完成。
 */
int linkg_uart_manager_get_apply_status(linkg_uart_apply_status_t *out)
{
    if (out == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_uart_apply.lock);
    *out = g_uart_apply.status;
    pthread_mutex_unlock(&g_uart_apply.lock);

    return 0;
}

/**
 * @brief 读取提交完成的UART配置，应用中不把候选配置当成持久化结果返回。
 */
int linkg_uart_manager_get_config(linkg_uart_config_t *out)
{
    int ret;

    pthread_mutex_lock(&g_uart_apply.lock);
    if (g_uart_apply.busy)
    {
        ret = -EBUSY;
    }
    else if (g_uart_apply.status.rollback_error != 0)
    {
        ret = -EIO;
    }
    else
    {
        ret = linkg_config_get_uart(out);
    }
    pthread_mutex_unlock(&g_uart_apply.lock);

    return ret;
}
