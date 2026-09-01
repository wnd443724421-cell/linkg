/**
 * @file cellular_status.c
 * @brief LinkG蜂窝网络内部状态管理实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-08-31
 */

#include "cellular_status.h"

#include <errno.h>
#include <net/if.h>
#include <pthread.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

#include "rg255_query.h"

/****************************** 状态周期 ******************************/

#define CELLULAR_STATUS_RETRY_INTERVAL_MS                    1000U  // 普通采集失败后的快速重试周期
#define CELLULAR_STATUS_NETWORK_MODE_RETRY_INTERVAL_MS       5000U  // 网络模式采集失败后的重试周期
#define CELLULAR_STATUS_SIM_STEADY_INTERVAL_MS               30000U // SIM稳定状态Watchdog周期
#define CELLULAR_STATUS_SIM_CONVERGENCE_INTERVAL_MS          1000U  // SIM初始化阶段确认周期
#define CELLULAR_STATUS_REGISTRATION_STEADY_INTERVAL_MS      10000U // 已注册状态Watchdog周期
#define CELLULAR_STATUS_REGISTRATION_CONVERGENCE_INTERVAL_MS 1000U  // 注网阶段确认周期
#define CELLULAR_STATUS_RADIO_INTERVAL_MS                    5000U  // 服务小区无线状态Watchdog周期
#define CELLULAR_STATUS_PDP_STEADY_INTERVAL_MS               3000U  // PDP已激活状态Watchdog周期
#define CELLULAR_STATUS_PDP_CONVERGENCE_INTERVAL_MS          1000U  // PDP收敛阶段确认周期
#define CELLULAR_STATUS_NETDEV_STEADY_INTERVAL_MS            5000U  // USB网络设备稳定状态Watchdog周期
#define CELLULAR_STATUS_NETDEV_CONVERGENCE_INTERVAL_MS       1000U  // USB网络设备收敛阶段确认周期
#define CELLULAR_STATUS_HOST_INTERVAL_MS                     800U   // Linux Host实际网络状态Watchdog周期

/****************************** 内部类型 ******************************/

typedef struct
{
    uint64_t network_mode_ms;     // 网络选择模式下一次确认时间
    uint64_t sim_ms;              // SIM逻辑状态下一次确认时间
    uint64_t registration_ms;     // 网络注册状态下一次确认时间
    uint64_t radio_ms;            // 服务小区无线状态下一次确认时间
    uint64_t pdp_ms;              // PDP激活状态下一次确认时间
    uint64_t pdp_address_ms;      // 模组PDP地址失败后的下一次重试时间
    uint64_t netdev_ms;           // USB网络设备状态下一次确认时间
    uint64_t expected_network_ms; // 模组期望Host网络参数失败后的下一次重试时间
    uint64_t host_ms;             // Linux Host状态下一次确认时间
} cellular_status_deadlines_t;

typedef struct
{
    pthread_mutex_t             lock;        // 状态锁，保护生命周期、期限和已发布快照
    at_channel_t               *channel;     // 借用AT通道，仅在start到stop期间有效
    cellular_status_info_t      info;        // 当前唯一内部事实快照
    cellular_status_deadlines_t deadlines;   // 各事实确认和失败重试绝对到期时间
    bool                        initialized; // 状态模块是否已经初始化
    bool                        started;     // 状态模块是否已经启动
} cellular_status_context_t;

/****************************** 全局上下文 ******************************/

static cellular_status_context_t g_cellular_status =
{
    .lock = PTHREAD_MUTEX_INITIALIZER // 状态锁静态初始化
};

/****************************** 状态元数据 ******************************/

/**
 * @brief 初始化单项事实采集元数据。
 */
static void _cellular_status_meta_init(cellular_status_meta_t *meta)
{
    if (meta == NULL)
    {
        return;
    }

    memset(meta, 0, sizeof(*meta));
    meta->last_error = -EAGAIN;
}

/**
 * @brief 记录单项事实本次成功确认。
 */
static void _cellular_status_meta_success(cellular_status_meta_t *meta, uint64_t now_ms)
{
    if (meta == NULL)
    {
        return;
    }

    meta->confirmed    = true;
    meta->attempted_ms = now_ms;
    meta->updated_ms   = now_ms;
    meta->last_error   = 0;
}

/**
 * @brief 记录单项事实本次采集失败并保留最近成功事实。
 */
static void _cellular_status_meta_failure(cellular_status_meta_t *meta, uint64_t now_ms, int error)
{
    if (meta == NULL)
    {
        return;
    }

    meta->attempted_ms = now_ms;
    meta->last_error   = error != 0 ? error : -EIO;
}

/****************************** 快照初始化 ******************************/

/**
 * @brief 初始化内部事实快照默认值。
 */
static void _cellular_status_info_init(cellular_status_info_t *info)
{
    if (info == NULL)
    {
        return;
    }

    memset(info, 0, sizeof(*info));

    info->partial              = true;
    info->last_error           = -EAGAIN;
    info->local.network_mode   = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    info->local.sim_state      = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    info->network.registration = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    info->network.network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
    info->netdev.state.mode    = CELLULAR_STATUS_NETDEV_MODE_UNKNOWN;

    _cellular_status_meta_init(&info->local.network_mode_meta);
    _cellular_status_meta_init(&info->local.sim_meta);
    _cellular_status_meta_init(&info->network.registration_meta);
    _cellular_status_meta_init(&info->network.radio_meta);
    _cellular_status_meta_init(&info->pdp.active_meta);
    _cellular_status_meta_init(&info->pdp.address_meta);
    _cellular_status_meta_init(&info->netdev.state.meta);
    _cellular_status_meta_init(&info->netdev.expected_ipv4.meta);
    _cellular_status_meta_init(&info->netdev.expected_ipv6.meta);
    _cellular_status_meta_init(&info->host.interface_meta);
    _cellular_status_meta_init(&info->host.interface_up_meta);
    _cellular_status_meta_init(&info->host.ipv4_meta);
    _cellular_status_meta_init(&info->host.ipv4_netmask_meta);
    _cellular_status_meta_init(&info->host.ipv6_meta);
    _cellular_status_meta_init(&info->host.ipv4_route_meta);
    _cellular_status_meta_init(&info->host.ipv6_route_meta);
}

/**
 * @brief 清空状态模块运行上下文。
 *
 * @note 调用方必须持有状态锁，且不存在正在执行的Owner状态处理调用。
 */
static void _cellular_status_reset_context_locked(void)
{
    memset(&g_cellular_status.deadlines, 0, sizeof(g_cellular_status.deadlines));
    _cellular_status_info_init(&g_cellular_status.info);

    g_cellular_status.channel     = NULL;
    g_cellular_status.initialized = false;
    g_cellular_status.started     = false;
}

/****************************** 期限辅助 ******************************/

/**
 * @brief 返回两个非零期限中更早的一个。
 */
static uint64_t _cellular_status_min_deadline(uint64_t left, uint64_t right)
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
 * @brief 根据绝对期限计算当前已经到期的状态刷新集合。
 */
static cellular_status_refresh_mask_t _cellular_status_due_mask(const cellular_status_deadlines_t *deadlines, uint64_t now_ms)
{
    cellular_status_refresh_mask_t mask;

    if (deadlines == NULL)
    {
        return CELLULAR_STATUS_REFRESH_NONE;
    }

    mask = CELLULAR_STATUS_REFRESH_NONE;

    if (deadlines->network_mode_ms != 0U && now_ms >= deadlines->network_mode_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_NETWORK_MODE;
    }

    if (deadlines->sim_ms != 0U && now_ms >= deadlines->sim_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_SIM;
    }

    if (deadlines->registration_ms != 0U && now_ms >= deadlines->registration_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_REGISTRATION;
    }

    if (deadlines->radio_ms != 0U && now_ms >= deadlines->radio_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_RADIO;
    }

    if (deadlines->pdp_ms != 0U && now_ms >= deadlines->pdp_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_PDP;
    }

    if (deadlines->pdp_address_ms != 0U && now_ms >= deadlines->pdp_address_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_PDP_ADDRESS;
    }

    if (deadlines->netdev_ms != 0U && now_ms >= deadlines->netdev_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_NETDEV;
    }

    if (deadlines->expected_network_ms != 0U && now_ms >= deadlines->expected_network_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK;
    }

    if (deadlines->host_ms != 0U && now_ms >= deadlines->host_ms)
    {
        mask |= CELLULAR_STATUS_REFRESH_HOST;
    }

    return mask;
}

/**
 * @brief 计算SIM事实下一次Watchdog确认时间。
 */
static uint64_t _cellular_status_next_sim_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->local.sim_meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_NOT_READY ||
        info->local.sim_state == LINKG_CELLULAR_SIM_STATE_UNKNOWN)
    {
        return now_ms + CELLULAR_STATUS_SIM_CONVERGENCE_INTERVAL_MS;
    }

    return now_ms + CELLULAR_STATUS_SIM_STEADY_INTERVAL_MS;
}

/**
 * @brief 计算网络注册事实下一次Watchdog确认时间。
 */
static uint64_t _cellular_status_next_registration_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->network.registration_meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    if (info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERING ||
        info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_NOT_REGISTERED ||
        info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN)
    {
        return now_ms + CELLULAR_STATUS_REGISTRATION_CONVERGENCE_INTERVAL_MS;
    }

    return now_ms + CELLULAR_STATUS_REGISTRATION_STEADY_INTERVAL_MS;
}

/**
 * @brief 计算服务小区事实下一次Watchdog确认时间。
 */
static uint64_t _cellular_status_next_radio_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->network.radio_meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    return now_ms + CELLULAR_STATUS_RADIO_INTERVAL_MS;
}

/**
 * @brief 计算PDP激活事实下一次Watchdog确认时间。
 */
static uint64_t _cellular_status_next_pdp_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->pdp.active_meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    if (info->pdp.active)
    {
        return now_ms + CELLULAR_STATUS_PDP_STEADY_INTERVAL_MS;
    }

    if (info->network.registration_meta.confirmed &&
        info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
    {
        return now_ms + CELLULAR_STATUS_PDP_CONVERGENCE_INTERVAL_MS;
    }

    return now_ms + CELLULAR_STATUS_PDP_STEADY_INTERVAL_MS;
}

/**
 * @brief 计算模组PDP地址失败后的下一次重试时间。
 *
 * @note PDP地址成功确认后停止内部定时查询，等待Owner或PDP状态变化再次触发刷新。
 */
static uint64_t _cellular_status_next_pdp_address_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->pdp.address_meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    return 0U;
}

/**
 * @brief 计算USB网络设备事实下一次Watchdog确认时间。
 */
static uint64_t _cellular_status_next_netdev_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->netdev.state.meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    if (info->netdev.state.connected)
    {
        return now_ms + CELLULAR_STATUS_NETDEV_STEADY_INTERVAL_MS;
    }

    if (info->pdp.active_meta.confirmed && info->pdp.active)
    {
        return now_ms + CELLULAR_STATUS_NETDEV_CONVERGENCE_INTERVAL_MS;
    }

    return now_ms + CELLULAR_STATUS_NETDEV_STEADY_INTERVAL_MS;
}

/**
 * @brief 计算模组期望Host网络参数失败后的下一次重试时间。
 *
 * @note IPv4和IPv6参数均成功确认后停止内部定时查询，等待Owner或Netdev状态变化再次触发刷新。
 */
static uint64_t _cellular_status_next_expected_network_deadline(const cellular_status_info_t *info, uint64_t now_ms)
{
    if (info->netdev.expected_ipv4.meta.last_error != 0 ||
        info->netdev.expected_ipv6.meta.last_error != 0)
    {
        return now_ms + CELLULAR_STATUS_RETRY_INTERVAL_MS;
    }

    return 0U;
}

/**
 * @brief 更新本次已经处理事实的下一次确认和失败重试期限。
 */
static void _cellular_status_update_deadlines(cellular_status_deadlines_t *deadlines, const cellular_status_info_t *info, cellular_status_refresh_mask_t processed, uint64_t now_ms)
{
    if ((processed & CELLULAR_STATUS_REFRESH_NETWORK_MODE) != 0U)
    {
        deadlines->network_mode_ms = info->local.network_mode_meta.last_error == 0
            ? 0U
            : now_ms + CELLULAR_STATUS_NETWORK_MODE_RETRY_INTERVAL_MS;
    }

    if ((processed & CELLULAR_STATUS_REFRESH_SIM) != 0U)
    {
        deadlines->sim_ms = _cellular_status_next_sim_deadline(info, now_ms);
    }

    if ((processed & CELLULAR_STATUS_REFRESH_REGISTRATION) != 0U)
    {
        deadlines->registration_ms = _cellular_status_next_registration_deadline(info, now_ms);
    }

    if ((processed & CELLULAR_STATUS_REFRESH_RADIO) != 0U)
    {
        deadlines->radio_ms = _cellular_status_next_radio_deadline(info, now_ms);
    }

    if ((processed & CELLULAR_STATUS_REFRESH_PDP) != 0U)
    {
        deadlines->pdp_ms = _cellular_status_next_pdp_deadline(info, now_ms);

        if (info->pdp.active_meta.confirmed &&
            info->pdp.active_meta.last_error == 0 &&
            !info->pdp.active)
        {
            deadlines->pdp_address_ms = 0U;
        }
    }

    if ((processed & CELLULAR_STATUS_REFRESH_PDP_ADDRESS) != 0U)
    {
        deadlines->pdp_address_ms = _cellular_status_next_pdp_address_deadline(info, now_ms);
    }

    if ((processed & CELLULAR_STATUS_REFRESH_NETDEV) != 0U)
    {
        deadlines->netdev_ms = _cellular_status_next_netdev_deadline(info, now_ms);

        if (info->netdev.state.meta.confirmed &&
            info->netdev.state.meta.last_error == 0 &&
            !info->netdev.state.connected)
        {
            deadlines->expected_network_ms = 0U;
        }
    }

    if ((processed & CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK) != 0U)
    {
        deadlines->expected_network_ms = _cellular_status_next_expected_network_deadline(info, now_ms);
    }

    if ((processed & CELLULAR_STATUS_REFRESH_HOST) != 0U)
    {
        deadlines->host_ms = now_ms + CELLULAR_STATUS_HOST_INTERVAL_MS;
    }
}

/****************************** 运行上下文 ******************************/

/**
 * @brief 获取Owner处理所需AT通道、快照和期限副本。
 */
static int _cellular_status_get_runtime(at_channel_t **channel, cellular_status_info_t *info, cellular_status_deadlines_t *deadlines)
{
    int ret;

    if (channel == NULL || info == NULL || deadlines == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    if (!g_cellular_status.started || g_cellular_status.channel == NULL)
    {
        ret = -ENETDOWN;
        goto unlock;
    }

    *channel   = g_cellular_status.channel;
    *info      = g_cellular_status.info;
    *deadlines = g_cellular_status.deadlines;
    ret        = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/****************************** 本机状态采集 ******************************/

/**
 * @brief 刷新RG255当前网络选择模式事实。
 */
static void _cellular_status_refresh_network_mode(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    linkg_cellular_network_mode_t mode;
    int                           ret;

    mode = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    ret = rg255_query_network_mode(channel, &mode);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->local.network_mode_meta, now_ms, ret);
        return;
    }

    info->local.network_mode = mode;
    _cellular_status_meta_success(&info->local.network_mode_meta, now_ms);
}

/**
 * @brief 刷新RG255当前SIM逻辑状态事实。
 */
static void _cellular_status_refresh_sim(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    linkg_cellular_sim_state_t state;
    int                         ret;

    state = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    ret = rg255_query_sim_state(channel, &state);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->local.sim_meta, now_ms, ret);
        return;
    }

    info->local.sim_state = state;
    _cellular_status_meta_success(&info->local.sim_meta, now_ms);
}

/****************************** 移动网络状态采集 ******************************/

/**
 * @brief 刷新当前网络注册状态事实。
 */
static void _cellular_status_refresh_registration(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    linkg_cellular_network_mode_t       mode;
    linkg_cellular_registration_state_t state;
    int                                 ret;

    mode = info->local.network_mode_meta.confirmed && info->local.network_mode_meta.last_error == 0
        ? info->local.network_mode
        : LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;

    state = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    ret = rg255_query_registration(channel, mode, &state);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->network.registration_meta, now_ms, ret);
        return;
    }

    info->network.registration = state;
    _cellular_status_meta_success(&info->network.registration_meta, now_ms);
}

/**
 * @brief 清空当前服务小区事实并确认当前无可识别服务小区。
 */
static void _cellular_status_clear_radio(cellular_status_info_t *info, uint64_t now_ms)
{
    info->network.serving_cell_valid = false;
    info->network.network_type       = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
    info->network.band               = 0U;
    info->network.rsrp_dbm           = 0;
    info->network.rsrp_valid         = false;
    info->network.rsrq_db            = 0;
    info->network.rsrq_valid         = false;
    info->network.sinr_db            = 0;
    info->network.sinr_valid         = false;

    _cellular_status_meta_success(&info->network.radio_meta, now_ms);
}

/**
 * @brief 通过一次QENG查询原子刷新网络类型和服务小区无线状态。
 */
static void _cellular_status_refresh_radio(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    rg255_serving_cell_info_t serving_cell;
    int                       ret;

    memset(&serving_cell, 0, sizeof(serving_cell));
    serving_cell.network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;

    ret = rg255_query_serving_cell(channel, &serving_cell);
    if (ret == -ENODATA)
    {
        _cellular_status_clear_radio(info, now_ms);
        return;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->network.radio_meta, now_ms, ret);
        return;
    }

    info->network.serving_cell_valid = true;
    info->network.network_type       = serving_cell.network_type;
    info->network.band               = serving_cell.band;
    info->network.rsrp_dbm           = serving_cell.rsrp_dbm;
    info->network.rsrp_valid         = serving_cell.rsrp_valid;
    info->network.rsrq_db            = serving_cell.rsrq_db;
    info->network.rsrq_valid         = serving_cell.rsrq_valid;
    info->network.sinr_db            = serving_cell.sinr_db;
    info->network.sinr_valid         = serving_cell.sinr_valid;

    _cellular_status_meta_success(&info->network.radio_meta, now_ms);
}

/****************************** PDP状态采集 ******************************/

/**
 * @brief 清空模组PDP地址并确认当前地址事实不可用。
 */
static void _cellular_status_clear_pdp_address(cellular_status_info_t *info, uint64_t now_ms)
{
    info->pdp.ipv4_valid        = false;
    info->pdp.global_ipv6_valid = false;

    memset(&info->pdp.ipv4, 0, sizeof(info->pdp.ipv4));
    memset(&info->pdp.global_ipv6, 0, sizeof(info->pdp.global_ipv6));

    _cellular_status_meta_success(&info->pdp.address_meta, now_ms);
}

/**
 * @brief 刷新默认PDP上下文模组地址事实。
 */
static void _cellular_status_refresh_pdp_address(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    rg255_pdp_address_t address;
    int                 ret;

    if (info->pdp.active_meta.confirmed &&
        info->pdp.active_meta.last_error == 0 &&
        !info->pdp.active)
    {
        _cellular_status_clear_pdp_address(info, now_ms);
        return;
    }

    if (!info->pdp.active_meta.confirmed || info->pdp.active_meta.last_error != 0)
    {
        _cellular_status_meta_failure(&info->pdp.address_meta, now_ms, -EAGAIN);
        return;
    }

    memset(&address, 0, sizeof(address));

    ret = rg255_query_pdp_address(channel, &address);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->pdp.address_meta, now_ms, ret);
        return;
    }

    info->pdp.ipv4_valid        = address.ipv4_valid;
    info->pdp.ipv4              = address.ipv4;
    info->pdp.global_ipv6_valid = address.global_ipv6_valid;
    info->pdp.global_ipv6       = address.global_ipv6;

    _cellular_status_meta_success(&info->pdp.address_meta, now_ms);
}

/**
 * @brief 刷新默认PDP上下文激活状态事实。
 */
static bool _cellular_status_refresh_pdp(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    bool previous_active;
    bool active;
    int  ret;

    previous_active = info->pdp.active_meta.confirmed && info->pdp.active;
    active          = false;

    ret = rg255_query_pdp_active(channel, &active);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->pdp.active_meta, now_ms, ret);
        return false;
    }

    info->pdp.active = active;
    _cellular_status_meta_success(&info->pdp.active_meta, now_ms);

    if (!active)
    {
        _cellular_status_clear_pdp_address(info, now_ms);
        return false;
    }

    return !previous_active || !info->pdp.address_meta.confirmed;
}

/****************************** USB网络设备状态采集 ******************************/

/**
 * @brief 将RG255网络设备模式转换为内部归一化模式。
 */
static cellular_status_netdev_mode_t _cellular_status_convert_netdev_mode(rg255_netdev_type_t type)
{
    switch (type)
    {
        case RG255_NETDEV_TYPE_DISCONNECT:
            return CELLULAR_STATUS_NETDEV_MODE_DISCONNECT;

        case RG255_NETDEV_TYPE_ONCE:
            return CELLULAR_STATUS_NETDEV_MODE_ONCE;

        case RG255_NETDEV_TYPE_AUTO:
            return CELLULAR_STATUS_NETDEV_MODE_AUTO;

        default:
            return CELLULAR_STATUS_NETDEV_MODE_UNKNOWN;
    }
}

/**
 * @brief 清空RG255提供给Host的期望IPv4和IPv6网络参数。
 */
static void _cellular_status_clear_expected_network(cellular_status_info_t *info, uint64_t now_ms)
{
    info->netdev.expected_ipv4.valid = false;
    info->netdev.expected_ipv6.valid = false;

    memset(&info->netdev.expected_ipv4.address, 0, sizeof(info->netdev.expected_ipv4.address));
    memset(&info->netdev.expected_ipv4.netmask, 0, sizeof(info->netdev.expected_ipv4.netmask));
    memset(&info->netdev.expected_ipv4.gateway, 0, sizeof(info->netdev.expected_ipv4.gateway));
    memset(&info->netdev.expected_ipv6.prefix, 0, sizeof(info->netdev.expected_ipv6.prefix));
    info->netdev.expected_ipv6.prefix_length = 0U;
    memset(&info->netdev.expected_ipv6.gateway, 0, sizeof(info->netdev.expected_ipv6.gateway));

    _cellular_status_meta_success(&info->netdev.expected_ipv4.meta, now_ms);
    _cellular_status_meta_success(&info->netdev.expected_ipv6.meta, now_ms);
}

/**
 * @brief 刷新RG255提供给Host的期望IPv4和IPv6网络参数。
 */
static void _cellular_status_refresh_expected_network(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    rg255_network_card_ipv4_info_t ipv4;
    rg255_network_card_ipv6_info_t ipv6;
    int                            ret;

    if (!info->netdev.state.meta.confirmed || info->netdev.state.meta.last_error != 0)
    {
        _cellular_status_meta_failure(&info->netdev.expected_ipv4.meta, now_ms, -EAGAIN);
        _cellular_status_meta_failure(&info->netdev.expected_ipv6.meta, now_ms, -EAGAIN);
        return;
    }

    if (!info->netdev.state.connected)
    {
        _cellular_status_clear_expected_network(info, now_ms);
        return;
    }

    memset(&ipv4, 0, sizeof(ipv4));
    ret = rg255_query_network_card_ipv4(channel, &ipv4);
    if (ret == 0)
    {
        info->netdev.expected_ipv4.valid   = true;
        info->netdev.expected_ipv4.address = ipv4.address;
        info->netdev.expected_ipv4.netmask = ipv4.netmask;
        info->netdev.expected_ipv4.gateway = ipv4.gateway;
        _cellular_status_meta_success(&info->netdev.expected_ipv4.meta, now_ms);
    }
    else
    {
        _cellular_status_meta_failure(&info->netdev.expected_ipv4.meta, now_ms, ret);
    }

    memset(&ipv6, 0, sizeof(ipv6));
    ret = rg255_query_network_card_ipv6(channel, &ipv6);
    if (ret == 0)
    {
        info->netdev.expected_ipv6.valid         = true;
        info->netdev.expected_ipv6.prefix        = ipv6.prefix;
        info->netdev.expected_ipv6.prefix_length = ipv6.prefix_length;
        info->netdev.expected_ipv6.gateway       = ipv6.gateway;
        _cellular_status_meta_success(&info->netdev.expected_ipv6.meta, now_ms);
    }
    else
    {
        _cellular_status_meta_failure(&info->netdev.expected_ipv6.meta, now_ms, ret);
    }
}

/**
 * @brief 刷新RG255 USB网络设备连接状态事实。
 */
static bool _cellular_status_refresh_netdev(at_channel_t *channel, cellular_status_info_t *info, uint64_t now_ms)
{
    rg255_netdev_status_t status;
    bool                  previous_connected;
    int                   ret;

    previous_connected = info->netdev.state.meta.confirmed && info->netdev.state.connected;

    memset(&status, 0, sizeof(status));
    status.type = RG255_NETDEV_TYPE_UNKNOWN;

    ret = rg255_query_netdev_status(channel, &status);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->netdev.state.meta, now_ms, ret);
        return false;
    }

    info->netdev.state.mode        = _cellular_status_convert_netdev_mode(status.type);
    info->netdev.state.cid         = status.cid;
    info->netdev.state.urc_enabled = status.urc_enabled;
    info->netdev.state.connected   = status.connected;

    _cellular_status_meta_success(&info->netdev.state.meta, now_ms);

    if (!status.connected)
    {
        _cellular_status_clear_expected_network(info, now_ms);
        return false;
    }

    return !previous_connected ||
        !info->netdev.expected_ipv4.meta.confirmed ||
        !info->netdev.expected_ipv6.meta.confirmed;
}

/****************************** Host网络状态采集 ******************************/

/**
 * @brief 判断Host地址查询错误是否表示当前没有对应地址。
 */
static bool _cellular_status_host_address_absent(int error)
{
    return error == -EADDRNOTAVAIL || error == -ENOENT;
}

/**
 * @brief 判断Host接口查询错误是否表示接口已经消失。
 */
static bool _cellular_status_host_interface_absent(int error)
{
    return error == -ENODEV || error == -ENXIO;
}

/**
 * @brief 清空Host接口依赖事实并确认当前接口不存在。
 */
static void _cellular_status_clear_host(cellular_status_info_t *info, uint64_t now_ms)
{
    cellular_status_host_info_t *host;

    host = &info->host;

    host->interface_present  = false;
    host->interface_index    = 0U;
    host->interface_up       = false;
    host->ipv4_valid         = false;
    host->ipv4_netmask_valid = false;
    host->global_ipv6_valid  = false;
    host->ipv4_gateway_valid = false;
    host->ipv6_gateway_valid = false;

    memset(&host->ipv4, 0, sizeof(host->ipv4));
    memset(&host->ipv4_netmask, 0, sizeof(host->ipv4_netmask));
    memset(&host->global_ipv6, 0, sizeof(host->global_ipv6));
    memset(&host->ipv4_gateway, 0, sizeof(host->ipv4_gateway));
    memset(&host->ipv6_gateway, 0, sizeof(host->ipv6_gateway));

    _cellular_status_meta_success(&host->interface_meta, now_ms);
    _cellular_status_meta_success(&host->interface_up_meta, now_ms);
    _cellular_status_meta_success(&host->ipv4_meta, now_ms);
    _cellular_status_meta_success(&host->ipv4_netmask_meta, now_ms);
    _cellular_status_meta_success(&host->ipv6_meta, now_ms);
    _cellular_status_meta_success(&host->ipv4_route_meta, now_ms);
    _cellular_status_meta_success(&host->ipv6_route_meta, now_ms);
}

/**
 * @brief 刷新Linux蜂窝接口存在状态和接口索引。
 */
static bool _cellular_status_refresh_host_interface(cellular_status_info_t *info, uint64_t now_ms)
{
    unsigned int ifindex;

    if (!linkg_network_interface_exists(LINKG_RESOURCE_INTERFACE_CELLULAR))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    errno = 0;
    ifindex = if_nametoindex(LINKG_RESOURCE_INTERFACE_CELLULAR);
    if (ifindex == 0U)
    {
        if (errno == ENODEV || errno == ENXIO || errno == 0)
        {
            _cellular_status_clear_host(info, now_ms);
            return false;
        }

        _cellular_status_meta_failure(&info->host.interface_meta, now_ms, -errno);
        return true;
    }

    info->host.interface_present = true;
    info->host.interface_index   = ifindex;
    _cellular_status_meta_success(&info->host.interface_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口UP状态。
 */
static bool _cellular_status_refresh_host_link(cellular_status_info_t *info, uint64_t now_ms)
{
    bool up;
    int  ret;

    up  = false;
    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_CELLULAR, &up);
    if (_cellular_status_host_interface_absent(ret))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->host.interface_up_meta, now_ms, ret);
        return true;
    }

    info->host.interface_up = up;
    _cellular_status_meta_success(&info->host.interface_up_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口IPv4地址事实。
 */
static bool _cellular_status_refresh_host_ipv4(cellular_status_info_t *info, uint64_t now_ms)
{
    struct in_addr address;
    int            ret;

    memset(&address, 0, sizeof(address));
    ret = linkg_network_interface_get_ipv4(LINKG_RESOURCE_INTERFACE_CELLULAR, &address);
    if (_cellular_status_host_interface_absent(ret))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    if (_cellular_status_host_address_absent(ret))
    {
        info->host.ipv4_valid = false;
        memset(&info->host.ipv4, 0, sizeof(info->host.ipv4));
        _cellular_status_meta_success(&info->host.ipv4_meta, now_ms);
        return true;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->host.ipv4_meta, now_ms, ret);
        return true;
    }

    info->host.ipv4       = address;
    info->host.ipv4_valid = linkg_network_ipv4_address_valid(&address);
    _cellular_status_meta_success(&info->host.ipv4_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口IPv4子网掩码事实。
 */
static bool _cellular_status_refresh_host_ipv4_netmask(cellular_status_info_t *info, uint64_t now_ms)
{
    struct in_addr netmask;
    int            ret;

    memset(&netmask, 0, sizeof(netmask));
    ret = linkg_network_interface_get_ipv4_netmask(LINKG_RESOURCE_INTERFACE_CELLULAR, &netmask);
    if (_cellular_status_host_interface_absent(ret))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    if (_cellular_status_host_address_absent(ret))
    {
        info->host.ipv4_netmask_valid = false;
        memset(&info->host.ipv4_netmask, 0, sizeof(info->host.ipv4_netmask));
        _cellular_status_meta_success(&info->host.ipv4_netmask_meta, now_ms);
        return true;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->host.ipv4_netmask_meta, now_ms, ret);
        return true;
    }

    info->host.ipv4_netmask       = netmask;
    info->host.ipv4_netmask_valid = linkg_network_ipv4_netmask_valid(&netmask);
    _cellular_status_meta_success(&info->host.ipv4_netmask_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口Global IPv6地址事实。
 */
static bool _cellular_status_refresh_host_ipv6(cellular_status_info_t *info, uint64_t now_ms)
{
    struct in6_addr address;
    int             ret;

    memset(&address, 0, sizeof(address));
    ret = linkg_network_interface_get_global_ipv6(LINKG_RESOURCE_INTERFACE_CELLULAR, &address);
    if (_cellular_status_host_interface_absent(ret))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    if (_cellular_status_host_address_absent(ret))
    {
        info->host.global_ipv6_valid = false;
        memset(&info->host.global_ipv6, 0, sizeof(info->host.global_ipv6));
        _cellular_status_meta_success(&info->host.ipv6_meta, now_ms);
        return true;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->host.ipv6_meta, now_ms, ret);
        return true;
    }

    info->host.global_ipv6       = address;
    info->host.global_ipv6_valid = linkg_network_ipv6_address_is_global(&address);
    _cellular_status_meta_success(&info->host.ipv6_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口IPv4默认路由事实。
 */
static bool _cellular_status_refresh_host_ipv4_route(cellular_status_info_t *info, uint64_t now_ms)
{
    struct in_addr gateway;
    int            ret;

    memset(&gateway, 0, sizeof(gateway));
    ret = linkg_network_route_get_ipv4_default_gateway(LINKG_RESOURCE_INTERFACE_CELLULAR, &gateway);
    if (_cellular_status_host_interface_absent(ret))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    if (_cellular_status_host_address_absent(ret))
    {
        info->host.ipv4_gateway_valid = false;
        memset(&info->host.ipv4_gateway, 0, sizeof(info->host.ipv4_gateway));
        _cellular_status_meta_success(&info->host.ipv4_route_meta, now_ms);
        return true;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->host.ipv4_route_meta, now_ms, ret);
        return true;
    }

    info->host.ipv4_gateway       = gateway;
    info->host.ipv4_gateway_valid = linkg_network_ipv4_address_valid(&gateway);
    _cellular_status_meta_success(&info->host.ipv4_route_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口IPv6默认路由事实。
 */
static bool _cellular_status_refresh_host_ipv6_route(cellular_status_info_t *info, uint64_t now_ms)
{
    struct in6_addr gateway;
    int             ret;

    memset(&gateway, 0, sizeof(gateway));
    ret = linkg_network_route_get_ipv6_default_gateway(LINKG_RESOURCE_INTERFACE_CELLULAR, &gateway);
    if (_cellular_status_host_interface_absent(ret))
    {
        _cellular_status_clear_host(info, now_ms);
        return false;
    }

    if (_cellular_status_host_address_absent(ret))
    {
        info->host.ipv6_gateway_valid = false;
        memset(&info->host.ipv6_gateway, 0, sizeof(info->host.ipv6_gateway));
        _cellular_status_meta_success(&info->host.ipv6_route_meta, now_ms);
        return true;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->host.ipv6_route_meta, now_ms, ret);
        return true;
    }

    info->host.ipv6_gateway       = gateway;
    info->host.ipv6_gateway_valid = !IN6_IS_ADDR_UNSPECIFIED(&gateway) && !IN6_IS_ADDR_MULTICAST(&gateway);
    _cellular_status_meta_success(&info->host.ipv6_route_meta, now_ms);

    return true;
}

/**
 * @brief 刷新Linux蜂窝接口全部实际网络事实。
 */
static void _cellular_status_refresh_host(cellular_status_info_t *info, uint64_t now_ms)
{
    if (!_cellular_status_refresh_host_interface(info, now_ms))
    {
        return;
    }

    if (!_cellular_status_refresh_host_link(info, now_ms))
    {
        return;
    }

    if (!_cellular_status_refresh_host_ipv4(info, now_ms))
    {
        return;
    }

    if (!_cellular_status_refresh_host_ipv4_netmask(info, now_ms))
    {
        return;
    }

    if (!_cellular_status_refresh_host_ipv6(info, now_ms))
    {
        return;
    }

    if (!_cellular_status_refresh_host_ipv4_route(info, now_ms))
    {
        return;
    }

    (void)_cellular_status_refresh_host_ipv6_route(info, now_ms);
}

/****************************** 快照状态 ******************************/

/**
 * @brief 获取当前公开事实中的第一个采集错误。
 */
static int _cellular_status_first_public_error(const cellular_status_info_t *info)
{
    const cellular_status_meta_t *metas[] =
    {
        &info->local.network_mode_meta,
        &info->local.sim_meta,
        &info->network.registration_meta,
        &info->network.radio_meta,
        &info->pdp.active_meta,
        &info->host.ipv4_meta,
        &info->host.ipv6_meta
    };
    size_t index;

    for (index = 0U; index < sizeof(metas) / sizeof(metas[0]); index++)
    {
        if (!metas[index]->confirmed)
        {
            return metas[index]->last_error != 0 ? metas[index]->last_error : -EAGAIN;
        }

        if (metas[index]->last_error != 0)
        {
            return metas[index]->last_error;
        }
    }

    return 0;
}

/**
 * @brief 完成本轮内部事实快照整体元数据更新。
 */
static void _cellular_status_finalize_info(cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    error = _cellular_status_first_public_error(info);

    info->valid           = true;
    info->partial         = error != 0;
    info->published_ms    = now_ms;
    info->last_attempt_ms = now_ms;
    info->last_error      = error;
    info->generation++;

    if (!info->partial)
    {
        info->last_complete_ms = now_ms;
    }
}

/**
 * @brief 原子发布内部事实快照和下一次状态确认期限。
 */
static int _cellular_status_publish(const cellular_status_info_t *info, const cellular_status_deadlines_t *deadlines)
{
    bool previous_partial;
    int  previous_error;

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized || !g_cellular_status.started || g_cellular_status.channel == NULL)
    {
        pthread_mutex_unlock(&g_cellular_status.lock);
        return -ENETDOWN;
    }

    previous_partial = g_cellular_status.info.partial;
    previous_error   = g_cellular_status.info.last_error;

    g_cellular_status.info      = *info;
    g_cellular_status.deadlines = *deadlines;

    pthread_mutex_unlock(&g_cellular_status.lock);

    if (previous_error != info->last_error)
    {
        if (info->last_error == 0)
        {
            LINKG_LOG_INFO("CELL-STATUS: public facts complete, generation=%llu",
                           (unsigned long long)info->generation);
        }
        else
        {
            LINKG_LOG_WARN("CELL-STATUS: public facts partial, error=%d, generation=%llu",
                           info->last_error,
                           (unsigned long long)info->generation);
        }
    }
    else if (previous_partial != info->partial)
    {
        LINKG_LOG_DEBUG("CELL-STATUS: partial state changed, partial=%d, generation=%llu",
                        info->partial ? 1 : 0,
                        (unsigned long long)info->generation);
    }

    return 0;
}

/**
 * @brief 获取两个已经确认事实更新时间中更早的一个。
 */
static uint64_t _cellular_status_pair_updated_ms(const cellular_status_meta_t *left, const cellular_status_meta_t *right)
{
    if (left == NULL || right == NULL || !left->confirmed || !right->confirmed)
    {
        return 0U;
    }

    return left->updated_ms < right->updated_ms ? left->updated_ms : right->updated_ms;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化无线程蜂窝状态事实模块。
 */
int cellular_status_init(void)
{
    int ret;

    pthread_mutex_lock(&g_cellular_status.lock);

    if (g_cellular_status.initialized)
    {
        ret = -EALREADY;
        goto unlock;
    }

    _cellular_status_reset_context_locked();
    g_cellular_status.initialized = true;
    ret = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/**
 * @brief 启动蜂窝状态事实模块并将全部基础Watchdog置为立即到期。
 *
 * @note PDP地址和模组期望Host网络参数不主动启动周期查询，仅由父事实或Owner触发，失败后进入内部重试。
 * @note channel为借用引用，只能由network-cell Owner串行调用process和生命周期接口。
 */
int cellular_status_start(at_channel_t *channel)
{
    uint64_t now_ms;
    int      ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    now_ms = linkg_time_elapsed_ms();
    if (now_ms == 0U)
    {
        now_ms = 1U;
    }

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    if (g_cellular_status.started)
    {
        ret = -EALREADY;
        goto unlock;
    }

    _cellular_status_info_init(&g_cellular_status.info);
    memset(&g_cellular_status.deadlines, 0, sizeof(g_cellular_status.deadlines));

    g_cellular_status.channel                   = channel;
    g_cellular_status.deadlines.network_mode_ms = now_ms;
    g_cellular_status.deadlines.sim_ms          = now_ms;
    g_cellular_status.deadlines.registration_ms = now_ms;
    g_cellular_status.deadlines.radio_ms        = now_ms;
    g_cellular_status.deadlines.pdp_ms          = now_ms;
    g_cellular_status.deadlines.netdev_ms       = now_ms;
    g_cellular_status.deadlines.host_ms         = now_ms;
    g_cellular_status.started                   = true;
    ret                                         = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/**
 * @brief 停止蜂窝状态事实模块并释放借用AT通道。
 */
int cellular_status_stop(void)
{
    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        pthread_mutex_unlock(&g_cellular_status.lock);
        return 0;
    }

    g_cellular_status.started = false;
    g_cellular_status.channel = NULL;
    memset(&g_cellular_status.deadlines, 0, sizeof(g_cellular_status.deadlines));

    pthread_mutex_unlock(&g_cellular_status.lock);

    return 0;
}

/**
 * @brief 反初始化蜂窝状态事实模块。
 */
int cellular_status_deinit(void)
{
    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        pthread_mutex_unlock(&g_cellular_status.lock);
        return 0;
    }

    g_cellular_status.started = false;
    g_cellular_status.channel = NULL;
    memset(&g_cellular_status.deadlines, 0, sizeof(g_cellular_status.deadlines));
    _cellular_status_info_init(&g_cellular_status.info);
    g_cellular_status.initialized = false;

    pthread_mutex_unlock(&g_cellular_status.lock);

    return 0;
}

/****************************** 定时处理 ******************************/

/**
 * @brief 获取蜂窝状态模块最近的状态确认绝对到期期限。
 */
uint64_t cellular_status_get_deadline(void)
{
    uint64_t deadline;

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized || !g_cellular_status.started)
    {
        pthread_mutex_unlock(&g_cellular_status.lock);
        return 0U;
    }

    deadline = _cellular_status_min_deadline(g_cellular_status.deadlines.network_mode_ms, g_cellular_status.deadlines.sim_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.registration_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.radio_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.pdp_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.pdp_address_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.netdev_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.expected_network_ms);
    deadline = _cellular_status_min_deadline(deadline, g_cellular_status.deadlines.host_ms);

    pthread_mutex_unlock(&g_cellular_status.lock);

    return deadline;
}

/**
 * @brief 处理定向刷新请求和所有已经到期的状态确认任务。
 *
 * @note 本接口只采集并发布事实，不执行拨号、接口修复、Modem配置或任何恢复动作。
 */
int cellular_status_process(uint64_t now_ms, cellular_status_refresh_mask_t requested)
{
    cellular_status_deadlines_t    deadlines;
    cellular_status_info_t         info;
    cellular_status_refresh_mask_t mask;
    cellular_status_refresh_mask_t processed;
    at_channel_t                  *channel;
    bool                           refresh_expected;
    bool                           refresh_pdp_address;
    int                            ret;

    if ((requested & ~CELLULAR_STATUS_REFRESH_ALL) != 0U)
    {
        return -EINVAL;
    }

    ret = _cellular_status_get_runtime(&channel, &info, &deadlines);
    if (ret != 0)
    {
        return ret;
    }

    mask = requested | _cellular_status_due_mask(&deadlines, now_ms);
    if (mask == CELLULAR_STATUS_REFRESH_NONE)
    {
        return 0;
    }

    processed           = CELLULAR_STATUS_REFRESH_NONE;
    refresh_expected    = false;
    refresh_pdp_address = false;

    if ((mask & CELLULAR_STATUS_REFRESH_NETWORK_MODE) != 0U)
    {
        _cellular_status_refresh_network_mode(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_NETWORK_MODE;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_SIM) != 0U)
    {
        _cellular_status_refresh_sim(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_SIM;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_REGISTRATION) != 0U)
    {
        _cellular_status_refresh_registration(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_REGISTRATION;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_RADIO) != 0U)
    {
        _cellular_status_refresh_radio(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_RADIO;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_PDP) != 0U)
    {
        refresh_pdp_address = _cellular_status_refresh_pdp(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_PDP;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_PDP_ADDRESS) != 0U || refresh_pdp_address)
    {
        _cellular_status_refresh_pdp_address(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_PDP_ADDRESS;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_NETDEV) != 0U)
    {
        refresh_expected = _cellular_status_refresh_netdev(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_NETDEV;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK) != 0U || refresh_expected)
    {
        _cellular_status_refresh_expected_network(channel, &info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK;
    }

    if ((mask & CELLULAR_STATUS_REFRESH_HOST) != 0U)
    {
        _cellular_status_refresh_host(&info, now_ms);
        processed |= CELLULAR_STATUS_REFRESH_HOST;
    }

    _cellular_status_update_deadlines(&deadlines, &info, processed, now_ms);
    _cellular_status_finalize_info(&info, now_ms);

    return _cellular_status_publish(&info, &deadlines);
}

/****************************** 状态读取 ******************************/

/**
 * @brief 获取当前蜂窝内部完整事实快照。
 */
int cellular_status_get_info(cellular_status_info_t *info)
{
    int ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    *info = g_cellular_status.info;
    ret   = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/**
 * @brief 将内部完整事实投影为对外蜂窝运行状态快照。
 */
int cellular_status_get_snapshot(linkg_cellular_status_snapshot_t *snapshot)
{
    cellular_status_info_t info;
    uint64_t               address_updated_ms;
    int                    ret;

    if (snapshot == NULL)
    {
        return -EINVAL;
    }

    ret = cellular_status_get_info(&info);
    if (ret != 0)
    {
        return ret;
    }

    if (!info.valid)
    {
        return -EAGAIN;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    snapshot->partial = info.partial;

    snapshot->local.network_mode = info.local.network_mode_meta.confirmed
        ? info.local.network_mode
        : LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    snapshot->local.sim_state = info.local.sim_meta.confirmed
        ? info.local.sim_state
        : LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    snapshot->local.network_mode_updated_ms = info.local.network_mode_meta.updated_ms;
    snapshot->local.sim_updated_ms          = info.local.sim_meta.updated_ms;

    snapshot->network.registration = info.network.registration_meta.confirmed
        ? info.network.registration
        : LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    snapshot->network.registration_updated_ms = info.network.registration_meta.updated_ms;

    snapshot->network.network_type = info.network.radio_meta.confirmed
        ? info.network.network_type
        : LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
    snapshot->network.network_type_updated_ms = info.network.radio_meta.updated_ms;

    snapshot->network.serving_cell.valid      = info.network.radio_meta.confirmed && info.network.serving_cell_valid;
    snapshot->network.serving_cell.band       = info.network.band;
    snapshot->network.serving_cell.rsrp_dbm   = info.network.rsrp_dbm;
    snapshot->network.serving_cell.rsrp_valid = info.network.rsrp_valid;
    snapshot->network.serving_cell.rsrq_db    = info.network.rsrq_db;
    snapshot->network.serving_cell.rsrq_valid = info.network.rsrq_valid;
    snapshot->network.serving_cell.sinr_db    = info.network.sinr_db;
    snapshot->network.serving_cell.sinr_valid = info.network.sinr_valid;
    snapshot->network.serving_cell.updated_ms = info.network.radio_meta.updated_ms;

    snapshot->data.pdp_valid      = info.pdp.active_meta.confirmed;
    snapshot->data.pdp_active     = info.pdp.active_meta.confirmed && info.pdp.active;
    snapshot->data.pdp_updated_ms = info.pdp.active_meta.updated_ms;

    snapshot->data.ipv4_valid = info.host.ipv4_meta.confirmed && info.host.ipv4_valid;
    snapshot->data.ipv4       = info.host.ipv4;

    snapshot->data.global_ipv6_valid = info.host.ipv6_meta.confirmed && info.host.global_ipv6_valid;
    snapshot->data.global_ipv6       = info.host.global_ipv6;

    address_updated_ms = _cellular_status_pair_updated_ms(&info.host.ipv4_meta, &info.host.ipv6_meta);
    snapshot->data.address_updated_ms = address_updated_ms;

    return 0;
}
