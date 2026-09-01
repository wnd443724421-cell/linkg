/**
 * @file linkg_cellular.c
 * @brief LinkG蜂窝网络运行控制实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-01
 */

#include "linkg_cellular.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"
#include "linkg_uart.h"

#include "at_channel.h"
#include "rg255_cmd.h"
#include "rg255_query.h"
#include "rg255_runtime_urc.h"

#include "cellular_monitor.h"
#include "cellular_runtime.h"
#include "cellular_status.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_AT_DEVICE                         "/dev/ttyUSB1"  // RG255 AT控制串口设备
#define LINKG_CELLULAR_VERIFY_TARGET                     "www.baidu.com" // V1公网连通性验证目标
#define LINKG_CELLULAR_AT_BAUDRATE                       115200           // RG255 AT串口波特率
#define LINKG_CELLULAR_AT_READY_TIMEOUT_MS               30000U           // AT通道整体就绪等待时间
#define LINKG_CELLULAR_AT_READY_RETRY_MS                 500U             // AT通道就绪失败重试间隔
#define LINKG_CELLULAR_MODEM_RESTART_SETTLE_MS           2000U            // CFUN重启后首次重新探测等待时间
#define LINKG_CELLULAR_CHECK_SIM_TIMEOUT_MS              15000U           // SIM初始化状态收敛超时
#define LINKG_CELLULAR_REGISTRATION_TIMEOUT_MS           120000U          // 移动网络注册等待超时
#define LINKG_CELLULAR_PDP_TIMEOUT_MS                    30000U           // PDP激活确认等待超时
#define LINKG_CELLULAR_NETDEV_TIMEOUT_MS                 30000U           // USB网络设备连接等待超时
#define LINKG_CELLULAR_HOST_TIMEOUT_MS                   30000U           // Linux Host网络配置收敛超时
#define LINKG_CELLULAR_VERIFY_TIMEOUT_MS                 30000U           // 公网连通性验证整体超时
#define LINKG_CELLULAR_VERIFY_RETRY_MS                   2000U            // 单次公网探测失败后的重试间隔
#define LINKG_CELLULAR_VERIFY_NEXT_FAMILY_DELAY_MS       1U               // IPv4成功后进入IPv6验证的调度间隔
#define LINKG_CELLULAR_VERIFY_FAILURE_LIMIT              3U               // 单个地址族连续公网探测失败阈值
#define LINKG_CELLULAR_ONLINE_VERIFY_INTERVAL_MS         30000U           // 在线状态公网复核周期
#define LINKG_CELLULAR_RETRY_BASE_MS                     5000U            // 连接失败首次退避时间
#define LINKG_CELLULAR_RETRY_MAX_MS                      60000U           // 连接失败最大退避时间
#define LINKG_CELLULAR_POLL_DESCRIPTOR_MAX               2U               // Owner循环最大poll描述符数量

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
    cellular_runtime_step_result_t result;     // 当前状态处理结果
    cellular_runtime_state_t       next_state; // DONE时需要进入的后续状态
    int                            error;       // FAILED或FATAL时的错误码
} linkg_cellular_step_t;

typedef struct
{
    pthread_mutex_t             lock;                  // 模块状态锁，保护生命周期和AT通道引用
    linkg_cellular_config_t      config;                // 蜂窝模块配置副本
    cellular_runtime_t           runtime;               // network-cell Owner唯一运行状态
    at_channel_t                *channel;               // RG255 AT通道，模块持有对象所有权
    cellular_status_refresh_mask_t requested_refresh;   // Owner下一轮需要强制确认的状态事实
    linkg_cellular_lifecycle_t   lifecycle;             // 蜂窝模块生命周期
    int                          last_error;             // 最近一次不可恢复生命周期错误
    bool                         monitor_initialized;    // Monitor软件资源是否已经初始化
    bool                         status_initialized;     // Status软件资源是否已经初始化
    bool                         monitor_started;        // Monitor是否已经注册URC回调
    bool                         status_started;         // Status是否已经借用当前AT通道
    bool                         pdp_action_started;     // 当前SIM会话是否执行过PDP激活动作
    bool                         netdev_action_started;  // 当前SIM会话是否执行过QNETDEV启动动作
    bool                         verify_ipv4_done;       // 当前验证轮次IPv4是否已经通过
    bool                         verify_ipv6_done;       // 当前验证轮次IPv6是否已经通过
    uint32_t                     verify_failure_count;   // 当前地址族连续公网探测失败次数
} linkg_cellular_context_t;

/****************************** 全局上下文 ******************************/

static linkg_cellular_context_t g_cellular =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,            // 模块状态锁静态初始化
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
 * @brief 判断事实元数据是否表示最近一次确认成功。
 */
static bool _linkg_cellular_meta_current(const cellular_status_meta_t *meta)
{
    return meta != NULL && meta->confirmed && meta->last_error == 0;
}

/**
 * @brief 判断AT动作错误是否表示控制基础设施已经无法继续运行。
 */
static bool _linkg_cellular_action_error_fatal(int error)
{
    switch (error)
    {
        case -EINVAL:
        case -EMSGSIZE:
        case -EDEADLK:
        case -EBADF:
        case -ENODEV:
        case -ECANCELED:
            return true;

        default:
            return false;
    }
}

/**
 * @brief 清空当前公网验证轮次状态。
 */
static void _linkg_cellular_reset_verify(void)
{
    g_cellular.verify_ipv4_done     = false;
    g_cellular.verify_ipv6_done     = false;
    g_cellular.verify_failure_count = 0U;
}

/**
 * @brief 清空模块上下文动态字段并恢复未初始化生命周期。
 *
 * @note 调用方必须持有g_cellular.lock，且全部子模块资源已经释放。
 */
static void _linkg_cellular_reset_context_locked(void)
{
    memset(&g_cellular.config, 0, sizeof(g_cellular.config));
    memset(&g_cellular.runtime, 0, sizeof(g_cellular.runtime));

    g_cellular.channel                = NULL;
    g_cellular.requested_refresh      = CELLULAR_STATUS_REFRESH_NONE;
    g_cellular.lifecycle              = LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED;
    g_cellular.last_error             = 0;
    g_cellular.monitor_initialized    = false;
    g_cellular.status_initialized     = false;
    g_cellular.monitor_started        = false;
    g_cellular.status_started         = false;
    g_cellular.pdp_action_started     = false;
    g_cellular.netdev_action_started  = false;
    g_cellular.verify_ipv4_done       = false;
    g_cellular.verify_ipv6_done       = false;
    g_cellular.verify_failure_count   = 0U;
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

/****************************** 状态结果辅助 ******************************/

/**
 * @brief 构造保持当前运行状态的处理结果。
 */
static linkg_cellular_step_t _linkg_cellular_step_wait(void)
{
    linkg_cellular_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_WAIT;

    return step;
}

/**
 * @brief 构造推进到指定运行状态的处理结果。
 */
static linkg_cellular_step_t _linkg_cellular_step_done(cellular_runtime_state_t next_state)
{
    linkg_cellular_step_t step;

    memset(&step, 0, sizeof(step));
    step.result     = CELLULAR_RUNTIME_STEP_DONE;
    step.next_state = next_state;

    return step;
}

/**
 * @brief 构造交由Owner失败策略处理的状态结果。
 */
static linkg_cellular_step_t _linkg_cellular_step_failed(int error)
{
    linkg_cellular_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_FAILED;
    step.error  = error < 0 ? error : -EIO;

    return step;
}

/**
 * @brief 构造要求Owner结束运行的致命状态结果。
 */
static linkg_cellular_step_t _linkg_cellular_step_fatal(int error)
{
    linkg_cellular_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_FATAL;
    step.error  = error < 0 ? error : -EIO;

    return step;
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
            return error != 0 ? error : -ETIMEDOUT;
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
 * @brief 取得并清空Owner下一轮内部强制刷新请求。
 */
static cellular_status_refresh_mask_t _linkg_cellular_take_requested_refresh(void)
{
    cellular_status_refresh_mask_t requested;

    requested                    = g_cellular.requested_refresh;
    g_cellular.requested_refresh = CELLULAR_STATUS_REFRESH_NONE;

    return requested;
}

/**
 * @brief 安排Owner下一轮定向确认指定状态事实。
 */
static void _linkg_cellular_request_refresh(cellular_status_refresh_mask_t requested)
{
    g_cellular.requested_refresh |= requested;
}

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

    if ((events->mask & CELLULAR_MONITOR_EVENT_MODEM_FUNCTION_CHANGED) != 0U)
    {
        refresh |= CELLULAR_STATUS_REFRESH_ALL;
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

    requested = event_refresh | _linkg_cellular_take_requested_refresh();

    ret = cellular_status_process(now_ms, requested);
    if (ret != 0)
    {
        return ret;
    }

    return cellular_status_get_info(info);
}

/****************************** 运行状态控制 ******************************/

/**
 * @brief 获取指定运行状态的整体等待超时。
 */
static uint64_t _linkg_cellular_state_timeout(cellular_runtime_state_t state)
{
    switch (state)
    {
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            return LINKG_CELLULAR_CHECK_SIM_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            return LINKG_CELLULAR_REGISTRATION_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            return LINKG_CELLULAR_PDP_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            return LINKG_CELLULAR_NETDEV_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            return LINKG_CELLULAR_HOST_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
            return LINKG_CELLULAR_VERIFY_TIMEOUT_MS;

        default:
            return 0U;
    }
}

/**
 * @brief 为新进入的状态安排立即需要确认的事实。
 */
static void _linkg_cellular_request_state_refresh(cellular_runtime_state_t state)
{
    switch (state)
    {
        case CELLULAR_RUNTIME_STATE_WAIT_SIM:
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
        case CELLULAR_RUNTIME_STATE_WAIT_PIN:
        case CELLULAR_RUNTIME_STATE_WAIT_PUK:
            _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_SIM);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_REGISTRATION |
                                            CELLULAR_STATUS_REFRESH_RADIO);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_PDP |
                                            CELLULAR_STATUS_REFRESH_PDP_ADDRESS);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_NETDEV |
                                            CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                            CELLULAR_STATUS_REFRESH_HOST);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_PDP_ADDRESS |
                                            CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                            CELLULAR_STATUS_REFRESH_HOST);
            break;

        case CELLULAR_RUNTIME_STATE_ONLINE:
            _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_SIM |
                                            CELLULAR_STATUS_REFRESH_REGISTRATION |
                                            CELLULAR_STATUS_REFRESH_PDP |
                                            CELLULAR_STATUS_REFRESH_NETDEV |
                                            CELLULAR_STATUS_REFRESH_HOST);
            break;

        default:
            break;
    }
}

/**
 * @brief 显式进入指定运行状态并建立该状态的时间边界。
 */
static int _linkg_cellular_enter_state(cellular_runtime_state_t state, uint64_t now_ms)
{
    cellular_runtime_state_t previous;
    uint64_t                 timeout_ms;
    int                      ret;

    previous   = g_cellular.runtime.state;
    timeout_ms = _linkg_cellular_state_timeout(state);

    ret = cellular_runtime_enter(&g_cellular.runtime, state, now_ms, timeout_ms);
    if (ret != 0)
    {
        return ret;
    }

    if (state == CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY)
    {
        _linkg_cellular_reset_verify();
    }

    if (state == CELLULAR_RUNTIME_STATE_ONLINE)
    {
        cellular_runtime_clear_failure(&g_cellular.runtime, now_ms);
        ret = cellular_runtime_schedule_action(&g_cellular.runtime,
                                               now_ms,
                                               LINKG_CELLULAR_ONLINE_VERIFY_INTERVAL_MS);
        if (ret != 0)
        {
            return ret;
        }
    }

    _linkg_cellular_request_state_refresh(state);

    if (previous != state)
    {
        LINKG_LOG_INFO("CELLULAR: runtime state changed, old=%s, new=%s",
                       cellular_runtime_state_name(previous),
                       cellular_runtime_state_name(state));
    }

    return 0;
}

/****************************** 数据会话清理 ******************************/

/**
 * @brief 按QNETDEV到PDP的逆序尽力停止当前数据会话。
 *
 * @note 本函数不结束SIM插卡会话；连接失败重试时保留SIM会话和PIN保护。
 */
static int _linkg_cellular_cleanup_data_session(void)
{
    at_channel_t *channel;
    int           first_error;
    int           ret;

    channel = _linkg_cellular_get_channel();
    if (channel == NULL)
    {
        return -ENODEV;
    }

    first_error = 0;

    if (g_cellular.netdev_action_started)
    {
        ret = rg255_cmd_stop_netdev(channel);
        if (ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: stop QNETDEV failed, error=%d", ret);
            _linkg_cellular_record_first_error(&first_error, ret);
        }
    }

    if (g_cellular.pdp_action_started)
    {
        ret = rg255_cmd_set_pdp_active(channel, false);
        if (ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: deactivate PDP failed, error=%d", ret);
            _linkg_cellular_record_first_error(&first_error, ret);
        }
    }

    _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_PDP |
                                    CELLULAR_STATUS_REFRESH_PDP_ADDRESS |
                                    CELLULAR_STATUS_REFRESH_NETDEV |
                                    CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                    CELLULAR_STATUS_REFRESH_HOST);

    _linkg_cellular_reset_verify();

    return first_error;
}

/**
 * @brief 处理SIM拔出并回到等待新插卡状态。
 */
static int _linkg_cellular_handle_sim_removed(uint64_t now_ms)
{
    int cleanup_ret;
    int ret;

    cleanup_ret = 0;

    if (cellular_runtime_session_active(&g_cellular.runtime))
    {
        cleanup_ret = _linkg_cellular_cleanup_data_session();
        cellular_runtime_end_session(&g_cellular.runtime, now_ms);
    }

    g_cellular.pdp_action_started    = false;
    g_cellular.netdev_action_started = false;

    if (g_cellular.runtime.state != CELLULAR_RUNTIME_STATE_WAIT_SIM)
    {
        ret = _linkg_cellular_enter_state(CELLULAR_RUNTIME_STATE_WAIT_SIM, now_ms);
        if (ret != 0)
        {
            return ret;
        }
    }

    return cleanup_ret;
}

/****************************** Host事实校验 ******************************/

/**
 * @brief 判断IPv6地址是否属于指定网络前缀。
 */
static bool _linkg_cellular_ipv6_in_prefix(const struct in6_addr *address, const struct in6_addr *prefix, uint8_t prefix_length)
{
    uint8_t mask;
    size_t  full_bytes;
    uint8_t remaining_bits;

    if (address == NULL || prefix == NULL || prefix_length == 0U || prefix_length > 128U)
    {
        return false;
    }

    full_bytes     = prefix_length / 8U;
    remaining_bits = prefix_length % 8U;

    if (full_bytes > 0U && memcmp(address->s6_addr, prefix->s6_addr, full_bytes) != 0)
    {
        return false;
    }

    if (remaining_bits == 0U)
    {
        return true;
    }

    mask = (uint8_t)(0xffU << (8U - remaining_bits));

    return (address->s6_addr[full_bytes] & mask) ==
           (prefix->s6_addr[full_bytes] & mask);
}

/**
 * @brief 判断Linux Host网络事实是否完整匹配RG255期望配置。
 */
static bool _linkg_cellular_host_ready(const cellular_status_info_t *info)
{
    const cellular_status_host_info_t          *host;
    const cellular_status_expected_ipv4_info_t *expected_ipv4;
    const cellular_status_expected_ipv6_info_t *expected_ipv6;

    if (info == NULL)
    {
        return false;
    }

    host          = &info->host;
    expected_ipv4 = &info->netdev.expected_ipv4;
    expected_ipv6 = &info->netdev.expected_ipv6;

    if (!_linkg_cellular_meta_current(&info->pdp.address_meta) ||
        !info->pdp.global_ipv6_valid)
    {
        return false;
    }

    if (!_linkg_cellular_meta_current(&expected_ipv4->meta) || !expected_ipv4->valid ||
        !_linkg_cellular_meta_current(&expected_ipv6->meta) || !expected_ipv6->valid)
    {
        return false;
    }

    if (!_linkg_cellular_meta_current(&host->interface_meta) || !host->interface_present ||
        !_linkg_cellular_meta_current(&host->interface_up_meta) || !host->interface_up)
    {
        return false;
    }

    if (!_linkg_cellular_meta_current(&host->ipv4_meta) || !host->ipv4_valid ||
        !_linkg_cellular_meta_current(&host->ipv4_netmask_meta) || !host->ipv4_netmask_valid ||
        !_linkg_cellular_meta_current(&host->ipv4_route_meta) || !host->ipv4_gateway_valid)
    {
        return false;
    }

    if (memcmp(&host->ipv4, &expected_ipv4->address, sizeof(host->ipv4)) != 0 ||
        memcmp(&host->ipv4_netmask, &expected_ipv4->netmask, sizeof(host->ipv4_netmask)) != 0 ||
        memcmp(&host->ipv4_gateway, &expected_ipv4->gateway, sizeof(host->ipv4_gateway)) != 0)
    {
        return false;
    }

    if (!_linkg_cellular_meta_current(&host->ipv6_meta) || !host->global_ipv6_valid ||
        !_linkg_cellular_meta_current(&host->ipv6_route_meta) || !host->ipv6_gateway_valid)
    {
        return false;
    }

    if (!_linkg_cellular_ipv6_in_prefix(&host->global_ipv6,
                                        &expected_ipv6->prefix,
                                        expected_ipv6->prefix_length))
    {
        return false;
    }

    return memcmp(&host->ipv6_gateway,
                  &expected_ipv6->gateway,
                  sizeof(host->ipv6_gateway)) == 0;
}

/**
 * @brief 判断在线状态是否已经被最新成功事实明确否定。
 */
static bool _linkg_cellular_online_invalid(const cellular_status_info_t *info)
{
    if (info == NULL)
    {
        return true;
    }

    if (_linkg_cellular_meta_current(&info->local.sim_meta) &&
        info->local.sim_state != LINKG_CELLULAR_SIM_STATE_READY)
    {
        return true;
    }

    if (_linkg_cellular_meta_current(&info->network.registration_meta) &&
        info->network.registration != LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
    {
        return true;
    }

    if (_linkg_cellular_meta_current(&info->pdp.active_meta) && !info->pdp.active)
    {
        return true;
    }

    if (_linkg_cellular_meta_current(&info->netdev.state.meta) &&
        !info->netdev.state.connected)
    {
        return true;
    }

    if (_linkg_cellular_meta_current(&info->host.interface_meta) &&
        _linkg_cellular_meta_current(&info->host.interface_up_meta) &&
        _linkg_cellular_meta_current(&info->host.ipv4_meta) &&
        _linkg_cellular_meta_current(&info->host.ipv4_netmask_meta) &&
        _linkg_cellular_meta_current(&info->host.ipv6_meta) &&
        _linkg_cellular_meta_current(&info->host.ipv4_route_meta) &&
        _linkg_cellular_meta_current(&info->host.ipv6_route_meta) &&
        _linkg_cellular_meta_current(&info->netdev.expected_ipv4.meta) &&
        _linkg_cellular_meta_current(&info->netdev.expected_ipv6.meta) &&
        !_linkg_cellular_host_ready(info))
    {
        return true;
    }

    return false;
}

/****************************** SIM会话协调 ******************************/

/**
 * @brief 根据最新SIM事实开始或结束物理插卡会话。
 *
 * @return 1表示本轮发生了强制状态转移，0表示未转移，负值表示错误。
 */
static int _linkg_cellular_sync_sim_session(const cellular_monitor_events_t *events, const cellular_status_info_t *info, uint64_t now_ms)
{
    linkg_cellular_sim_state_t sim_state;
    bool                       physical_removed;
    int                        ret;

    physical_removed = events != NULL &&
                       (events->mask & CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED) != 0U &&
                       events->sim_presence_valid &&
                       events->sim_presence == CELLULAR_MONITOR_SIM_PRESENCE_REMOVED;

    sim_state = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    if (info != NULL && _linkg_cellular_meta_current(&info->local.sim_meta))
    {
        sim_state = info->local.sim_state;
    }

    if (physical_removed || sim_state == LINKG_CELLULAR_SIM_STATE_ABSENT)
    {
        ret = _linkg_cellular_handle_sim_removed(now_ms);
        if (ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: cleanup after SIM removal returned error=%d", ret);
        }

        return 1;
    }

    if (!cellular_runtime_session_active(&g_cellular.runtime) &&
        sim_state != LINKG_CELLULAR_SIM_STATE_UNKNOWN)
    {
        ret = cellular_runtime_begin_session(&g_cellular.runtime, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        g_cellular.pdp_action_started    = false;
        g_cellular.netdev_action_started = false;

        ret = _linkg_cellular_enter_state(CELLULAR_RUNTIME_STATE_CHECK_SIM, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        return 1;
    }

    if (cellular_runtime_session_active(&g_cellular.runtime) &&
        sim_state != LINKG_CELLULAR_SIM_STATE_UNKNOWN &&
        sim_state != LINKG_CELLULAR_SIM_STATE_READY)
    {
        switch (g_cellular.runtime.state)
        {
            case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            case CELLULAR_RUNTIME_STATE_WAIT_PIN:
            case CELLULAR_RUNTIME_STATE_WAIT_PUK:
                break;

            default:
                ret = _linkg_cellular_enter_state(CELLULAR_RUNTIME_STATE_CHECK_SIM, now_ms);
                if (ret != 0)
                {
                    return ret;
                }

                return 1;
        }
    }

    return 0;
}

/****************************** 状态步骤 ******************************/

/**
 * @brief 处理IDLE状态并进入SIM等待流程。
 */
static linkg_cellular_step_t _linkg_cellular_state_idle(void)
{
    return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_SIM);
}

/**
 * @brief 处理等待SIM插入状态。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_sim(void)
{
    if (cellular_runtime_session_active(&g_cellular.runtime))
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_CHECK_SIM);
    }

    return _linkg_cellular_step_wait();
}

/**
 * @brief 根据CPIN事实决定SIM会话后续流程。
 */
static linkg_cellular_step_t _linkg_cellular_state_check_sim(const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info == NULL || !_linkg_cellular_meta_current(&info->local.sim_meta))
    {
        if (cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
        {
            error = info != NULL && info->local.sim_meta.last_error != 0
                ? info->local.sim_meta.last_error
                : -ETIMEDOUT;

            return _linkg_cellular_step_failed(error);
        }

        return _linkg_cellular_step_wait();
    }

    switch (info->local.sim_state)
    {
        case LINKG_CELLULAR_SIM_STATE_READY:
            return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);

        case LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED:
            if (g_cellular.config.pin[0] == '\0' || cellular_runtime_pin_attempted(&g_cellular.runtime))
            {
                return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
            }

            return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_ENTER_PIN);

        case LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED:
            return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PUK);

        case LINKG_CELLULAR_SIM_STATE_ABSENT:
            return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_SIM);

        case LINKG_CELLULAR_SIM_STATE_NOT_READY:
        case LINKG_CELLULAR_SIM_STATE_UNKNOWN:
        default:
            if (cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
            {
                return _linkg_cellular_step_failed(-ETIMEDOUT);
            }

            return _linkg_cellular_step_wait();
    }
}

/**
 * @brief 在当前物理插卡会话中安全执行一次用户PIN输入。
 */
static linkg_cellular_step_t _linkg_cellular_state_enter_pin(uint64_t now_ms)
{
    at_channel_t *channel;
    int           ret;

    if (g_cellular.config.pin[0] == '\0')
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
    }

    ret = cellular_runtime_mark_pin_attempted(&g_cellular.runtime, now_ms);
    if (ret != 0)
    {
        if (ret == -EALREADY)
        {
            return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
        }

        return _linkg_cellular_step_fatal(ret);
    }

    ret = cellular_runtime_note_attempt(&g_cellular.runtime, now_ms);
    if (ret != 0)
    {
        return _linkg_cellular_step_fatal(ret);
    }

    channel = _linkg_cellular_get_channel();
    if (channel == NULL)
    {
        return _linkg_cellular_step_fatal(-ENODEV);
    }

    ret = rg255_cmd_enter_pin(channel, g_cellular.config.pin);
    if (ret != 0)
    {
        if (_linkg_cellular_action_error_fatal(ret))
        {
            return _linkg_cellular_step_fatal(ret);
        }

        LINKG_LOG_WARN("CELLULAR: enter SIM PIN returned error=%d, action=query-truth", ret);
    }

    _linkg_cellular_request_refresh(CELLULAR_STATUS_REFRESH_SIM);

    return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_CHECK_SIM);
}

/**
 * @brief 处理等待用户提供正确PIN或更换SIM状态。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_pin(const cellular_status_info_t *info)
{
    if (info == NULL || !_linkg_cellular_meta_current(&info->local.sim_meta))
    {
        return _linkg_cellular_step_wait();
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_READY)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PUK);
    }

    return _linkg_cellular_step_wait();
}

/**
 * @brief 处理等待用户人工解除SIM PUK状态。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_puk(const cellular_status_info_t *info)
{
    if (info == NULL || !_linkg_cellular_meta_current(&info->local.sim_meta))
    {
        return _linkg_cellular_step_wait();
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_READY)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
    }

    return _linkg_cellular_step_wait();
}

/**
 * @brief 等待移动网络注册成功。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_registration(const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info != NULL && _linkg_cellular_meta_current(&info->network.registration_meta))
    {
        if (info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
        {
            return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_PREPARE_PDP);
        }

        if (info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_DENIED)
        {
            return _linkg_cellular_step_failed(-EACCES);
        }
    }

    if (!cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_wait();
    }

    error = info != NULL && info->network.registration_meta.last_error != 0
        ? info->network.registration_meta.last_error
        : -ETIMEDOUT;

    return _linkg_cellular_step_failed(error);
}

/**
 * @brief 查询并按需准备默认IPv4/IPv6双栈PDP上下文。
 */
static linkg_cellular_step_t _linkg_cellular_state_prepare_pdp(uint64_t now_ms)
{
    rg255_pdp_config_t current;
    at_channel_t      *channel;
    const char        *required_apn;
    bool               matches;
    int                ret;

    channel = _linkg_cellular_get_channel();
    if (channel == NULL)
    {
        return _linkg_cellular_step_fatal(-ENODEV);
    }

    ret = cellular_runtime_note_attempt(&g_cellular.runtime, now_ms);
    if (ret != 0)
    {
        return _linkg_cellular_step_fatal(ret);
    }

    memset(&current, 0, sizeof(current));
    ret = rg255_query_pdp_config(channel, &current);
    if (ret != 0)
    {
        return _linkg_cellular_action_error_fatal(ret)
            ? _linkg_cellular_step_fatal(ret)
            : _linkg_cellular_step_failed(ret);
    }

    matches = current.cid == RG255_PDP_CONTEXT_ID &&
              current.pdp_type == RG255_PDP_TYPE_IPV4V6;

    if (g_cellular.config.apn[0] != '\0')
    {
        matches = matches && strcmp(current.apn, g_cellular.config.apn) == 0;
    }

    if (matches)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_ACTIVATE_PDP);
    }

    required_apn = g_cellular.config.apn[0] != '\0'
        ? g_cellular.config.apn
        : current.apn;

    if (required_apn[0] == '\0')
    {
        return _linkg_cellular_step_failed(-ENOENT);
    }

    ret = rg255_cmd_set_pdp_context(channel, required_apn);
    if (ret != 0)
    {
        return _linkg_cellular_action_error_fatal(ret)
            ? _linkg_cellular_step_fatal(ret)
            : _linkg_cellular_step_failed(ret);
    }

    return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_ACTIVATE_PDP);
}

/**
 * @brief 请求激活默认PDP上下文并转入事实确认状态。
 */
static linkg_cellular_step_t _linkg_cellular_state_activate_pdp(uint64_t now_ms)
{
    at_channel_t *channel;
    int           ret;

    channel = _linkg_cellular_get_channel();
    if (channel == NULL)
    {
        return _linkg_cellular_step_fatal(-ENODEV);
    }

    ret = cellular_runtime_note_attempt(&g_cellular.runtime, now_ms);
    if (ret != 0)
    {
        return _linkg_cellular_step_fatal(ret);
    }

    g_cellular.pdp_action_started = true;

    ret = rg255_cmd_set_pdp_active(channel, true);
    if (ret != 0)
    {
        if (_linkg_cellular_action_error_fatal(ret))
        {
            return _linkg_cellular_step_fatal(ret);
        }

        LINKG_LOG_WARN("CELLULAR: activate PDP returned error=%d, action=query-truth", ret);
    }

    return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_PDP);
}

/**
 * @brief 等待Status确认默认PDP上下文已经激活。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_pdp(const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info != NULL && _linkg_cellular_meta_current(&info->pdp.active_meta) && info->pdp.active)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_START_NETDEV);
    }

    if (!cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_wait();
    }

    error = info != NULL && info->pdp.active_meta.last_error != 0
        ? info->pdp.active_meta.last_error
        : -ETIMEDOUT;

    return _linkg_cellular_step_failed(error);
}

/**
 * @brief 请求启动RG255 USB网络设备连接并转入事实确认状态。
 */
static linkg_cellular_step_t _linkg_cellular_state_start_netdev(uint64_t now_ms)
{
    at_channel_t *channel;
    int           ret;

    channel = _linkg_cellular_get_channel();
    if (channel == NULL)
    {
        return _linkg_cellular_step_fatal(-ENODEV);
    }

    ret = cellular_runtime_note_attempt(&g_cellular.runtime, now_ms);
    if (ret != 0)
    {
        return _linkg_cellular_step_fatal(ret);
    }

    g_cellular.netdev_action_started = true;

    ret = rg255_cmd_start_netdev(channel);
    if (ret != 0)
    {
        if (_linkg_cellular_action_error_fatal(ret))
        {
            return _linkg_cellular_step_fatal(ret);
        }

        LINKG_LOG_WARN("CELLULAR: start QNETDEV returned error=%d, action=query-truth", ret);
    }

    return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_NETDEV);
}

/**
 * @brief 等待Status确认USB网络设备已经连接。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_netdev(const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info != NULL && _linkg_cellular_meta_current(&info->netdev.state.meta) &&
        info->netdev.state.connected)
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_WAIT_HOST);
    }

    if (!cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_wait();
    }

    error = info != NULL && info->netdev.state.meta.last_error != 0
        ? info->netdev.state.meta.last_error
        : -ETIMEDOUT;

    return _linkg_cellular_step_failed(error);
}

/**
 * @brief 等待Linux Host网络配置完整匹配RG255期望事实。
 */
static linkg_cellular_step_t _linkg_cellular_state_wait_host(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (_linkg_cellular_host_ready(info))
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY);
    }

    if (cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_failed(-ETIMEDOUT);
    }

    return _linkg_cellular_step_wait();
}

/**
 * @brief 通过指定地址族和usb0接口执行一次公网连通性探测。
 */
static int _linkg_cellular_probe_connectivity(bool ipv6)
{
    if (ipv6)
    {
        return linkg_os_run("ping6",
                            "-I",
                            LINKG_RESOURCE_INTERFACE_CELLULAR,
                            "-c",
                            "1",
                            "-W",
                            "2",
                            LINKG_CELLULAR_VERIFY_TARGET,
                            NULL);
    }

    return linkg_os_run("ping",
                        "-4",
                        "-I",
                        LINKG_RESOURCE_INTERFACE_CELLULAR,
                        "-c",
                        "1",
                        "-W",
                        "2",
                        LINKG_CELLULAR_VERIFY_TARGET,
                        NULL);
}

/**
 * @brief 顺序验证IPv4和IPv6真实公网连通性。
 */
static linkg_cellular_step_t _linkg_cellular_state_verify(const cellular_status_info_t *info, uint64_t now_ms)
{
    bool ipv6;
    int  ret;

    if (!_linkg_cellular_host_ready(info))
    {
        return _linkg_cellular_step_failed(-ENETDOWN);
    }

    if (g_cellular.runtime.next_action_ms != 0U &&
        !cellular_runtime_action_due(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_wait();
    }

    cellular_runtime_clear_action(&g_cellular.runtime, now_ms);

    ipv6 = g_cellular.verify_ipv4_done;

    ret = cellular_runtime_note_attempt(&g_cellular.runtime, now_ms);
    if (ret != 0)
    {
        return _linkg_cellular_step_fatal(ret);
    }

    ret = _linkg_cellular_probe_connectivity(ipv6);
    if (ret == 0)
    {
        g_cellular.verify_failure_count = 0U;

        if (!ipv6)
        {
            g_cellular.verify_ipv4_done = true;

            ret = cellular_runtime_schedule_action(&g_cellular.runtime,
                                                   now_ms,
                                                   LINKG_CELLULAR_VERIFY_NEXT_FAMILY_DELAY_MS);
            if (ret != 0)
            {
                return _linkg_cellular_step_fatal(ret);
            }

            return _linkg_cellular_step_wait();
        }

        g_cellular.verify_ipv6_done = true;

        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_ONLINE);
    }

    g_cellular.verify_failure_count++;

    if (g_cellular.verify_failure_count >= LINKG_CELLULAR_VERIFY_FAILURE_LIMIT ||
        cellular_runtime_state_timed_out(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_failed(-ENETUNREACH);
    }

    ret = cellular_runtime_schedule_action(&g_cellular.runtime,
                                           now_ms,
                                           LINKG_CELLULAR_VERIFY_RETRY_MS);
    if (ret != 0)
    {
        return _linkg_cellular_step_fatal(ret);
    }

    return _linkg_cellular_step_wait();
}

/**
 * @brief 维护已经验证上线的蜂窝数据链状态。
 */
static linkg_cellular_step_t _linkg_cellular_state_online(const cellular_status_info_t *info, uint64_t now_ms)
{
    int ret;

    if (_linkg_cellular_online_invalid(info))
    {
        return _linkg_cellular_step_failed(-ENETDOWN);
    }

    if (g_cellular.runtime.next_action_ms == 0U)
    {
        ret = cellular_runtime_schedule_action(&g_cellular.runtime,
                                               now_ms,
                                               LINKG_CELLULAR_ONLINE_VERIFY_INTERVAL_MS);
        if (ret != 0)
        {
            return _linkg_cellular_step_fatal(ret);
        }

        return _linkg_cellular_step_wait();
    }

    if (cellular_runtime_action_due(&g_cellular.runtime, now_ms))
    {
        return _linkg_cellular_step_done(CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY);
    }

    return _linkg_cellular_step_wait();
}

/**
 * @brief 等待统一连接重试退避期限到达。
 */
static linkg_cellular_step_t _linkg_cellular_state_retry_wait(void)
{
    if (!cellular_runtime_retry_due(&g_cellular.runtime, linkg_time_elapsed_ms()))
    {
        return _linkg_cellular_step_wait();
    }

    return _linkg_cellular_step_done(g_cellular.runtime.retry_target_state);
}

/**
 * @brief 执行当前运行状态的一步处理。
 */
static linkg_cellular_step_t _linkg_cellular_run_current_state(const cellular_status_info_t *info, uint64_t now_ms)
{
    switch (g_cellular.runtime.state)
    {
        case CELLULAR_RUNTIME_STATE_IDLE:
            return _linkg_cellular_state_idle();

        case CELLULAR_RUNTIME_STATE_WAIT_SIM:
            return _linkg_cellular_state_wait_sim();

        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            return _linkg_cellular_state_check_sim(info, now_ms);

        case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            return _linkg_cellular_state_enter_pin(now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_PIN:
            return _linkg_cellular_state_wait_pin(info);

        case CELLULAR_RUNTIME_STATE_WAIT_PUK:
            return _linkg_cellular_state_wait_puk(info);

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            return _linkg_cellular_state_wait_registration(info, now_ms);

        case CELLULAR_RUNTIME_STATE_PREPARE_PDP:
            return _linkg_cellular_state_prepare_pdp(now_ms);

        case CELLULAR_RUNTIME_STATE_ACTIVATE_PDP:
            return _linkg_cellular_state_activate_pdp(now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            return _linkg_cellular_state_wait_pdp(info, now_ms);

        case CELLULAR_RUNTIME_STATE_START_NETDEV:
            return _linkg_cellular_state_start_netdev(now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            return _linkg_cellular_state_wait_netdev(info, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            return _linkg_cellular_state_wait_host(info, now_ms);

        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
            return _linkg_cellular_state_verify(info, now_ms);

        case CELLULAR_RUNTIME_STATE_ONLINE:
            return _linkg_cellular_state_online(info, now_ms);

        case CELLULAR_RUNTIME_STATE_RETRY_WAIT:
            return _linkg_cellular_state_retry_wait();

        case CELLULAR_RUNTIME_STATE_NONE:
        default:
            return _linkg_cellular_step_fatal(-EPROTO);
    }
}

/****************************** 失败处理 ******************************/

/**
 * @brief 根据当前SIM会话累计重试次数计算有上限退避时间。
 */
static uint64_t _linkg_cellular_retry_delay(void)
{
    uint64_t delay_ms;
    uint32_t shift;

    shift = g_cellular.runtime.retry_count;
    if (shift > 4U)
    {
        shift = 4U;
    }

    delay_ms = LINKG_CELLULAR_RETRY_BASE_MS << shift;
    if (delay_ms > LINKG_CELLULAR_RETRY_MAX_MS)
    {
        delay_ms = LINKG_CELLULAR_RETRY_MAX_MS;
    }

    return delay_ms;
}

/**
 * @brief 记录当前状态失败并执行对应V1统一恢复策略。
 */
static int _linkg_cellular_handle_state_failure(int error, uint64_t now_ms)
{
    cellular_runtime_state_t failed_state;
    cellular_runtime_state_t retry_target;
    uint64_t                 retry_delay_ms;
    int                      cleanup_ret;
    int                      ret;

    ret = cellular_runtime_record_failure(&g_cellular.runtime, error, now_ms);
    if (ret != 0)
    {
        return ret;
    }

    failed_state = g_cellular.runtime.failed_state;

    LINKG_LOG_WARN("CELLULAR: runtime state failed, state=%s, error=%d",
                   cellular_runtime_state_name(failed_state),
                   error);

    switch (failed_state)
    {
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            retry_target = CELLULAR_RUNTIME_STATE_CHECK_SIM;
            break;

        case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            return _linkg_cellular_enter_state(CELLULAR_RUNTIME_STATE_WAIT_PIN, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            retry_target = CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION;
            break;

        case CELLULAR_RUNTIME_STATE_PREPARE_PDP:
        case CELLULAR_RUNTIME_STATE_ACTIVATE_PDP:
        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
        case CELLULAR_RUNTIME_STATE_START_NETDEV:
        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
        case CELLULAR_RUNTIME_STATE_ONLINE:
            retry_target = CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION;
            break;

        default:
            return -EPROTO;
    }

    if (failed_state != CELLULAR_RUNTIME_STATE_CHECK_SIM)
    {
        cleanup_ret = _linkg_cellular_cleanup_data_session();
        if (cleanup_ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: data cleanup during retry returned error=%d", cleanup_ret);
        }
    }

    retry_delay_ms = _linkg_cellular_retry_delay();

    ret = cellular_runtime_schedule_retry(&g_cellular.runtime,
                                          retry_target,
                                          now_ms,
                                          retry_delay_ms);
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("CELLULAR: retry scheduled, target=%s, delay_ms=%llu, count=%u",
                   cellular_runtime_state_name(retry_target),
                   (unsigned long long)retry_delay_ms,
                   (unsigned int)g_cellular.runtime.retry_count);

    return 0;
}

/****************************** Owner轮询 ******************************/

/**
 * @brief 获取Status与Runtime最近的Owner处理期限。
 */
static uint64_t _linkg_cellular_get_owner_deadline(void)
{
    uint64_t runtime_deadline;
    uint64_t status_deadline;

    status_deadline  = cellular_status_get_deadline();
    runtime_deadline = cellular_runtime_get_deadline(&g_cellular.runtime);

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
    linkg_cellular_step_t         step;
    uint64_t                      now_ms;
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

        session_result = _linkg_cellular_sync_sim_session(&events, &info, now_ms);
        if (session_result < 0)
        {
            return session_result;
        }

        if (session_result > 0)
        {
            continue;
        }

        step = _linkg_cellular_run_current_state(&info, now_ms);

        switch (step.result)
        {
            case CELLULAR_RUNTIME_STEP_DONE:
                ret = _linkg_cellular_enter_state(step.next_state, now_ms);
                if (ret != 0)
                {
                    return ret;
                }

                continue;

            case CELLULAR_RUNTIME_STEP_FAILED:
                ret = _linkg_cellular_handle_state_failure(step.error, now_ms);
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

    if (cellular_runtime_session_active(&g_cellular.runtime))
    {
        ret = _linkg_cellular_cleanup_data_session();
        if (ret != 0)
        {
            _linkg_cellular_record_first_error(&first_error, ret);
        }

        cellular_runtime_end_session(&g_cellular.runtime, linkg_time_elapsed_ms());
    }

    g_cellular.pdp_action_started    = false;
    g_cellular.netdev_action_started = false;

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
    cellular_runtime_reset(&g_cellular.runtime, linkg_time_elapsed_ms());
    g_cellular.requested_refresh = CELLULAR_STATUS_REFRESH_NONE;
    _linkg_cellular_reset_verify();

    return first_error;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝模块及其内部纯软件子模块。
 */
int linkg_cellular_init(const linkg_cellular_config_t *config)
{
    cellular_runtime_t runtime;
    uint64_t           now_ms;
    int                ret;

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

    ret = cellular_runtime_init(&runtime, now_ms);
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
    g_cellular.runtime             = runtime;
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
        ret = g_cellular.lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED
            ? -ENODEV
            : -EALREADY;

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

    cellular_runtime_reset(&g_cellular.runtime, linkg_time_elapsed_ms());

    ret = _linkg_cellular_enter_state(CELLULAR_RUNTIME_STATE_WAIT_SIM,
                                      linkg_time_elapsed_ms());
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
        return lifecycle == LINKG_CELLULAR_LIFECYCLE_UNINITIALIZED
            ? -ENODEV
            : -ENETDOWN;
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
