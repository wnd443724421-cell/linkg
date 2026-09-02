/**
 * @file linkg_cellular.c
 * @brief LinkG蜂窝网络运行控制实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-02
 */

#include "linkg_cellular.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_uart.h"

#include "at_channel.h"
#include "rg255_cmd.h"
#include "rg255_query.h"

#include "cellular_fsm.h"
#include "cellular_monitor.h"
#include "cellular_status.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_AT_DEVICE                   "/dev/ttyUSB1" // RG255 AT控制串口设备
#define LINKG_CELLULAR_AT_BAUDRATE                 115200         // RG255 AT串口波特率
#define LINKG_CELLULAR_AT_READY_TIMEOUT_MS         30000U         // AT通道整体就绪等待时间
#define LINKG_CELLULAR_AT_READY_RETRY_MS           500U           // AT通道就绪失败重试间隔
#define LINKG_CELLULAR_MODEM_RESTART_SETTLE_MS     2000U          // CFUN重启后首次重新探测等待时间
#define LINKG_CELLULAR_POLL_DESCRIPTOR_MAX         2U             // Owner循环最大poll描述符数量

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
    pthread_mutex_t            lock;                   // 模块状态锁，保护生命周期和AT通道引用

    linkg_cellular_config_t    config;                 // 蜂窝模块配置副本
    cellular_fsm_t             fsm;                    // network-cell Owner连接状态机

    at_channel_t              *channel;                // RG255 AT通道，模块持有对象所有权

    linkg_cellular_lifecycle_t lifecycle;              // 蜂窝模块生命周期
    int                        last_error;             // 最近一次不可恢复生命周期错误
    bool                       monitor_initialized;    // Monitor软件资源是否已经初始化
    bool                       status_initialized;     // Status软件资源是否已经初始化
    bool                       monitor_started;        // Monitor是否已经注册URC回调
    bool                       status_started;         // Status是否已经借用当前AT通道
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

    g_cellular.channel                = NULL;
    g_cellular.lifecycle              = LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED;
    g_cellular.last_error             = 0;
    g_cellular.monitor_initialized    = false;
    g_cellular.status_initialized     = false;
    g_cellular.monitor_started        = false;
    g_cellular.status_started         = false;
}

/**
 * @brief 设置蜂窝模块生命周期和对应错误码。
 */
static void _linkg_cellular_set_lifecycle(linkg_cellular_lifecycle_t lifecycle, int error)
{
    pthread_mutex_lock(&g_cellular.lock);
    g_cellular.lifecycle  = lifecycle;
    g_cellular.last_error = error;
    pthread_mutex_unlock(&g_cellular.lock);
}

/**
 * @brief 获取当前AT通道借用引用。
 */
static at_channel_t *_linkg_cellular_get_channel(void)
{
    at_channel_t *channel;

    pthread_mutex_lock(&g_cellular.lock);
    channel = g_cellular.channel;
    pthread_mutex_unlock(&g_cellular.lock);

    return channel;
}

/**
 * @brief 发布当前模块持有的AT通道对象。
 */
static void _linkg_cellular_set_channel(at_channel_t *channel)
{
    pthread_mutex_lock(&g_cellular.lock);
    g_cellular.channel = channel;
    pthread_mutex_unlock(&g_cellular.lock);
}

/**
 * @brief 销毁当前模块持有的AT通道对象。
 */
static void _linkg_cellular_destroy_channel(void)
{
    at_channel_t *channel;

    pthread_mutex_lock(&g_cellular.lock);
    channel            = g_cellular.channel;
    g_cellular.channel = NULL;
    pthread_mutex_unlock(&g_cellular.lock);

    if (channel != NULL)
    {
        at_channel_destroy(channel);
    }
}

/****************************** AT通道启动 ******************************/

/**
 * @brief 初始化RG255 AT控制串口参数。
 */
static void _linkg_cellular_make_uart_config(uart_config_t *config)
{
    memset(config, 0, sizeof(*config));

    config->baudrate        = LINKG_CELLULAR_AT_BAUDRATE;
    config->data_bits       = 8;
    config->stop_bits       = 1;
    config->parity          = 'N';
    config->hw_flow_control = false;
    config->exclusive       = true;
}

/**
 * @brief 对已经可通信的RG255应用基础AT运行参数。
 */
static int _linkg_cellular_apply_at_baseline(at_channel_t *channel)
{
    int ret;

    ret = rg255_cmd_set_echo(channel, false);
    if (ret != 0)
    {
        return ret;
    }

    ret = rg255_cmd_enable_cmee(channel);
    if (ret != 0)
    {
        return ret;
    }

    return rg255_cmd_disable_sleep(channel);
}

/**
 * @brief 创建AT通道并等待RG255进入可执行命令状态。
 */
static int _linkg_cellular_create_ready_channel(at_channel_t **out)
{
    uart_config_t config;
    at_channel_t *channel;
    uint64_t      started_ms;
    uint64_t      now_ms;
    int           error;
    int           ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out       = NULL;
    started_ms = linkg_time_elapsed_ms();
    error      = -ETIMEDOUT;

    _linkg_cellular_make_uart_config(&config);

    for (;;)
    {
        errno   = 0;
        channel = at_channel_create(LINKG_CELLULAR_AT_DEVICE, &config);
        if (channel == NULL)
        {
            error = errno != 0 ? -errno : -EIO;
        }
        else
        {
            ret = at_channel_start(channel);
            if (ret == 0)
            {
                ret = rg255_cmd_test(channel);
            }

            if (ret == 0)
            {
                ret = _linkg_cellular_apply_at_baseline(channel);
            }

            if (ret == 0)
            {
                *out = channel;
                return 0;
            }

            error = ret;
            at_channel_destroy(channel);
        }

        now_ms = linkg_time_elapsed_ms();
        if (now_ms - started_ms >= LINKG_CELLULAR_AT_READY_TIMEOUT_MS)
        {
            if (error != 0)
            {
                return error;
            }

            return -ETIMEDOUT;
        }

        ret = linkg_time_sleep_ms(LINKG_CELLULAR_AT_READY_RETRY_MS);
        if (ret != 0)
        {
            return ret;
        }
    }
}

/****************************** 持久配置 ******************************/

/**
 * @brief 返回项目硬件要求的SIM插入有效电平。
 */
static rg255_sim_insert_level_t _linkg_cellular_required_sim_insert_level(void)
{
    return LINKG_RESOURCE_CELLULAR_SIM_INSERT_ACTIVE_HIGH
        ? RG255_SIM_INSERT_LEVEL_HIGH
        : RG255_SIM_INSERT_LEVEL_LOW;
}

/**
 * @brief 查询并按需修正RG255全部持久运行配置。
 *
 * @note apply为false时只验证配置，不执行写入；任一配置不一致返回-EPROTO。
 */
static int _linkg_cellular_converge_persistent_config(at_channel_t *channel, bool apply, bool *changed)
{
    rg255_sim_detect_config_t      sim_detect;
    rg255_sim_status_urc_t         sim_urc;
    linkg_cellular_network_mode_t  network_mode;
    rg255_network_card_mode_t      card_mode;
    rg255_sim_insert_level_t       insert_level;
    rg255_usbnet_mode_t            usbnet_mode;
    bool                           local_changed;
    int                            ret;

    if (channel == NULL || changed == NULL)
    {
        return -EINVAL;
    }

    local_changed = false;
    insert_level  = _linkg_cellular_required_sim_insert_level();

    ret = rg255_query_usbnet_mode(channel, &usbnet_mode);
    if (ret != 0)
    {
        return ret;
    }

    if (usbnet_mode != RG255_USBNET_MODE_ECM)
    {
        if (!apply)
        {
            return -EPROTO;
        }

        ret = rg255_cmd_set_usbnet(channel, RG255_USBNET_MODE_ECM);
        if (ret != 0)
        {
            return ret;
        }

        local_changed = true;
    }

    ret = rg255_query_network_card_mode(channel, &card_mode);
    if (ret != 0)
    {
        return ret;
    }

    if (card_mode != RG255_NETWORK_CARD_MODE_NIC)
    {
        if (!apply)
        {
            return -EPROTO;
        }

        ret = rg255_cmd_set_network_card_mode(channel, RG255_NETWORK_CARD_MODE_NIC);
        if (ret != 0)
        {
            return ret;
        }

        local_changed = true;
    }

    ret = rg255_query_network_mode(channel, &network_mode);
    if (ret != 0)
    {
        return ret;
    }

    if (network_mode != g_cellular.config.network_mode)
    {
        if (!apply)
        {
            return -EPROTO;
        }

        ret = rg255_cmd_set_network_mode(channel, g_cellular.config.network_mode);
        if (ret != 0)
        {
            return ret;
        }

        local_changed = true;
    }

    memset(&sim_detect, 0, sizeof(sim_detect));
    ret = rg255_query_sim_detect(channel, &sim_detect);
    if (ret != 0)
    {
        return ret;
    }

    if (!sim_detect.enabled || sim_detect.insert_level != insert_level)
    {
        if (!apply)
        {
            return -EPROTO;
        }

        ret = rg255_cmd_set_sim_detect(channel, true, insert_level);
        if (ret != 0)
        {
            return ret;
        }

        local_changed = true;
    }

    memset(&sim_urc, 0, sizeof(sim_urc));
    ret = rg255_query_sim_status_urc(channel, &sim_urc);
    if (ret != 0)
    {
        return ret;
    }

    if (!sim_urc.enabled)
    {
        if (!apply)
        {
            return -EPROTO;
        }

        ret = rg255_cmd_set_sim_status_urc(channel, true);
        if (ret != 0)
        {
            return ret;
        }

        local_changed = true;
    }

    *changed = *changed || local_changed;

    return 0;
}

/**
 * @brief 完成持久配置收敛并在必要时只重启RG255一次。
 */
static int _linkg_cellular_prepare_persistent_config(at_channel_t **channel)
{
    at_channel_t *replacement;
    bool          changed;
    int           ret;

    if (channel == NULL || *channel == NULL)
    {
        return -EINVAL;
    }

    changed = false;

    ret = _linkg_cellular_converge_persistent_config(*channel, true, &changed);
    if (ret != 0)
    {
        return ret;
    }

    if (!changed)
    {
        return 0;
    }

    ret = rg255_cmd_restart(*channel);
    if (ret != 0)
    {
        return ret;
    }

    at_channel_destroy(*channel);
    *channel = NULL;

    ret = linkg_time_sleep_ms(LINKG_CELLULAR_MODEM_RESTART_SETTLE_MS);
    if (ret != 0)
    {
        return ret;
    }

    replacement = NULL;
    ret = _linkg_cellular_create_ready_channel(&replacement);
    if (ret != 0)
    {
        return ret;
    }

    changed = false;
    ret = _linkg_cellular_converge_persistent_config(replacement, false, &changed);
    if (ret != 0)
    {
        at_channel_destroy(replacement);
        return ret;
    }

    *channel = replacement;

    return 0;
}

/**
 * @brief 开启最终AT通道上的运行期异步状态上报。
 */
static int _linkg_cellular_enable_runtime_urcs(at_channel_t *channel)
{
    int ret;

    ret = rg255_cmd_set_eps_registration_urc(channel, true);
    if (ret != 0)
    {
        return ret;
    }

    ret = rg255_cmd_set_5g_registration_urc(channel, true);
    if (ret != 0)
    {
        return ret;
    }

    return rg255_cmd_set_signal_urc(channel, true);
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

    if ((events->mask & (CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED |
                         CELLULAR_MONITOR_EVENT_SIM_STATE_CHANGED)) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_SIM;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_REGISTRATION_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_REGISTRATION |
                   CELLULAR_STATUS_REFRESH_RADIO;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_RADIO_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_RADIO;
    }

    if ((events->mask & CELLULAR_MONITOR_EVENT_PDP_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_PDP |
                   CELLULAR_STATUS_REFRESH_PDP_ADDRESS;
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
    cellular_monitor_events_t     events;
    cellular_status_info_t        info;
    cellular_status_refresh_mask_t event_refresh;
    cellular_fsm_step_t            step;
    uint64_t                       now_ms;
    int                           session_result;
    int                           ret;

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
        if (session_result < 0)
        {
            return session_result;
        }

        if (session_result > 0)
        {
            continue;
        }

        step = cellular_fsm_run(&g_cellular.fsm, &g_cellular.config, _linkg_cellular_get_channel(), &info, now_ms);

        switch (step.result)
        {
            case CELLULAR_RUNTIME_STEP_DONE:
                ret = cellular_fsm_enter(&g_cellular.fsm, step.next_state, now_ms);
                if (ret != 0)
                {
                    return ret;
                }

                continue;

            case CELLULAR_RUNTIME_STEP_FAILED:
                ret = cellular_fsm_handle_failure(&g_cellular.fsm, _linkg_cellular_get_channel(), step.error, now_ms);
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

        ret = _linkg_cellular_poll_owner(owner_thread, linkg_time_elapsed_ms());
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** 运行资源回收 ******************************/

/**
 * @brief 按逆序停止蜂窝运行期资源。
 */
static int _linkg_cellular_stop_runtime(void)
{
    int first_error;
    int ret;

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

    _linkg_cellular_destroy_channel();
    cellular_fsm_reset(&g_cellular.fsm, linkg_time_elapsed_ms());

    return first_error;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝模块及其内部纯软件子模块。
 */
int linkg_cellular_init(const linkg_cellular_config_t *config)
{
    cellular_fsm_t fsm;
    uint64_t       now_ms;
    int            ret;

    if (config == NULL)
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

    pthread_mutex_lock(&g_cellular.lock);

    g_cellular.config              = *config;
    g_cellular.fsm                 = fsm;
    g_cellular.monitor_initialized = true;
    g_cellular.status_initialized  = true;
    g_cellular.lifecycle           = LINKG_CELLULAR_LIFECYCLE_INITIALIZED;
    g_cellular.last_error          = 0;

    pthread_mutex_unlock(&g_cellular.lock);

    return 0;

fail:
    pthread_mutex_lock(&g_cellular.lock);
    _linkg_cellular_reset_context_locked();
    pthread_mutex_unlock(&g_cellular.lock);

    return ret;
}

/**
 * @brief 启动AT通道、收敛一次性Modem配置并建立运行期监控。
 *
 * @note 本接口不等待SIM、网络注册、PDP、Host地址或公网验证完成。
 */
int linkg_cellular_start(void)
{
    at_channel_t *channel;
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

    pthread_mutex_unlock(&g_cellular.lock);

    channel = NULL;

    ret = _linkg_cellular_create_ready_channel(&channel);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_cellular_prepare_persistent_config(&channel);
    if (ret != 0)
    {
        goto fail_channel;
    }

    _linkg_cellular_set_channel(channel);
    channel = NULL;

    ret = cellular_monitor_start(_linkg_cellular_get_channel());
    if (ret != 0)
    {
        goto fail_runtime;
    }

    g_cellular.monitor_started = true;

    ret = _linkg_cellular_enable_runtime_urcs(_linkg_cellular_get_channel());
    if (ret != 0)
    {
        goto fail_runtime;
    }

    ret = cellular_status_start(_linkg_cellular_get_channel());
    if (ret != 0)
    {
        goto fail_runtime;
    }

    g_cellular.status_started = true;

    cellular_fsm_reset(&g_cellular.fsm, linkg_time_elapsed_ms());

    ret = cellular_fsm_enter(&g_cellular.fsm, CELLULAR_RUNTIME_STATE_WAIT_SIM, linkg_time_elapsed_ms());
    if (ret != 0)
    {
        goto fail_runtime;
    }

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_RUNNING, 0);

    return 0;

fail_runtime:
    cleanup_ret = _linkg_cellular_stop_runtime();
    if (ret == 0)
    {
        ret = cleanup_ret;
    }

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_INITIALIZED, ret);

    return ret;

fail_channel:
    at_channel_destroy(channel);

fail:
    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_INITIALIZED, ret);

    return ret;
}

/**
 * @brief 在network-cell线程中运行蜂窝连接和维护状态机。
 */
int linkg_cellular_run(linkg_thread_t *owner_thread)
{
    linkg_cellular_lifecycle_t lifecycle;
    int                        ret;

    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;
    pthread_mutex_unlock(&g_cellular.lock);

    if (lifecycle != LINKG_CELLULAR_LIFECYCLE_RUNNING)
    {
        if (lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED)
        {
            return -ENODEV;
        }

        return -ENETDOWN;
    }

    ret = _linkg_cellular_owner_loop(owner_thread);
    if (ret != 0 && linkg_thread_is_running(owner_thread))
    {
        _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_FAILED, ret);
    }

    return ret;
}

/**
 * @brief 停止蜂窝运行状态机及全部运行期资源。
 */
int linkg_cellular_stop(void)
{
    linkg_cellular_lifecycle_t lifecycle;
    int                        ret;

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_cellular.lock);
        return 0;
    }

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_INITIALIZED)
    {
        pthread_mutex_unlock(&g_cellular.lock);
        return 0;
    }

    g_cellular.lifecycle = LINKG_CELLULAR_LIFECYCLE_STOPPING;

    pthread_mutex_unlock(&g_cellular.lock);

    ret = _linkg_cellular_stop_runtime();

    _linkg_cellular_set_lifecycle(LINKG_CELLULAR_LIFECYCLE_INITIALIZED, ret);

    return ret;
}

/**
 * @brief 反初始化蜂窝模块并释放全部软件资源。
 */
int linkg_cellular_deinit(void)
{
    linkg_cellular_lifecycle_t lifecycle;
    int                        first_error;
    int                        ret;

    pthread_mutex_lock(&g_cellular.lock);
    lifecycle = g_cellular.lifecycle;
    pthread_mutex_unlock(&g_cellular.lock);

    if (lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED)
    {
        return 0;
    }

    first_error = 0;

    if (lifecycle != LINKG_CELLULAR_LIFECYCLE_INITIALIZED)
    {
        ret = linkg_cellular_stop();
        if (ret != 0)
        {
            _linkg_cellular_record_first_error(&first_error, ret);
        }
    }

    if (g_cellular.status_initialized)
    {
        ret = cellular_status_deinit();
        if (ret != 0)
        {
            _linkg_cellular_record_first_error(&first_error, ret);
        }
    }

    if (g_cellular.monitor_initialized)
    {
        cellular_monitor_deinit();
    }

    pthread_mutex_lock(&g_cellular.lock);
    _linkg_cellular_reset_context_locked();
    pthread_mutex_unlock(&g_cellular.lock);

    return first_error;
}
