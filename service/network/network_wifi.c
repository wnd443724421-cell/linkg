/**
 * @file network_wifi.c
 * @brief LinkG网络服务Wi-Fi生命周期实现
 */

#include "network_wifi.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_wifi.h"

#include "network_internal.h"
#include "network_manager.h"
#include "linkg_discovery.h"
#include "linkg_log.h"
#include "linkg_switch.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断Wi-Fi模块是否已经初始化。
 */
static bool _linkg_network_wifi_is_initialized(void)
{
    bool initialized;

    pthread_mutex_lock(&g_network.lock);

    initialized = g_network.wifi_worker.module_initialized;

    pthread_mutex_unlock(&g_network.lock);

    return initialized;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi模块。
 *
 * 每轮Owner启动均重新读取Wi-Fi配置，避免完整重启后继续使用旧配置快照。
 * 本函数由Wi-Fi Owner线程调用，不在Network整体初始化阶段执行。
 */
int _linkg_network_wifi_init(void)
{
    linkg_wifi_config_t wifi_config;
    linkg_paths_config_t paths_config;
    linkg_packet_pool_t *packet_pool;
    linkg_device_role_t  role;
    uint8_t              node_id;
    int                  ret;

    if (_linkg_network_wifi_is_initialized())
    {
        return -EALREADY;
    }

    memset(&wifi_config, 0, sizeof(wifi_config));
    memset(&paths_config, 0, sizeof(paths_config));

    ret = linkg_config_get_wifi(&wifi_config);
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

    role        = g_network.role;
    node_id     = g_network.network_config.node_id;
    packet_pool = g_network.packet_pool;

    pthread_mutex_unlock(&g_network.lock);

    if (packet_pool == NULL)
    {
        return -ENODEV;
    }

    ret = linkg_wifi_init(role, node_id, &wifi_config, paths_config.wifi.enabled, packet_pool);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.wifi_config                    = wifi_config;
    g_network.wifi_worker.module_initialized = true;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}

/**
 * @brief 启动Wi-Fi模块并通知设备发现。
 */
int _linkg_network_wifi_start(void)
{
    int ret;

    if (!_linkg_network_wifi_is_initialized())
    {
        return -ENODEV;
    }

    ret = linkg_wifi_start();
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_notify_wifi_network_changed();
    if (ret != 0)
    {
        LINKG_LOG_WARN("notify Wi-Fi Discovery failed, error=%d", ret);
    }

    ret = linkg_switch_end_maintenance(LINKG_LINK_ACCESS_WIFI);
    if (ret != 0)
    {
        LINKG_LOG_WARN("end Wi-Fi maintenance failed, error=%d", ret);
    }

    return 0;
}

/**
 * @brief 运行Wi-Fi Owner控制循环。
 *
 * 本函数在Wi-Fi Owner线程中执行，退出后由Worker统一执行停止和反初始化。
 */
int _linkg_network_wifi_run(linkg_thread_t *owner_thread)
{
    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    if (!_linkg_network_wifi_is_initialized())
    {
        return -ENODEV;
    }

    return linkg_wifi_run(owner_thread);
}

/**
 * @brief 停止Wi-Fi模块运行资源。
 *
 * 停止成功后模块仍处于已初始化状态，后续由deinit释放初始化资源。
 */
int _linkg_network_wifi_stop(void)
{
    if (!_linkg_network_wifi_is_initialized())
    {
        return 0;
    }

    return linkg_wifi_stop();
}

/**
 * @brief 反初始化Wi-Fi模块。
 *
 * 仅在实际释放成功后清除模块初始化标志，失败时保留状态供后续清理。
 */
int _linkg_network_wifi_deinit(void)
{
    int ret;

    if (!_linkg_network_wifi_is_initialized())
    {
        return 0;
    }

    ret = linkg_wifi_deinit();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.wifi_worker.module_initialized = false;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}

/****************************** 独立重启 ******************************/

/**
 * @brief 请求Wi-Fi独立完整重启。
 *
 * 只提交请求并唤醒Network Manager，不在调用线程中执行耗时生命周期操作。
 */
int _linkg_network_wifi_restart(void)
{
    return _linkg_network_manager_request_wifi_restart();
}
