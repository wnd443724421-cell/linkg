/**
 * @file linkg_wifi.c
 * @brief LinkG Wi-Fi运行控制实现
 * @author Dawn
 * @version 3.0.0
 * @date 2026-08-26
 */

#include "linkg_wifi.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <string.h>

#include "linkg_network_ops.h"
#include "linkg_time.h"

#include "wifi_address.h"
#include "wifi_driver_loader.h"
#include "wifi_driver_ops.h"
#include "wifi_internal.h"
#include "wifi_monitor.h"
#include "wifi_platform_internal.h"
#include "wifi_radio.h"
#include "wifi_service_ops.h"
#include "wifi_status.h"

/****************************** 运行参数 ******************************/

#define WIFI_INTERFACE_CREATE_TIMEOUT_MS 5000U // 无线接口创建等待时间

/****************************** 模块上下文 ******************************/

static linkg_wifi_context_t g_wifi =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,           // 上下文操作锁
    .lifecycle = LINKG_WIFI_LIFECYCLE_UNINITIALIZED, // Wi-Fi生命周期
    .role      = LINKG_DEVICE_ROLE_UNKNOWN            // 当前设备角色
};

/****************************** 上下文辅助 ******************************/

/**
 * @brief 设置Wi-Fi生命周期状态。
 */
static void _linkg_wifi_set_lifecycle(linkg_wifi_lifecycle_t lifecycle)
{
    pthread_mutex_lock(&g_wifi.lock);
    g_wifi.lifecycle = lifecycle;
    pthread_mutex_unlock(&g_wifi.lock);
}

/**
 * @brief 获取Wi-Fi生命周期状态。
 */
static linkg_wifi_lifecycle_t _linkg_wifi_get_lifecycle(void)
{
    linkg_wifi_lifecycle_t lifecycle;

    pthread_mutex_lock(&g_wifi.lock);
    lifecycle = g_wifi.lifecycle;
    pthread_mutex_unlock(&g_wifi.lock);

    return lifecycle;
}

/**
 * @brief 清空Wi-Fi模块上下文中的动态字段。
 *
 * @note 调用方必须持有g_wifi.lock，且所有子模块资源已经释放。
 */
static void _linkg_wifi_reset_context_locked(void)
{
    memset(&g_wifi.config, 0, sizeof(g_wifi.config));
    memset(&g_wifi.ipv4, 0, sizeof(g_wifi.ipv4));
    memset(&g_wifi.runtime, 0, sizeof(g_wifi.runtime));

    g_wifi.lifecycle = LINKG_WIFI_LIFECYCLE_UNINITIALIZED;
    g_wifi.role      = LINKG_DEVICE_ROLE_UNKNOWN;
    g_wifi.node_id   = 0U;
}

/****************************** 运行参数 ******************************/

/**
 * @brief 应用无线接口通用运行参数并配置IPv4地址。
 */
static int _linkg_wifi_apply_interface_runtime(void)
{
    int ret;

    ret = wifi_driver_set_log_level(WIFI_DRIVER_LOG_LEVEL_ERROR);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_driver_set_napi_state(false);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_driver_set_low_latency(true);
    if (ret != 0)
    {
        return ret;
    }

    if (g_wifi.role == LINKG_DEVICE_ROLE_STA)
    {
        ret = wifi_driver_set_sta_power_save(false);
        if (ret != 0)
        {
            return ret;
        }
    }

    return linkg_network_interface_set_ipv4(WIFI_PLATFORM_INTERFACE_NAME, &g_wifi.ipv4.ip, &g_wifi.ipv4.netmask);
}

/****************************** 运行事件 ******************************/

/**
 * @brief 清空一个Wi-Fi运行事件。
 */
static void _linkg_wifi_clear_runtime_event(wifi_runtime_event_t *event)
{
    if (event == NULL)
    {
        return;
    }

    memset(event, 0, sizeof(*event));
    event->type = WIFI_RUNTIME_EVENT_NONE;
}

/**
 * @brief 将STA连接事件同步到运行状态和无线参数维护模块。
 */
static int _linkg_wifi_apply_link_event(const wifi_runtime_event_t *event, uint64_t now_ms)
{
    int ret;

    ret = wifi_runtime_apply_event(&g_wifi.runtime, event, now_ms);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_radio_sync_runtime(&g_wifi.runtime, now_ms);
    if (ret != 0)
    {
        WIFI_WARN("synchronize radio state after STA link event failed, event=%d, error=%d",
                  (int)event->type,
                  ret);
    }

    return 0;
}

/**
 * @brief 向运行状态提交一个内部恢复阶段事件。
 */
static int _linkg_wifi_apply_recovery_event(wifi_runtime_event_type_t type,
                                             wifi_runtime_recovery_reason_t reason,
                                             int error,
                                             uint64_t now_ms)
{
    wifi_runtime_event_t event;

    _linkg_wifi_clear_runtime_event(&event);

    event.type            = type;
    event.recovery_reason = reason;
    event.error           = error;

    return wifi_runtime_apply_event(&g_wifi.runtime, &event, now_ms);
}

/**
 * @brief 执行STA服务恢复并重新建立连接监控。
 *
 * @note 本函数只能由network-wifi Owner线程调用；服务启动内部仍保留
 *       STA连接前无线参数准备的硬件时序。
 */
static int _linkg_wifi_recover_sta_service(void)
{
    wifi_runtime_event_t initial_event;
    uint64_t             now_ms;
    int                  sync_ret;
    int                  ret;

    if (g_wifi.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EOPNOTSUPP;
    }

    if (!wifi_runtime_recovery_required(&g_wifi.runtime))
    {
        return 0;
    }

    now_ms = linkg_time_elapsed_ms();

    ret = _linkg_wifi_apply_recovery_event(WIFI_RUNTIME_EVENT_RECOVERY_STARTED,
                                            g_wifi.runtime.recovery_reason,
                                            0,
                                            now_ms);
    if (ret != 0)
    {
        return ret;
    }

    sync_ret = wifi_radio_sync_runtime(&g_wifi.runtime, now_ms);
    if (sync_ret != 0)
    {
        WIFI_WARN("synchronize radio state before STA service recovery failed, error=%d",
                  sync_ret);
    }

    wifi_monitor_stop();

    ret = wifi_service_ops_stop(LINKG_DEVICE_ROLE_STA);
    if (ret != 0)
    {
        goto fail;
    }

    ret = wifi_service_ops_start(LINKG_DEVICE_ROLE_STA, &g_wifi.config);
    if (ret != 0)
    {
        goto fail;
    }

    now_ms = linkg_time_elapsed_ms();

    wifi_radio_notify_reapplied(&g_wifi.runtime, now_ms);

    ret = _linkg_wifi_apply_recovery_event(WIFI_RUNTIME_EVENT_RECOVERY_SUCCEEDED,
                                            WIFI_RUNTIME_RECOVERY_REASON_NONE,
                                            0,
                                            now_ms);
    if (ret != 0)
    {
        return ret;
    }

    sync_ret = wifi_radio_sync_runtime(&g_wifi.runtime, now_ms);
    if (sync_ret != 0)
    {
        WIFI_WARN("synchronize radio state after STA service recovery failed, error=%d",
                  sync_ret);
    }

    _linkg_wifi_clear_runtime_event(&initial_event);

    ret = wifi_monitor_start(now_ms, &initial_event);
    if (ret != 0)
    {
        goto fail_after_recovery;
    }

    if (initial_event.type == WIFI_RUNTIME_EVENT_STA_CONNECTED ||
        initial_event.type == WIFI_RUNTIME_EVENT_STA_DISCONNECTED)
    {
        ret = _linkg_wifi_apply_link_event(&initial_event, now_ms);
        if (ret != 0)
        {
            goto fail_after_recovery;
        }
    }

    WIFI_INFO("STA service recovery completed");

    return 0;

fail:
    now_ms = linkg_time_elapsed_ms();

    (void)_linkg_wifi_apply_recovery_event(WIFI_RUNTIME_EVENT_RECOVERY_FAILED,
                                           g_wifi.runtime.recovery_reason,
                                           ret,
                                           now_ms);

    WIFI_ERROR("STA service recovery failed, error=%d", ret);

    return ret;

fail_after_recovery:
    /**
     * 服务本身已经恢复成功，但运行状态重新接管失败。
     * 将其重新标记为恢复失败，交由上层结束本轮Wi-Fi Owner运行。
     */
    g_wifi.runtime.recovery_state  = WIFI_RUNTIME_RECOVERY_RUNNING;
    g_wifi.runtime.recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_EVENT_CHANNEL;

    (void)_linkg_wifi_apply_recovery_event(WIFI_RUNTIME_EVENT_RECOVERY_FAILED,
                                           WIFI_RUNTIME_RECOVERY_REASON_EVENT_CHANNEL,
                                           ret,
                                           linkg_time_elapsed_ms());

    WIFI_ERROR("resume STA runtime after service recovery failed, error=%d", ret);

    return ret;
}

/**
 * @brief 应用monitor输出的Wi-Fi语义运行事件。
 */
static int _linkg_wifi_handle_runtime_event(const wifi_runtime_event_t *event, uint64_t now_ms)
{
    int ret;

    if (event == NULL || event->type == WIFI_RUNTIME_EVENT_NONE)
    {
        return 0;
    }

    switch (event->type)
    {
        case WIFI_RUNTIME_EVENT_STA_CONNECTED:
        case WIFI_RUNTIME_EVENT_STA_DISCONNECTED:
            return _linkg_wifi_apply_link_event(event, now_ms);

        case WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED:
            ret = wifi_runtime_apply_event(&g_wifi.runtime, event, now_ms);
            if (ret != 0)
            {
                return ret;
            }

            return _linkg_wifi_recover_sta_service();

        default:
            return -EINVAL;
    }
}

/****************************** Owner循环 ******************************/

/**
 * @brief 获取monitor和radio最近的运行期限。
 */
static uint64_t _linkg_wifi_get_runtime_deadline(void)
{
    uint64_t deadline;
    uint64_t radio_deadline;
    uint64_t monitor_deadline;

    radio_deadline = wifi_radio_get_deadline();

    if (g_wifi.role != LINKG_DEVICE_ROLE_STA)
    {
        return radio_deadline;
    }

    monitor_deadline = wifi_monitor_get_deadline();

    if (radio_deadline == 0U)
    {
        return monitor_deadline;
    }

    if (monitor_deadline == 0U)
    {
        return radio_deadline;
    }

    deadline = radio_deadline < monitor_deadline
        ? radio_deadline
        : monitor_deadline;

    return deadline;
}

/**
 * @brief 将单调时间期限转换为poll超时毫秒数。
 */
static int _linkg_wifi_get_poll_timeout(uint64_t now_ms)
{
    uint64_t deadline;
    uint64_t delay_ms;

    deadline = _linkg_wifi_get_runtime_deadline();
    if (deadline == 0U)
    {
        return -1;
    }

    if (deadline <= now_ms)
    {
        return 0;
    }

    delay_ms = deadline - now_ms;
    if (delay_ms > INT_MAX)
    {
        return INT_MAX;
    }

    return (int)delay_ms;
}

/**
 * @brief 处理一次STA monitor定时任务。
 */
static int _linkg_wifi_process_monitor(uint64_t now_ms)
{
    wifi_runtime_event_t event;
    int                  ret;

    if (g_wifi.role != LINKG_DEVICE_ROLE_STA)
    {
        return 0;
    }

    _linkg_wifi_clear_runtime_event(&event);

    ret = wifi_monitor_process(now_ms, &event);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_wifi_handle_runtime_event(&event, now_ms);
}

/**
 * @brief 处理一次无线参数维护任务。
 */
static void _linkg_wifi_process_radio(uint64_t now_ms)
{
    int ret;

    ret = wifi_radio_process(&g_wifi.runtime, now_ms);
    if (ret != 0)
    {
        /**
         * wifi_radio内部已经保留重试期限和失败计数。
         * 单次驱动控制失败不终止Wi-Fi Owner，避免瞬时驱动错误放大成服务重启。
         */
        WIFI_DEBUG("radio maintenance returned error=%d", ret);
    }
}

/**
 * @brief 处理STA WPA事件文件描述符。
 */
static int _linkg_wifi_process_monitor_fd(short revents, uint64_t now_ms)
{
    wifi_runtime_event_t event;
    int                  ret;

    if (g_wifi.role != LINKG_DEVICE_ROLE_STA)
    {
        return 0;
    }

    _linkg_wifi_clear_runtime_event(&event);

    if ((revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        ret = wifi_monitor_handle_event_channel_error(now_ms, &event);
    }
    else if ((revents & POLLIN) != 0)
    {
        ret = wifi_monitor_handle_event_fd(now_ms, &event);
    }
    else
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    return _linkg_wifi_handle_runtime_event(&event, now_ms);
}

/**
 * @brief 运行Wi-Fi唯一控制面Owner事件循环。
 *
 * @note 本函数在network-wifi线程中运行；monitor和radio不创建控制线程，
 *       所有运行状态变化和服务恢复均在本线程串行执行。
 */
static int _linkg_wifi_owner_loop(linkg_thread_t *owner_thread)
{
    struct pollfd descriptors[2];
    uint64_t      now_ms;
    int           monitor_fd;
    int           timeout_ms;
    int           wakeup_fd;
    int           ret;
    nfds_t        descriptor_count;

    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(owner_thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    while (linkg_thread_is_running(owner_thread))
    {
        memset(descriptors, 0, sizeof(descriptors));

        descriptors[0].fd     = wakeup_fd;
        descriptors[0].events = POLLIN;
        descriptor_count      = 1U;

        if (g_wifi.role == LINKG_DEVICE_ROLE_STA)
        {
            monitor_fd = wifi_monitor_get_event_fd();
            if (monitor_fd >= 0)
            {
                descriptors[descriptor_count].fd     = monitor_fd;
                descriptors[descriptor_count].events = POLLIN;
                descriptor_count++;
            }
            else if (monitor_fd != -ENOTCONN)
            {
                return monitor_fd;
            }
        }

        now_ms     = linkg_time_elapsed_ms();
        timeout_ms = _linkg_wifi_get_poll_timeout(now_ms);

        do
        {
            ret = poll(descriptors, descriptor_count, timeout_ms);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(owner_thread));

        if (ret < 0)
        {
            return -errno;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(owner_thread);
            if (ret != 0)
            {
                return ret;
            }

            if (!linkg_thread_is_running(owner_thread))
            {
                break;
            }
        }

        now_ms = linkg_time_elapsed_ms();

        if (descriptor_count > 1U && descriptors[1].revents != 0)
        {
            ret = _linkg_wifi_process_monitor_fd(descriptors[1].revents, now_ms);
            if (ret != 0)
            {
                return ret;
            }

            now_ms = linkg_time_elapsed_ms();
        }

        ret = _linkg_wifi_process_monitor(now_ms);
        if (ret != 0)
        {
            return ret;
        }

        now_ms = linkg_time_elapsed_ms();
        _linkg_wifi_process_radio(now_ms);
    }

    return 0;
}

/****************************** 运行资源回收 ******************************/

/**
 * @brief 记录运行资源回收期间出现的首个错误。
 */
static void _linkg_wifi_record_cleanup_error(int *first_error, int error)
{
    if (first_error == NULL)
    {
        return;
    }

    if (*first_error == 0 && error != 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 按固定逆序停止全部Wi-Fi运行资源。
 */
static int _linkg_wifi_stop_runtime(void)
{
    int first_error;
    int ret;

    first_error = 0;

    if (g_wifi.role == LINKG_DEVICE_ROLE_STA)
    {
        wifi_monitor_stop();

        ret = wifi_service_ops_disconnect_sta();
        if (ret != 0 && ret != -ENOTCONN)
        {
            WIFI_WARN("disconnect STA before service stop failed, error=%d", ret);
        }
    }

    ret = wifi_radio_stop();
    if (ret != 0)
    {
        WIFI_WARN("stop Wi-Fi radio maintenance failed, error=%d", ret);
        _linkg_wifi_record_cleanup_error(&first_error, ret);
    }

    ret = wifi_status_stop();
    if (ret != 0)
    {
        WIFI_WARN("stop Wi-Fi status module failed, error=%d", ret);
        _linkg_wifi_record_cleanup_error(&first_error, ret);
    }

    ret = wifi_service_ops_stop(g_wifi.role);
    if (ret != 0)
    {
        WIFI_WARN("stop Wi-Fi role service failed, role=%d, error=%d",
                  (int)g_wifi.role,
                  ret);

        _linkg_wifi_record_cleanup_error(&first_error, ret);
    }

    if (linkg_network_interface_exists(WIFI_PLATFORM_INTERFACE_NAME))
    {
        ret = linkg_network_interface_set_up(WIFI_PLATFORM_INTERFACE_NAME, false);
        if (ret != 0 && ret != -ENODEV && ret != -ENXIO)
        {
            WIFI_WARN("set Wi-Fi interface down failed, interface=%s, error=%d",
                      WIFI_PLATFORM_INTERFACE_NAME,
                      ret);

            _linkg_wifi_record_cleanup_error(&first_error, ret);
        }
    }

    return first_error;
}

/**
 * @brief 释放全部Wi-Fi子模块初始化资源。
 */
static int _linkg_wifi_deinit_modules(void)
{
    int first_error;
    int ret;

    first_error = 0;

    if (g_wifi.role == LINKG_DEVICE_ROLE_STA)
    {
        wifi_monitor_deinit();
    }

    wifi_radio_deinit();

    ret = wifi_status_deinit();
    if (ret != 0)
    {
        _linkg_wifi_record_cleanup_error(&first_error, ret);
    }

    wifi_driver_ops_deinit();

    return first_error;
}

/****************************** 初始化 ******************************/

/**
 * @brief 初始化Wi-Fi模块及其内部子模块。
 *
 * @note 本接口只初始化进程内资源，不加载驱动、不操作无线接口。
 */
int linkg_wifi_init(linkg_device_role_t role, uint8_t node_id, const linkg_wifi_config_t *config)
{
    linkg_network_ipv4_config_t ipv4;
    wifi_runtime_t              runtime;
    int                         cleanup_ret;
    int                         ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    ret = linkg_wifi_config_validate(config);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_address_from_node_id(node_id, &ipv4);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_runtime_init(&runtime, role, linkg_time_elapsed_ms());
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_wifi.lock);

    if (g_wifi.lifecycle != LINKG_WIFI_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_wifi.lock);
        return -EALREADY;
    }

    g_wifi.lifecycle = LINKG_WIFI_LIFECYCLE_INITIALIZING;

    pthread_mutex_unlock(&g_wifi.lock);

    if (config->enabled)
    {
        ret = wifi_driver_ops_init();
        if (ret != 0)
        {
            goto fail;
        }

        ret = wifi_status_init(role, config);
        if (ret != 0)
        {
            goto fail;
        }

        ret = wifi_radio_init(role, config);
        if (ret != 0)
        {
            goto fail;
        }

        if (role == LINKG_DEVICE_ROLE_STA)
        {
            ret = wifi_monitor_init();
            if (ret != 0)
            {
                goto fail;
            }
        }
    }

    pthread_mutex_lock(&g_wifi.lock);

    g_wifi.role      = role;
    g_wifi.node_id   = node_id;
    g_wifi.config    = *config;
    g_wifi.ipv4      = ipv4;
    g_wifi.runtime   = runtime;
    g_wifi.lifecycle = LINKG_WIFI_LIFECYCLE_STOPPED;

    pthread_mutex_unlock(&g_wifi.lock);

    WIFI_INFO("module initialized, role=%d, node_id=%u, enabled=%d",
              (int)role,
              (unsigned int)node_id,
              config->enabled);

    return 0;

fail:
    /**
     * role/config尚未写入g_wifi，因此先写入用于按角色清理本轮已经初始化的模块。
     */
    pthread_mutex_lock(&g_wifi.lock);
    g_wifi.role = role;
    pthread_mutex_unlock(&g_wifi.lock);

    cleanup_ret = _linkg_wifi_deinit_modules();
    if (cleanup_ret != 0)
    {
        WIFI_WARN("cleanup Wi-Fi modules after init failure failed, error=%d", cleanup_ret);
    }

    pthread_mutex_lock(&g_wifi.lock);
    _linkg_wifi_reset_context_locked();
    pthread_mutex_unlock(&g_wifi.lock);

    return ret;
}

/****************************** 启动 ******************************/

/**
 * @brief 启动Wi-Fi模块及其运行资源。
 */
int linkg_wifi_start(void)
{
    wifi_runtime_event_t initial_event;
    const char          *phase;
    bool                 driver_was_loaded;
    bool                 ini_changed;
    uint64_t             now_ms;
    int                  cleanup_ret;
    int                  ret;

    pthread_mutex_lock(&g_wifi.lock);

    if (g_wifi.lifecycle == LINKG_WIFI_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_wifi.lock);
        return -ENODEV;
    }

    if (g_wifi.lifecycle == LINKG_WIFI_LIFECYCLE_INITIALIZING ||
        g_wifi.lifecycle == LINKG_WIFI_LIFECYCLE_DEINITIALIZING)
    {
        pthread_mutex_unlock(&g_wifi.lock);
        return -EBUSY;
    }

    if (g_wifi.lifecycle != LINKG_WIFI_LIFECYCLE_STOPPED)
    {
        ret = g_wifi.lifecycle == LINKG_WIFI_LIFECYCLE_ERROR ? -EIO : -EALREADY;
        pthread_mutex_unlock(&g_wifi.lock);
        return ret;
    }

    g_wifi.lifecycle = LINKG_WIFI_LIFECYCLE_STARTING;

    if (!g_wifi.config.enabled)
    {
        g_wifi.lifecycle = LINKG_WIFI_LIFECYCLE_RUNNING;
        pthread_mutex_unlock(&g_wifi.lock);

        WIFI_INFO("module start skipped because Wi-Fi is disabled");
        return 0;
    }

    pthread_mutex_unlock(&g_wifi.lock);

    phase = "prepare_service_config";
    ret = wifi_service_ops_update_config(g_wifi.role, &g_wifi.config);
    if (ret != 0)
    {
        goto fail;
    }

    driver_was_loaded = wifi_driver_is_loaded();

    phase = "prepare_driver_ini";
    ret = wifi_driver_prepare_ini(g_wifi.config.wideband.work_mode, &ini_changed);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "load_driver";
    ret = wifi_driver_load();
    if (ret != 0)
    {
        goto fail;
    }

    phase = "wait_interface";
    ret = linkg_network_interface_wait(WIFI_PLATFORM_INTERFACE_NAME, WIFI_INTERFACE_CREATE_TIMEOUT_MS);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "interface_down";
    ret = linkg_network_interface_set_up(WIFI_PLATFORM_INTERFACE_NAME, false);
    if (ret != 0)
    {
        goto fail;
    }

    if (driver_was_loaded && ini_changed)
    {
        phase = "reload_driver_ini";
        ret = wifi_driver_reload_ini(g_wifi.role);
        if (ret != 0)
        {
            goto fail;
        }
    }

    phase = "interface_up";
    ret = linkg_network_interface_set_up(WIFI_PLATFORM_INTERFACE_NAME, true);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "disable_power_management";
    ret = wifi_driver_set_power_management(false);
    if (ret != 0)
    {
        goto fail;
    }

    /**
     * 服务启动内部保留经过实机验证的角色相关无线配置时序：
     * STA在wpa_supplicant启动前准备连接用窄带/自适应参数；
     * AP在hostapd VAP进入ENABLED后应用最终无线参数。
     */
    phase = "start_role_service";
    ret = wifi_service_ops_start(g_wifi.role, &g_wifi.config);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "apply_interface_runtime";
    ret = _linkg_wifi_apply_interface_runtime();
    if (ret != 0)
    {
        goto fail;
    }

    phase = "start_status";
    ret = wifi_status_start();
    if (ret != 0)
    {
        goto fail;
    }

    now_ms = linkg_time_elapsed_ms();
    wifi_runtime_reset(&g_wifi.runtime, now_ms);

    _linkg_wifi_clear_runtime_event(&initial_event);

    if (g_wifi.role == LINKG_DEVICE_ROLE_STA)
    {
        phase = "start_monitor";
        ret = wifi_monitor_start(now_ms, &initial_event);
        if (ret != 0)
        {
            goto fail;
        }

        if (initial_event.type == WIFI_RUNTIME_EVENT_STA_CONNECTED ||
            initial_event.type == WIFI_RUNTIME_EVENT_STA_DISCONNECTED)
        {
            ret = wifi_runtime_apply_event(&g_wifi.runtime, &initial_event, now_ms);
            if (ret != 0)
            {
                goto fail;
            }
        }
    }

    phase = "start_radio";
    ret = wifi_radio_start(&g_wifi.runtime, now_ms);
    if (ret != 0)
    {
        goto fail;
    }

    _linkg_wifi_set_lifecycle(LINKG_WIFI_LIFECYCLE_RUNNING);

    WIFI_INFO("module started, role=%d, interface=%s",
              (int)g_wifi.role,
              WIFI_PLATFORM_INTERFACE_NAME);

    return 0;

fail:
    cleanup_ret = _linkg_wifi_stop_runtime();

    _linkg_wifi_set_lifecycle(cleanup_ret == 0
        ? LINKG_WIFI_LIFECYCLE_STOPPED
        : LINKG_WIFI_LIFECYCLE_ERROR);

    if (cleanup_ret != 0)
    {
        WIFI_ERROR("rollback Wi-Fi startup failed, error=%d", cleanup_ret);
    }

    WIFI_ERROR("module start failed, phase=%s, role=%d, error=%d",
               phase,
               (int)g_wifi.role,
               ret);

    return ret;
}

/****************************** 运行 ******************************/

/**
 * @brief 在network-wifi Owner线程中运行Wi-Fi控制面事件循环。
 */
int linkg_wifi_run(linkg_thread_t *owner_thread)
{
    linkg_wifi_lifecycle_t lifecycle;

    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    lifecycle = _linkg_wifi_get_lifecycle();
    if (lifecycle != LINKG_WIFI_LIFECYCLE_RUNNING)
    {
        return lifecycle == LINKG_WIFI_LIFECYCLE_UNINITIALIZED ? -ENODEV : -EBUSY;
    }

    return _linkg_wifi_owner_loop(owner_thread);
}

/****************************** 停止 ******************************/

/**
 * @brief 停止Wi-Fi模块及其全部运行资源。
 */
int linkg_wifi_stop(void)
{
    linkg_wifi_lifecycle_t lifecycle;
    int                    ret;

    lifecycle = _linkg_wifi_get_lifecycle();

    if (lifecycle == LINKG_WIFI_LIFECYCLE_UNINITIALIZED ||
        lifecycle == LINKG_WIFI_LIFECYCLE_STOPPED)
    {
        return 0;
    }

    if (lifecycle == LINKG_WIFI_LIFECYCLE_INITIALIZING ||
        lifecycle == LINKG_WIFI_LIFECYCLE_STARTING ||
        lifecycle == LINKG_WIFI_LIFECYCLE_STOPPING ||
        lifecycle == LINKG_WIFI_LIFECYCLE_DEINITIALIZING)
    {
        return -EBUSY;
    }

    _linkg_wifi_set_lifecycle(LINKG_WIFI_LIFECYCLE_STOPPING);

    if (!g_wifi.config.enabled)
    {
        _linkg_wifi_set_lifecycle(LINKG_WIFI_LIFECYCLE_STOPPED);
        return 0;
    }

    ret = _linkg_wifi_stop_runtime();

    _linkg_wifi_set_lifecycle(ret == 0
        ? LINKG_WIFI_LIFECYCLE_STOPPED
        : LINKG_WIFI_LIFECYCLE_ERROR);

    if (ret != 0)
    {
        WIFI_ERROR("module stop incomplete, error=%d", ret);
        return ret;
    }

    WIFI_INFO("module stopped");

    return 0;
}

/****************************** 反初始化 ******************************/

/**
 * @brief 停止并反初始化Wi-Fi模块。
 */
int linkg_wifi_deinit(void)
{
    bool resources_initialized;
    int  cleanup_ret;
    int  ret;

    ret = linkg_wifi_stop();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_wifi.lock);

    if (g_wifi.lifecycle == LINKG_WIFI_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_wifi.lock);
        return 0;
    }

    if (g_wifi.lifecycle != LINKG_WIFI_LIFECYCLE_STOPPED)
    {
        pthread_mutex_unlock(&g_wifi.lock);
        return -EBUSY;
    }

    g_wifi.lifecycle       = LINKG_WIFI_LIFECYCLE_DEINITIALIZING;
    resources_initialized = g_wifi.config.enabled;

    pthread_mutex_unlock(&g_wifi.lock);

    cleanup_ret = 0;

    if (resources_initialized)
    {
        cleanup_ret = _linkg_wifi_deinit_modules();
    }

    pthread_mutex_lock(&g_wifi.lock);
    _linkg_wifi_reset_context_locked();
    pthread_mutex_unlock(&g_wifi.lock);

    if (cleanup_ret != 0)
    {
        WIFI_ERROR("module deinitialization completed with cleanup error=%d",
                   cleanup_ret);
        return cleanup_ret;
    }

    WIFI_INFO("module deinitialized");

    return 0;
}

/****************************** 状态读取 ******************************/

/**
 * @brief 获取最新有效的统一Wi-Fi状态快照。
 */
int linkg_wifi_get_status(linkg_wifi_status_snapshot_t *snapshot)
{
    wifi_status_info_t       info;
    linkg_wifi_lifecycle_t   lifecycle;
    bool                     enabled;
    int                      ret;

    if (snapshot == NULL)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    pthread_mutex_lock(&g_wifi.lock);
    lifecycle = g_wifi.lifecycle;
    enabled   = g_wifi.config.enabled;
    pthread_mutex_unlock(&g_wifi.lock);

    if (lifecycle == LINKG_WIFI_LIFECYCLE_UNINITIALIZED || !enabled)
    {
        return -ENODEV;
    }

    if (lifecycle != LINKG_WIFI_LIFECYCLE_RUNNING)
    {
        return -ENETDOWN;
    }

    ret = wifi_status_get_info(&info);
    if (ret != 0)
    {
        return ret;
    }

    if (!info.valid)
    {
        return -EAGAIN;
    }

    *snapshot = info.snapshot;

    return 0;
}
