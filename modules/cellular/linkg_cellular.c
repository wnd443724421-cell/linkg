/**
 * @file linkg_cellular.c
 * @brief LinkG蜂窝网络运行控制实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-02
 */

#include "linkg_cellular.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular_link.h"
#include "linkg_link_manager.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

#include "at_channel.h"

#include "cellular_internal.h"
#include "cellular_fsm.h"
#include "cellular_health.h"
#include "cellular_modem.h"
#include "cellular_monitor.h"
#include "cellular_status.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_POLL_DESCRIPTOR_MAX 2U         // Owner循环最大poll描述符数量
#define LINKG_CELLULAR_LINK_NAME           "cellular" // 蜂窝业务链路名称

/****************************** 内部类型 ******************************/

typedef enum
{
    LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED = 0, // 蜂窝模块尚未初始化
    LINKG_CELLULAR_LIFECYCLE_INITIALIZING,      // 蜂窝模块正在初始化
    LINKG_CELLULAR_LIFECYCLE_INITIALIZED,       // 蜂窝模块已经初始化
    LINKG_CELLULAR_LIFECYCLE_STARTING,          // 蜂窝模块正在启动
    LINKG_CELLULAR_LIFECYCLE_RUNNING,           // 蜂窝模块已经启动
    LINKG_CELLULAR_LIFECYCLE_STOPPING,          // 蜂窝模块正在停止
    LINKG_CELLULAR_LIFECYCLE_FAILED             // 蜂窝模块发生不可恢复运行错误
} linkg_cellular_lifecycle_t;

typedef struct
{
    pthread_mutex_t            lock;                   // 模块状态锁，保护模块生命周期

    linkg_cellular_config_t    config;                 // 蜂窝模块配置副本
    cellular_fsm_t             fsm;                    // network-cell Owner连接状态机

    linkg_link_t               *link;                   // 蜂窝业务Link，由蜂窝模块创建并拥有

    linkg_cellular_lifecycle_t lifecycle;              // 蜂窝模块生命周期
    int                        last_error;             // 最近一次不可恢复生命周期错误
    bool                       monitor_initialized;    // Monitor软件资源是否已经初始化
    bool                       status_initialized;     // Status软件资源是否已经初始化
    bool                       health_initialized;     // Health软件资源是否已初始化
    bool                       monitor_started;        // Monitor是否已经注册URC回调
    bool                       status_started;         // Status是否已经借用当前AT通道
    bool                       health_started;         // Health后台检测线程是否已启动
    bool                       data_ready;              // 本机Cellular IPv6数据通道是否已完成建链
    bool                       ipv4_internet_available; // IPv4公网健康检测结果
    bool                       ipv6_internet_available; // IPv6公网健康检测结果
} linkg_cellular_context_t;

/****************************** 全局上下文 ******************************/

static linkg_cellular_context_t g_cellular =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,             // 模块状态锁静态初始化
    .lifecycle = LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED // 初始生命周期
};

/****************************** 通用辅助 ******************************/

/**
 * @brief 记录清理过程中出现的首个错误。
 */
static void _linkg_cellular_record_first_error(int *first_error, int error)
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
 * @brief 返回两个非零绝对期限中更早的一个。
 */
static uint64_t _linkg_cellular_min_deadline(uint64_t left, uint64_t right)
{
    if (left == 0U)
    {
        return right;
    }

    if (right == 0U)
    {
        return left;
    }

    return left < right ? left : right;
}

/**
 * @brief 将绝对单调期限转换为poll超时毫秒数。
 */
static int _linkg_cellular_deadline_to_timeout(uint64_t now_ms, uint64_t deadline_ms)
{
    uint64_t delay_ms;

    if (deadline_ms == 0U)
    {
        return -1;
    }

    if (deadline_ms <= now_ms)
    {
        return 0;
    }

    delay_ms = deadline_ms - now_ms;
    if (delay_ms > (uint64_t)INT_MAX)
    {
        return INT_MAX;
    }

    return (int)delay_ms;
}

/**
 * @brief 清空模块上下文动态字段并恢复未初始化生命周期。
 *
 * @note 调用方必须持有g_cellular.lock，且全部子模块资源已经释放。
 */
static void _linkg_cellular_reset_context_locked(void)
{
    memset(&g_cellular.config, 0, sizeof(g_cellular.config));
    memset(&g_cellular.fsm, 0, sizeof(g_cellular.fsm));

    g_cellular.link                    = NULL;
    g_cellular.lifecycle               = LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED;
    g_cellular.last_error              = 0;
    g_cellular.monitor_initialized     = false;
    g_cellular.status_initialized      = false;
    g_cellular.health_initialized      = false;
    g_cellular.monitor_started         = false;
    g_cellular.status_started          = false;
    g_cellular.health_started          = false;
    g_cellular.data_ready              = false;
    g_cellular.ipv4_internet_available = false;
    g_cellular.ipv6_internet_available = false;
}

/**
 * @brief 发布本机Cellular IPv6数据通道就绪状态。
 *
 * @note 仅记录FSM建链结果，不代表IPv4或IPv6公网探测成功。
 */
static void _linkg_cellular_publish_data_ready(void)
{
    cellular_health_info_t info;
    bool                   ready;
    int                    ret;

    ready = cellular_runtime_online(&g_cellular.fsm.runtime);
    cellular_health_set_online(ready);

    memset(&info, 0, sizeof(info));
    ret = cellular_health_get_info(&info);

    pthread_mutex_lock(&g_cellular.lock);
    g_cellular.data_ready              = ready;
    g_cellular.ipv4_internet_available = ready && ret == 0 && info.ipv4_valid && info.ipv4_available;
    g_cellular.ipv6_internet_available = ready && ret == 0 && info.ipv6_valid && info.ipv6_available;
    pthread_mutex_unlock(&g_cellular.lock);
}

/**
 * @brief 设置蜂窝模块生命周期和对应错误码。
 */
static void _linkg_cellular_set_lifecycle(linkg_cellular_lifecycle_t lifecycle, int error)
{
    pthread_mutex_lock(&g_cellular.lock);

    g_cellular.lifecycle  = lifecycle;
    g_cellular.last_error = error;
    if (lifecycle != LINKG_CELLULAR_LIFECYCLE_RUNNING)
    {
        g_cellular.data_ready              = false;
        g_cellular.ipv4_internet_available = false;
        g_cellular.ipv6_internet_available = false;
    }

    pthread_mutex_unlock(&g_cellular.lock);
}

/**
 * @brief 获取Modem管理模块持有的AT通道借用引用。
 */
static at_channel_t *_linkg_cellular_get_channel(void)
{
    return cellular_modem_get_channel();
}

/**
 * @brief 记录蜂窝数据链成功上线时的Host地址。
 */
static void _linkg_cellular_log_connected(const cellular_status_info_t *info)
{
    char        ipv4[INET_ADDRSTRLEN];
    char        ipv6[INET6_ADDRSTRLEN];
    const char *ipv4_text;
    const char *ipv6_text;

    ipv4_text = "<unavailable>";
    ipv6_text = "<unavailable>";

    if (info != NULL && info->host.ipv4_valid && inet_ntop(AF_INET, &info->host.ipv4, ipv4, sizeof(ipv4)) != NULL)
    {
        ipv4_text = ipv4;
    }

    if (info != NULL && info->host.global_ipv6_valid && inet_ntop(AF_INET6, &info->host.global_ipv6, ipv6, sizeof(ipv6)) != NULL)
    {
        ipv6_text = ipv6;
    }

    CELLULAR_INFO("data link connected, ipv4=%s, ipv6=%s", ipv4_text, ipv6_text);
}

/****************************** 状态调度 ******************************/

/**
 * @brief 将Monitor语义事件转换为Status事实刷新集合。
 */
static cellular_status_refresh_mask_t _linkg_cellular_refresh_from_events(const cellular_monitor_events_t *events)
{
    cellular_status_refresh_mask_t refresh;

    if (events == NULL)
    {
        return CELLULAR_STATUS_REFRESH_NONE;
    }

    refresh = CELLULAR_STATUS_REFRESH_NONE;

    if ((events->mask & (CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED | CELLULAR_MONITOR_EVENT_SIM_STATE_CHANGED)) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_SIM;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_REGISTRATION_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_REGISTRATION | CELLULAR_STATUS_REFRESH_RADIO;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_RADIO_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_RADIO;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_PDP_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_PDP | CELLULAR_STATUS_REFRESH_PDP_ADDRESS;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_NETDEV_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_NETDEV |
                   CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                   CELLULAR_STATUS_REFRESH_HOST;
    }

    return refresh;
}

/**
 * @brief 处理本轮到期和事件驱动的Status事实确认。
 */
static int _linkg_cellular_process_status(uint64_t now_ms, cellular_status_refresh_mask_t event_refresh, cellular_status_info_t *info)
{
    cellular_status_refresh_mask_t requested;
    int                            ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    requested = event_refresh | cellular_fsm_take_requested_refresh(&g_cellular.fsm);

    ret = cellular_status_process(now_ms, requested);
    if (ret != 0)
    {
        return ret;
    }

    return cellular_status_get_info(info);
}

/****************************** Owner轮询 ******************************/

/**
 * @brief 获取Status与FSM最近的Owner处理期限。
 */
static uint64_t _linkg_cellular_get_owner_deadline(void)
{
    uint64_t runtime_deadline;
    uint64_t status_deadline;

    status_deadline  = cellular_status_get_deadline();
    runtime_deadline = cellular_fsm_get_deadline(&g_cellular.fsm);

    return _linkg_cellular_min_deadline(status_deadline, runtime_deadline);
}

/**
 * @brief 等待Owner停止请求、Monitor事件或最近运行期限。
 */
static int _linkg_cellular_poll_owner(linkg_thread_t *owner_thread, uint64_t now_ms)
{
    struct pollfd descriptors[LINKG_CELLULAR_POLL_DESCRIPTOR_MAX];
    uint64_t      deadline_ms;
    nfds_t        descriptor_count;
    int           monitor_fd;
    int           timeout_ms;
    int           wakeup_fd;
    int           ret;

    wakeup_fd = linkg_thread_get_wakeup_fd(owner_thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    monitor_fd = cellular_monitor_get_event_fd();
    if (monitor_fd < 0)
    {
        return monitor_fd;
    }

    memset(descriptors, 0, sizeof(descriptors));

    descriptors[0].fd     = wakeup_fd;
    descriptors[0].events = POLLIN;
    descriptors[1].fd     = monitor_fd;
    descriptors[1].events = POLLIN;
    descriptor_count      = LINKG_CELLULAR_POLL_DESCRIPTOR_MAX;

    deadline_ms = _linkg_cellular_get_owner_deadline();
    timeout_ms  = _linkg_cellular_deadline_to_timeout(now_ms, deadline_ms);

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
    }

    if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 运行蜂窝唯一控制面Owner状态机。
 */
static int _linkg_cellular_owner_loop(linkg_thread_t *owner_thread)
{
    cellular_monitor_events_t      events;
    cellular_status_info_t         info;
    cellular_status_refresh_mask_t event_refresh;
    cellular_fsm_step_t            step;
    uint64_t                       now_ms;
    int                            session_result;
    int                            ret;

    while (linkg_thread_is_running(owner_thread))
    {
        now_ms = linkg_time_elapsed_ms();

        memset(&events, 0, sizeof(events));
        ret = cellular_monitor_take_events(&events);
        if (ret != 0)
        {
            return ret;
        }

        if ((events.mask & CELLULAR_MONITOR_EVENT_MODEM_POWERED_DOWN) != 0U)
        {
            CELLULAR_WARN("modem powered down");
            return -ENODEV;
        }

        event_refresh = _linkg_cellular_refresh_from_events(&events);

        memset(&info, 0, sizeof(info));
        ret = _linkg_cellular_process_status(now_ms, event_refresh, &info);
        if (ret != 0)
        {
            return ret;
        }

        session_result = cellular_fsm_sync_sim_session(&g_cellular.fsm, _linkg_cellular_get_channel(), &events, &info, now_ms);
        if (session_result > 0)
        {
            cellular_health_reset_session();
        }

        _linkg_cellular_publish_data_ready();
        if (session_result < 0)
        {
            return session_result;
        }

        if (session_result > 0)
        {
            continue;
        }

        step = cellular_fsm_run(&g_cellular.fsm, &g_cellular.config, _linkg_cellular_get_channel(), &info, now_ms);

        now_ms = linkg_time_elapsed_ms();

        switch (step.result)
        {
            case CELLULAR_RUNTIME_STEP_DONE:
                ret = cellular_fsm_enter(&g_cellular.fsm, step.next_state, now_ms);
                _linkg_cellular_publish_data_ready();
                if (ret != 0)
                {
                    return ret;
                }

                if (step.next_state == CELLULAR_RUNTIME_STATE_ONLINE)
                {
                    _linkg_cellular_log_connected(&info);
                }

                continue;

            case CELLULAR_RUNTIME_STEP_FAILED:
                ret = cellular_fsm_handle_failure(&g_cellular.fsm, _linkg_cellular_get_channel(), step.error, now_ms);
                _linkg_cellular_publish_data_ready();
                if (ret != 0)
                {
                    return ret;
                }

                continue;

            case CELLULAR_RUNTIME_STEP_FATAL:
                return step.error;

            case CELLULAR_RUNTIME_STEP_WAIT:
            default:
                break;
        }

        _linkg_cellular_publish_data_ready();

        ret = _linkg_cellular_poll_owner(owner_thread, linkg_time_elapsed_ms());
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 链路辅助 ******************************/

/**
 * @brief 创建蜂窝业务链路对象。
 */
static int _linkg_cellular_create_link(linkg_packet_pool_t *packet_pool, linkg_link_t **out)
{
    linkg_cellular_link_config_t cellular_config;
    linkg_link_config_t          link_config;

    if (packet_pool == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    memset(&link_config, 0, sizeof(link_config));
    memset(&cellular_config, 0, sizeof(cellular_config));

    link_config.name              = LINKG_CELLULAR_LINK_NAME;
    link_config.access            = LINKG_LINK_ACCESS_CELLULAR;
    link_config.tx_batch_size     = LINKG_LINK_TX_BATCH_SIZE_DEFAULT;
    link_config.rx_batch_size     = LINKG_LINK_RX_BATCH_SIZE_DEFAULT;
    link_config.packet_pool       = packet_pool;
    link_config.receive           = NULL;
    link_config.receive_user_data = NULL;

    cellular_config.data_port     = LINKG_RESOURCE_UDP_PORT_CELLULAR_DATA;
    cellular_config.realtime_port = LINKG_RESOURCE_UDP_PORT_CELLULAR_REALTIME;
    cellular_config.video_port    = LINKG_RESOURCE_UDP_PORT_CELLULAR_VIDEO;

    return linkg_cellular_link_create(&link_config, &cellular_config, out);
}

/****************************** 运行资源回收 ******************************/

/**
 * @brief 按逆序停止蜂窝运行期资源。
 */
static int _linkg_cellular_stop_runtime(void)
{
    int first_error;
    int ret;

    cellular_health_set_online(false);
    if (g_cellular.health_started)
    {
        ret = cellular_health_stop();
        if (ret != 0)
        {
            return ret;
        }

        g_cellular.health_started = false;
    }

    if (g_cellular.link != NULL)
    {
        ret = linkg_link_stop(g_cellular.link);
        if (ret != 0)
        {
            CELLULAR_ERROR("stop cellular business link failed, error=%d", ret);
            return ret;
        }
    }

    first_error = 0;

    ret = cellular_fsm_stop_session(&g_cellular.fsm, _linkg_cellular_get_channel(), linkg_time_elapsed_ms());
    if (ret != 0)
    {
        _linkg_cellular_record_first_error(&first_error, ret);
    }

    if (g_cellular.status_started)
    {
        ret = cellular_status_stop();
        if (ret != 0)
        {
            _linkg_cellular_record_first_error(&first_error, ret);
        }

        g_cellular.status_started = false;
    }

    if (g_cellular.monitor_started)
    {
        ret = cellular_monitor_stop();
        if (ret != 0)
        {
            _linkg_cellular_record_first_error(&first_error, ret);
        }

        g_cellular.monitor_started = false;
    }

    cellular_modem_stop();
    cellular_fsm_reset(&g_cellular.fsm, linkg_time_elapsed_ms());
    _linkg_cellular_publish_data_ready();

    return first_error;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝模块及其内部纯软件子模块。
 */
int linkg_cellular_init(const linkg_cellular_config_t *config, bool path_enabled, linkg_packet_pool_t *packet_pool)
{
    cellular_fsm_t fsm;
    linkg_link_t  *link;
    uint64_t       now_ms;
    int            cleanup_ret;
    int            ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (path_enabled && (packet_pool == NULL || !config->enabled))
    {
        return -EINVAL;
    }

    ret = linkg_cellular_config_validate(config);
    if (ret != 0)
    {
        return ret;
    }

    now_ms = linkg_time_elapsed_ms();

    ret = cellular_fsm_init(&fsm, now_ms);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_cellular.lock);

    if (g_cellular.lifecycle != LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_cellular.lock);
        return -EALREADY;
    }

    g_cellular.lifecycle = LINKG_CELLULAR_LIFECYCLE_INITIALIZING;

    pthread_mutex_unlock(&g_cellular.lock);

    link = NULL;

    ret = cellular_monitor_init();
    if (ret != 0)
    {
        goto fail;
    }

    ret = cellular_status_init();
    if (ret != 0)
    {
        cellular_monitor_deinit();
        goto fail;
    }

    ret = cellular_health_init();
    if (ret != 0)
    {
        goto fail_modules;
    }

    if (path_enabled)
    {
        ret = _linkg_cellular_create_link(packet_pool, &link);
        if (ret != 0)
        {
            CELLULAR_ERROR("create cellular business link failed, error=%d", ret);
            goto fail_modules;
        }

        ret = linkg_link_manager_register(link);
        if (ret != 0)
        {
            CELLULAR_ERROR("register cellular business link failed, error=%d", ret);
            linkg_link_destroy(link);
            link = NULL;
            goto fail_modules;
        }
    }

    pthread_mutex_lock(&g_cellular.lock);

    g_cellular.config              = *config;
    g_cellular.fsm                 = fsm;
    g_cellular.link                = link;
    g_cellular.monitor_initialized = true;
    g_cellular.status_initialized  = true;
    g_cellular.health_initialized  = true;
    g_cellular.lifecycle           = LINKG_CELLULAR_LIFECYCLE_INITIALIZED;
    g_cellular.last_error          = 0;

    pthread_mutex_unlock(&g_cellular.lock);

    CELLULAR_INFO("module initialized, link_id=%u", link != NULL ? linkg_link_get_id(link) : LINKG_LINK_ID_INVALID);
    CELLULAR_DEBUG("config loaded, enabled=%d, network_mode=%d, apn=%s, pin_configured=%d", config->enabled ? 1 : 0, (int)config->network_mode, config->apn[0] != '\0' ? config->apn : "<auto>", config->pin[0] != '\0' ? 1 : 0);

    return 0;

fail_modules:
    cellular_health_deinit();
    cleanup_ret = cellular_status_deinit();
    if (cleanup_ret != 0)
    {
        CELLULAR_WARN("deinitialize cellular status after init failure failed, error=%d", cleanup_ret);
    }

    cellular_monitor_deinit();

fail:
    pthread_mutex_lock(&g_cellular.lock);
    _linkg_cellular_reset_context_locked();
    pthread_mutex_unlock(&g_cellular.lock);

    CELLULAR_WARN("module initialization failed, error=%d", ret);

    return ret;
}

/**
 * @brief 启动RG255模组并建立蜂窝连接状态机的运行环境。
 *
 * @note 本接口不等待SIM、网络注册、PDP或Host就绪，公网检测由后续健康模块负责。
 */
int linkg_cellular_start(void)
{
    at_channel_t *channel;
    linkg_link_t *link;
    int           cleanup_ret;
    int           ret;

    pthread_mutex_lock(&g_cellular.lock);

    if (g_cellular.lifecycle != LINKG_CELLULAR_LIFECYCLE_INITIALIZED)
    {
        if (g_cellular.lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED)
        {
            ret = -ENODEV;
        }
        else
        {
            ret = -EALREADY;
        }

        pthread_mutex_unlock(&g_cellular.lock);
        return ret;
    }

    g_cellular.lifecycle = LINKG_CELLULAR_LIFECYCLE_STARTING;
    link                 = g_cellular.link;

    pthread_mutex_unlock(&g_cellular.lock);

    CELLULAR_INFO("module starting");

    ret = cellular_modem_start(&g_cellular.config);
    if (ret != 0)
    {
        goto fail;
    }

    channel = cellular_modem_get_channel();
    if (channel == NULL)
    {
        ret = -EIO;
        goto fail_modem;
    }

    ret = cellular_monitor_start(channel);
    if (ret != 0)
    {
        goto fail_runtime;
    }

    g_cellular.monitor_started = true;
    CELLULAR_DEBUG("monitor started");

    ret = cellular_modem_enable_runtime_urcs();
    if (ret != 0)
    {
        goto fail_runtime;
    }

    ret = cellular_status_start(channel);
    if (ret != 0)
    {
        goto fail_runtime;
    }

    g_cellular.status_started = true;
    CELLULAR_DEBUG("status started");

    ret = cellular_health_start();
    if (ret != 0)
    {
        goto fail_runtime;
    }

    g_cellular.health_started = true;
    CELLULAR_DEBUG("health started");

    cellular_fsm_reset(&g_cellular.fsm, linkg_time_elapsed_ms());

    ret = cellular_fsm_enter(&g_cellular.fsm, CELLULAR_RUNTIME_STATE_WAIT_SIM, linkg_time_elapsed_ms());
    _linkg_cellular_publish_data_ready();
    if (ret != 0)
    {
        goto fail_runtime;
    }

    if (link != NULL)
    {
        ret = linkg_link_start(link);
        if (ret != 0)
        {
            goto fail_runtime;
        }
    }

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_RUNNING, 0);

    CELLULAR_INFO("module started");

    return 0;

fail_runtime:
    cleanup_ret = _linkg_cellular_stop_runtime();
    if (cleanup_ret != 0)
    {
        _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_FAILED, cleanup_ret);
        CELLULAR_ERROR("module start cleanup failed, start_error=%d, cleanup_error=%d", ret, cleanup_ret);
        return cleanup_ret;
    }

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_INITIALIZED, ret);
    CELLULAR_WARN("module start failed, error=%d", ret);

    return ret;

fail_modem:
    cellular_modem_stop();

fail:
    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_INITIALIZED, ret);
    CELLULAR_WARN("module start failed, error=%d", ret);

    return ret;
}

/**
 * @brief 在network-cell线程中运行蜂窝连接和维护状态机。
 *
 * @note 异常退出只记录模块失败；运行期资源由上层Network Worker统一回收。
 */
int linkg_cellular_run(linkg_thread_t *owner_thread)
{
    linkg_cellular_lifecycle_t lifecycle;
    int                       ret;

    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;
    pthread_mutex_unlock(&g_cellular.lock);

    if (lifecycle != LINKG_CELLULAR_LIFECYCLE_RUNNING)
    {
        return lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED ? -ENODEV : -ENETDOWN;
    }

    CELLULAR_DEBUG("owner loop started");

    ret = _linkg_cellular_owner_loop(owner_thread);
    if (!linkg_thread_is_running(owner_thread))
    {
        CELLULAR_DEBUG("owner loop stopped");
        return ret;
    }

    if (ret == 0)
    {
        ret = -EIO;
    }

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_FAILED, ret);
    CELLULAR_WARN("owner loop failed, error=%d", ret);

    return ret;
}

/**
 * @brief 停止蜂窝连接控制面及运行期资源。
 *
 * @note 由Network Worker在Owner运行循环退出后调用，不与FSM执行并发。
 */
int linkg_cellular_stop(void)
{
    linkg_cellular_lifecycle_t lifecycle;
    int                       ret;

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED ||
        lifecycle == LINKG_CELLULAR_LIFECYCLE_INITIALIZED)
    {
        pthread_mutex_unlock(&g_cellular.lock);
        return 0;
    }

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_INITIALIZING ||
        lifecycle == LINKG_CELLULAR_LIFECYCLE_STARTING ||
        lifecycle == LINKG_CELLULAR_LIFECYCLE_STOPPING)
    {
        pthread_mutex_unlock(&g_cellular.lock);
        return -EBUSY;
    }

    g_cellular.lifecycle               = LINKG_CELLULAR_LIFECYCLE_STOPPING;
    g_cellular.data_ready              = false;
    g_cellular.ipv4_internet_available = false;
    g_cellular.ipv6_internet_available = false;

    pthread_mutex_unlock(&g_cellular.lock);

    CELLULAR_INFO("module stopping");

    ret = _linkg_cellular_stop_runtime();
    if (ret != 0)
    {
        _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_FAILED, ret);
        CELLULAR_WARN("module stopped with cleanup error=%d", ret);
        return ret;
    }

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_INITIALIZED, 0);
    CELLULAR_INFO("module stopped");

    return 0;
}

/**
 * @brief 反初始化蜂窝模块并释放全部软件资源。
 *
 * @note 只有全部必要清理成功后才恢复UNINITIALIZED，失败时保留上下文以供重试。
 */
int linkg_cellular_deinit(void)
{
    linkg_cellular_lifecycle_t lifecycle;
    linkg_link_t              *link;
    int                        ret;

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;
    pthread_mutex_unlock(&g_cellular.lock);

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED)
    {
        return 0;
    }

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_INITIALIZING ||
        lifecycle == LINKG_CELLULAR_LIFECYCLE_STARTING ||
        lifecycle == LINKG_CELLULAR_LIFECYCLE_STOPPING)
    {
        return -EBUSY;
    }

    if (lifecycle != LINKG_CELLULAR_LIFECYCLE_INITIALIZED)
    {
        ret = linkg_cellular_stop();
        if (ret != 0)
        {
            return ret;
        }
    }

    pthread_mutex_lock(&g_cellular.lock);
    link = g_cellular.link;
    pthread_mutex_unlock(&g_cellular.lock);

    if (link != NULL)
    {
        ret = linkg_link_manager_unregister(link);
        if (ret != 0)
        {
            return ret;
        }

        linkg_link_destroy(link);

        pthread_mutex_lock(&g_cellular.lock);
        g_cellular.link = NULL;
        pthread_mutex_unlock(&g_cellular.lock);
    }

    if (g_cellular.health_initialized)
    {
        cellular_health_deinit();
        g_cellular.health_initialized = false;
    }

    if (g_cellular.status_initialized)
    {
        ret = cellular_status_deinit();
        if (ret != 0)
        {
            CELLULAR_WARN("deinitialize cellular status failed, error=%d", ret);
            return ret;
        }

        g_cellular.status_initialized = false;
    }

    if (g_cellular.monitor_initialized)
    {
        cellular_monitor_deinit();
        g_cellular.monitor_initialized = false;
    }

    pthread_mutex_lock(&g_cellular.lock);
    _linkg_cellular_reset_context_locked();
    pthread_mutex_unlock(&g_cellular.lock);

    CELLULAR_INFO("module deinitialized");

    return 0;
}

/****************************** 状态读取 ******************************/

/**
 * @brief 获取最新有效的统一蜂窝网络状态快照。
 */
int linkg_cellular_get_status(linkg_cellular_status_snapshot_t *snapshot)
{
    linkg_cellular_lifecycle_t lifecycle;
    bool                       enabled;
    int                        ret;

    if (snapshot == NULL)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;
    enabled   = g_cellular.config.enabled;
    pthread_mutex_unlock(&g_cellular.lock);

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED || !enabled)
    {
        return -ENODEV;
    }

    if (lifecycle != LINKG_CELLULAR_LIFECYCLE_RUNNING)
    {
        return -ENETDOWN;
    }

    ret = cellular_status_get_snapshot(snapshot);
    if (ret != 0)
    {
        return ret;
    }

    return 0;
}

/**
 * @brief 根据查询类型读取蜂窝数据通道或公网可用状态。
 *
 * @note DATA只表示FSM建链就绪，IPV4/IPV6由独立健康检测维护。
 *       本函数不执行任何网络探测。
 */
int linkg_cellular_get_internet_available(linkg_cellular_available_type_t type, bool *available)
{
    linkg_cellular_lifecycle_t lifecycle;

    if (available == NULL)
    {
        return -EINVAL;
    }

    *available = false;

    if (type != LINKG_CELLULAR_AVAILABLE_DATA &&
        type != LINKG_CELLULAR_AVAILABLE_IPV4 &&
        type != LINKG_CELLULAR_AVAILABLE_IPV6)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular.lock);

    lifecycle = g_cellular.lifecycle;

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_RUNNING && g_cellular.config.enabled)
    {
        switch (type)
        {
            case LINKG_CELLULAR_AVAILABLE_DATA:
                *available = g_cellular.data_ready;
                break;

            case LINKG_CELLULAR_AVAILABLE_IPV4:
                *available = g_cellular.ipv4_internet_available;
                break;

            case LINKG_CELLULAR_AVAILABLE_IPV6:
                *available = g_cellular.ipv6_internet_available;
                break;

            default:
                break;
        }
    }

    pthread_mutex_unlock(&g_cellular.lock);

    return lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED ? -ENODEV : 0;
}
