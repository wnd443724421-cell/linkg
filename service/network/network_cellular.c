/**
 * @file network_cellular.c
 * @brief LinkG网络服务Cellular生命周期实现
 */

#include "network_cellular.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_cellular.h"
#include "linkg_config.h"

#include "network_internal.h"
#include "network_manager.h"
#include "linkg_discovery.h"
#include "linkg_log.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断Cellular模块是否已经初始化。
 */
static bool _linkg_network_cellular_is_initialized(void)
{
    bool initialized;

    pthread_mutex_lock(&g_network.lock);

    initialized = g_network.cellular_worker.module_initialized;

    pthread_mutex_unlock(&g_network.lock);

    return initialized;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Cellular模块。
 *
 * 每轮Owner启动均重新读取Cellular配置，避免完整重启后继续使用旧配置快照。
 * 本函数由Cellular Owner线程调用，不在Network整体初始化阶段执行。
 */
int _linkg_network_cellular_init(void)
{
    linkg_cellular_config_t cellular_config;
    linkg_paths_config_t    paths_config;
    linkg_packet_pool_t    *packet_pool;
    int                     ret;

    if (_linkg_network_cellular_is_initialized())
    {
        return -EALREADY;
    }

    memset(&cellular_config, 0, sizeof(cellular_config));
    memset(&paths_config, 0, sizeof(paths_config));

    ret = linkg_config_get_cellular(&cellular_config);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_config_get_paths(&paths_config);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    packet_pool = g_network.packet_pool;

    pthread_mutex_unlock(&g_network.lock);

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    ret = linkg_cellular_init(&cellular_config, paths_config.cellular.enabled, packet_pool);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.cellular_config                    = cellular_config;
    g_network.cellular_worker.module_initialized = true;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}

/**
 * @brief 启动Cellular模块并通知设备发现。
 */
int _linkg_network_cellular_start(void)
{
    int ret;

    if (!_linkg_network_cellular_is_initialized())
    {
        return -ENODEV;
    }

    ret = linkg_cellular_start();
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_notify_cellular_network_changed();
    if (ret != 0)
    {
        LINKG_LOG_WARN("notify Cellular Discovery failed, error=%d", ret);
    }

    return 0;
}

/**
 * @brief 运行Cellular Owner控制循环。
 *
 * 本函数在Cellular Owner线程中执行，退出后由Worker统一执行停止和反初始化。
 */
int _linkg_network_cellular_run(linkg_thread_t *owner_thread)
{
    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_network_cellular_is_initialized())
    {
        return -ENODEV;
    }

    return linkg_cellular_run(owner_thread);
}

/**
 * @brief 停止Cellular模块运行资源。
 *
 * 停止成功后模块仍处于已初始化状态，后续由deinit释放初始化资源。
 */
int _linkg_network_cellular_stop(void)
{
    if (!_linkg_network_cellular_is_initialized())
    {
        return 0;
    }

    return linkg_cellular_stop();
}

/**
 * @brief 反初始化Cellular模块。
 *
 * 仅在实际释放成功后清除模块初始化标志，失败时保留状态供后续清理。
 */
int _linkg_network_cellular_deinit(void)
{
    int ret;

    if (!_linkg_network_cellular_is_initialized())
    {
        return 0;
    }

    ret = linkg_cellular_deinit();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.cellular_worker.module_initialized = false;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}

/****************************** 独立重启 ******************************/

/**
 * @brief 请求Cellular独立完整重启。
 *
 * 只提交请求并唤醒Network Manager，不在调用线程中执行耗时生命周期操作。
 */
int _linkg_network_cellular_restart(void)
{
    return _linkg_network_manager_request_cellular_restart();
}
