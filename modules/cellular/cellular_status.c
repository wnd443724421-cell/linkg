/**
 * @file cellular_status.c
 * @brief LinkG蜂窝网络内部状态管理实现
 */

#include "cellular_status.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 状态配置 ******************************/

#define CELLULAR_STATUS_THREAD_NAME               "cell-status"   // 状态采集线程名称
#define CELLULAR_STATUS_TICK_INTERVAL_MS           1000U          // 状态线程基础检查周期
#define CELLULAR_STATUS_MODEM_REFRESH_INTERVAL_MS  5000U          // 模组运行状态采集周期
#define CELLULAR_STATUS_LOCAL_REFRESH_INTERVAL_MS  30000U         // 本机低频状态采集周期

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t       lock;        // 状态互斥锁
    linkg_thread_t        thread;      // 状态采集线程
    at_channel_t         *channel;     // 当前AT通信通道
    cellular_status_info_t info;       // 当前唯一状态快照
    bool                  initialized; // 模块是否已经初始化
} cellular_status_context_t;

static cellular_status_context_t g_cellular_status =
{
    .lock = PTHREAD_MUTEX_INITIALIZER
};

/****************************** 状态元数据 ******************************/

/**
 * @brief 初始化单项状态元数据。
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
 * @brief 记录单项状态查询成功。
 */
static void _cellular_status_meta_success(cellular_status_meta_t *meta, uint64_t updated_ms)
{
    if (meta == NULL)
    {
        return;
    }

    meta->valid      = true;
    meta->updated_ms = updated_ms;
    meta->last_error = 0;
}

/**
 * @brief 记录单项状态当前明确不可用。
 * @note 该情况表示查询行为本身成功确认了“当前没有可用值”，不是采集失败。
 */
static void _cellular_status_meta_unavailable(cellular_status_meta_t *meta, uint64_t updated_ms)
{
    if (meta == NULL)
    {
        return;
    }

    meta->valid      = false;
    meta->updated_ms = updated_ms;
    meta->last_error = 0;
}

/**
 * @brief 记录单项状态查询失败。
 * @note 查询失败时保留最近一次成功状态及其更新时间，仅更新last_error。
 */
static void _cellular_status_meta_failure(cellular_status_meta_t *meta, int error)
{
    if (meta == NULL)
    {
        return;
    }

    meta->last_error = error != 0 ? error : -EIO;
}

/****************************** 上下文辅助 ******************************/

/**
 * @brief 初始化内部状态快照默认值。
 */
static void _cellular_status_info_init(cellular_status_info_t *info)
{
    if (info == NULL)
    {
        return;
    }

    memset(info, 0, sizeof(*info));

    info->partial                    = true;
    info->last_error                 = -EAGAIN;
    info->local.network_mode         = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    info->local.sim_state            = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    info->network.registration       = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    info->network.serving_cell.network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
    info->modem_data.netdev.type     = RG255_NETDEV_TYPE_UNKNOWN;

    _cellular_status_meta_init(&info->local.network_mode_meta);
    _cellular_status_meta_init(&info->local.sim_meta);
    _cellular_status_meta_init(&info->network.registration_meta);
    _cellular_status_meta_init(&info->network.serving_cell_meta);
    _cellular_status_meta_init(&info->modem_data.pdp_meta);
    _cellular_status_meta_init(&info->modem_data.pdp_address_meta);
    _cellular_status_meta_init(&info->modem_data.netdev_meta);
    _cellular_status_meta_init(&info->modem_data.expected_ipv4_meta);
    _cellular_status_meta_init(&info->modem_data.expected_ipv6_meta);
    _cellular_status_meta_init(&info->host.meta);
}

/**
 * @brief 清空模块运行上下文。
 * @note 调用方必须持有状态互斥锁，且必须先停止并释放状态线程资源。
 */
static void _cellular_status_reset_context_locked(void)
{
    memset(&g_cellular_status.thread, 0, sizeof(g_cellular_status.thread));
    _cellular_status_info_init(&g_cellular_status.info);

    g_cellular_status.channel     = NULL;
    g_cellular_status.initialized = false;
}

/**
 * @brief 获取当前AT通道和状态快照副本。
 */
static int _cellular_status_get_runtime(at_channel_t **channel, cellular_status_info_t *info)
{
    int ret;

    if (channel == NULL || info == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    if (g_cellular_status.channel == NULL)
    {
        ret = -ENOTCONN;
        goto unlock;
    }

    *channel = g_cellular_status.channel;
    *info    = g_cellular_status.info;
    ret      = 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/****************************** 本机状态采集 ******************************/

/**
 * @brief 刷新RG255当前网络搜索模式。
 */
static void _cellular_status_refresh_network_mode(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    linkg_cellular_network_mode_t mode;
    int                           ret;

    mode = LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    ret = rg255_query_network_mode(channel, &mode);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->local.network_mode_meta, ret);
        return;
    }

    info->local.network_mode = mode;
    _cellular_status_meta_success(&info->local.network_mode_meta, updated_ms);
}

/**
 * @brief 刷新RG255当前SIM状态。
 */
static void _cellular_status_refresh_sim(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    linkg_cellular_sim_state_t state;
    int                         ret;

    state = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    ret = rg255_query_sim_state(channel, &state);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->local.sim_meta, ret);
        return;
    }

    info->local.sim_state = state;
    _cellular_status_meta_success(&info->local.sim_meta, updated_ms);
}

/**
 * @brief 刷新本机低频状态。
 */
static void _cellular_status_refresh_local(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    _cellular_status_refresh_network_mode(channel, info, updated_ms);
    _cellular_status_refresh_sim(channel, info, updated_ms);
}

/****************************** 移动网络状态采集 ******************************/

/**
 * @brief 刷新当前服务小区状态。
 */
static void _cellular_status_refresh_serving_cell(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    rg255_serving_cell_info_t serving_cell;
    int                       ret;

    memset(&serving_cell, 0, sizeof(serving_cell));
    serving_cell.network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;

    ret = rg255_query_serving_cell(channel, &serving_cell);
    if (ret == -ENODATA)
    {
        memset(&info->network.serving_cell, 0, sizeof(info->network.serving_cell));
        info->network.serving_cell.network_type = LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
        _cellular_status_meta_unavailable(&info->network.serving_cell_meta, updated_ms);
        return;
    }

    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->network.serving_cell_meta, ret);
        return;
    }

    info->network.serving_cell = serving_cell;
    _cellular_status_meta_success(&info->network.serving_cell_meta, updated_ms);
}

/**
 * @brief 刷新当前网络注册状态。
 */
static void _cellular_status_refresh_registration(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    linkg_cellular_network_mode_t         mode;
    linkg_cellular_network_type_t         network_type;
    linkg_cellular_registration_state_t   state;
    int                                   ret;

    mode = info->local.network_mode_meta.valid && info->local.network_mode_meta.last_error == 0
        ? info->local.network_mode
        : LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;

    network_type = info->network.serving_cell_meta.valid && info->network.serving_cell_meta.last_error == 0
        ? info->network.serving_cell.network_type
        : LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;

    state = LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    ret = rg255_query_registration(channel, mode, network_type, &state);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->network.registration_meta, ret);
        return;
    }

    info->network.registration = state;
    _cellular_status_meta_success(&info->network.registration_meta, updated_ms);
}

/****************************** 模组数据状态采集 ******************************/

/**
 * @brief 刷新默认PDP上下文激活状态。
 */
static void _cellular_status_refresh_pdp_active(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    bool active;
    int  ret;

    active = false;
    ret = rg255_query_pdp_active(channel, &active);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->modem_data.pdp_meta, ret);
        return;
    }

    info->modem_data.pdp_active = active;
    _cellular_status_meta_success(&info->modem_data.pdp_meta, updated_ms);
}

/**
 * @brief 刷新默认PDP上下文地址。
 */
static void _cellular_status_refresh_pdp_address(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    rg255_pdp_address_t address;
    int                 ret;

    memset(&address, 0, sizeof(address));

    ret = rg255_query_pdp_address(channel, &address);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->modem_data.pdp_address_meta, ret);
        return;
    }

    info->modem_data.pdp_address = address;
    _cellular_status_meta_success(&info->modem_data.pdp_address_meta, updated_ms);
}

/**
 * @brief 刷新USB网卡连接状态。
 */
static void _cellular_status_refresh_netdev(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    rg255_netdev_status_t status;
    int                   ret;

    memset(&status, 0, sizeof(status));
    status.type = RG255_NETDEV_TYPE_UNKNOWN;

    ret = rg255_query_netdev_status(channel, &status);
    if (ret != 0)
    {
        _cellular_status_meta_failure(&info->modem_data.netdev_meta, ret);
        return;
    }

    info->modem_data.netdev = status;
    _cellular_status_meta_success(&info->modem_data.netdev_meta, updated_ms);
}

/**
 * @brief 清空RG255提供给Host的期望网络参数。
 */
static void _cellular_status_clear_expected_network(cellular_status_info_t *info, uint64_t updated_ms)
{
    memset(&info->modem_data.expected_ipv4, 0, sizeof(info->modem_data.expected_ipv4));
    memset(&info->modem_data.expected_ipv6, 0, sizeof(info->modem_data.expected_ipv6));

    _cellular_status_meta_unavailable(&info->modem_data.expected_ipv4_meta, updated_ms);
    _cellular_status_meta_unavailable(&info->modem_data.expected_ipv6_meta, updated_ms);
}

/**
 * @brief 刷新RG255提供给Host的IPv4和IPv6期望网络参数。
 * @note 只有本轮明确确认QNETDEV已连接时才查询netmaskset参数。
 */
static void _cellular_status_refresh_expected_network(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    rg255_network_card_ipv4_info_t ipv4;
    rg255_network_card_ipv6_info_t ipv6;
    int                            ret;

    if (!info->modem_data.netdev_meta.valid || info->modem_data.netdev_meta.last_error != 0)
    {
        if (info->modem_data.netdev_meta.last_error != 0)
        {
            _cellular_status_meta_failure(&info->modem_data.expected_ipv4_meta, info->modem_data.netdev_meta.last_error);
            _cellular_status_meta_failure(&info->modem_data.expected_ipv6_meta, info->modem_data.netdev_meta.last_error);
        }

        return;
    }

    if (!info->modem_data.netdev.connected)
    {
        _cellular_status_clear_expected_network(info, updated_ms);
        return;
    }

    memset(&ipv4, 0, sizeof(ipv4));
    ret = rg255_query_network_card_ipv4(channel, &ipv4);
    if (ret == 0)
    {
        info->modem_data.expected_ipv4 = ipv4;
        _cellular_status_meta_success(&info->modem_data.expected_ipv4_meta, updated_ms);
    }
    else
    {
        _cellular_status_meta_failure(&info->modem_data.expected_ipv4_meta, ret);
    }

    memset(&ipv6, 0, sizeof(ipv6));
    ret = rg255_query_network_card_ipv6(channel, &ipv6);
    if (ret == 0)
    {
        info->modem_data.expected_ipv6 = ipv6;
        _cellular_status_meta_success(&info->modem_data.expected_ipv6_meta, updated_ms);
    }
    else
    {
        _cellular_status_meta_failure(&info->modem_data.expected_ipv6_meta, ret);
    }
}

/**
 * @brief 刷新模组运行状态。
 */
static void _cellular_status_refresh_modem(at_channel_t *channel, cellular_status_info_t *info, uint64_t updated_ms)
{
    _cellular_status_refresh_serving_cell(channel, info, updated_ms);
    _cellular_status_refresh_registration(channel, info, updated_ms);
    _cellular_status_refresh_pdp_active(channel, info, updated_ms);
    _cellular_status_refresh_pdp_address(channel, info, updated_ms);
    _cellular_status_refresh_netdev(channel, info, updated_ms);
    _cellular_status_refresh_expected_network(channel, info, updated_ms);
}

/****************************** Host网络状态采集 ******************************/

/**
 * @brief 清空Host接口相关动态网络状态。
 */
static void _cellular_status_clear_host_network(cellular_status_host_network_info_t *host)
{
    if (host == NULL)
    {
        return;
    }

    host->link_up            = false;
    host->ipv4_valid         = false;
    host->ipv4_netmask_valid = false;
    host->ipv4_gateway_valid = false;
    host->global_ipv6_valid  = false;
    host->ipv6_gateway_valid = false;

    memset(&host->ipv4, 0, sizeof(host->ipv4));
    memset(&host->ipv4_netmask, 0, sizeof(host->ipv4_netmask));
    memset(&host->ipv4_gateway, 0, sizeof(host->ipv4_gateway));
    memset(&host->global_ipv6, 0, sizeof(host->global_ipv6));
    memset(&host->ipv6_gateway, 0, sizeof(host->ipv6_gateway));
}

/**
 * @brief 判断Host地址查询错误是否表示当前没有对应地址。
 */
static bool _cellular_status_host_address_absent(int error)
{
    return error == -EADDRNOTAVAIL || error == -ENOENT;
}

/**
 * @brief 记录Host状态采集过程中遇到的第一个真实错误。
 */
static void _cellular_status_record_first_error(int error, int *first_error)
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
 * @brief 刷新Linux蜂窝接口实际网络状态。
 */
static void _cellular_status_refresh_host(cellular_status_info_t *info, uint64_t updated_ms)
{
    cellular_status_host_network_info_t *host;
    struct in6_addr                      ipv6;
    struct in_addr                       ipv4;
    bool                                 up;
    int                                  first_error;
    int                                  ret;

    host = &info->host;
    first_error = 0;

    host->interface_present = linkg_network_interface_exists(LINKG_RESOURCE_INTERFACE_CELLULAR);
    if (!host->interface_present)
    {
        _cellular_status_clear_host_network(host);
        _cellular_status_meta_success(&host->meta, updated_ms);
        return;
    }

    up = false;
    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_CELLULAR, &up);
    if (ret == 0)
    {
        host->link_up = up;
    }
    else
    {
        _cellular_status_record_first_error(ret, &first_error);
    }

    memset(&ipv4, 0, sizeof(ipv4));
    ret = linkg_network_interface_get_ipv4(LINKG_RESOURCE_INTERFACE_CELLULAR, &ipv4);
    if (ret == 0)
    {
        host->ipv4       = ipv4;
        host->ipv4_valid = linkg_network_ipv4_address_valid(&ipv4);
    }
    else if (_cellular_status_host_address_absent(ret))
    {
        host->ipv4_valid = false;
        memset(&host->ipv4, 0, sizeof(host->ipv4));
    }
    else
    {
        _cellular_status_record_first_error(ret, &first_error);
    }

    memset(&ipv4, 0, sizeof(ipv4));
    ret = linkg_network_interface_get_ipv4_netmask(LINKG_RESOURCE_INTERFACE_CELLULAR, &ipv4);
    if (ret == 0)
    {
        host->ipv4_netmask       = ipv4;
        host->ipv4_netmask_valid = linkg_network_ipv4_netmask_valid(&ipv4);
    }
    else if (_cellular_status_host_address_absent(ret))
    {
        host->ipv4_netmask_valid = false;
        memset(&host->ipv4_netmask, 0, sizeof(host->ipv4_netmask));
    }
    else
    {
        _cellular_status_record_first_error(ret, &first_error);
    }

    memset(&ipv4, 0, sizeof(ipv4));
    ret = linkg_network_route_get_ipv4_default_gateway(LINKG_RESOURCE_INTERFACE_CELLULAR, &ipv4);
    if (ret == 0)
    {
        host->ipv4_gateway       = ipv4;
        host->ipv4_gateway_valid = linkg_network_ipv4_address_valid(&ipv4);
    }
    else if (_cellular_status_host_address_absent(ret))
    {
        host->ipv4_gateway_valid = false;
        memset(&host->ipv4_gateway, 0, sizeof(host->ipv4_gateway));
    }
    else
    {
        _cellular_status_record_first_error(ret, &first_error);
    }

    memset(&ipv6, 0, sizeof(ipv6));
    if (info->modem_data.expected_ipv6_meta.valid &&
        info->modem_data.expected_ipv6_meta.last_error == 0)
    {
        ret = linkg_network_interface_get_global_ipv6_in_prefix(
            LINKG_RESOURCE_INTERFACE_CELLULAR,
            &info->modem_data.expected_ipv6.prefix,
            info->modem_data.expected_ipv6.prefix_length,
            &ipv6);
    }
    else
    {
        ret = linkg_network_interface_get_global_ipv6(LINKG_RESOURCE_INTERFACE_CELLULAR, &ipv6);
    }

    if (ret == 0)
    {
        host->global_ipv6       = ipv6;
        host->global_ipv6_valid = true;
    }
    else if (_cellular_status_host_address_absent(ret))
    {
        host->global_ipv6_valid = false;
        memset(&host->global_ipv6, 0, sizeof(host->global_ipv6));
    }
    else
    {
        _cellular_status_record_first_error(ret, &first_error);
    }

    memset(&ipv6, 0, sizeof(ipv6));
    ret = linkg_network_route_get_ipv6_default_gateway(LINKG_RESOURCE_INTERFACE_CELLULAR, &ipv6);
    if (ret == 0)
    {
        host->ipv6_gateway       = ipv6;
        host->ipv6_gateway_valid = !IN6_IS_ADDR_UNSPECIFIED(&ipv6) && !IN6_IS_ADDR_MULTICAST(&ipv6);
    }
    else if (_cellular_status_host_address_absent(ret))
    {
        host->ipv6_gateway_valid = false;
        memset(&host->ipv6_gateway, 0, sizeof(host->ipv6_gateway));
    }
    else
    {
        _cellular_status_record_first_error(ret, &first_error);
    }

    if (first_error == 0)
    {
        _cellular_status_meta_success(&host->meta, updated_ms);
    }
    else
    {
        _cellular_status_meta_failure(&host->meta, first_error);
    }
}

/****************************** 快照状态 ******************************/

/**
 * @brief 获取当前快照中第一个采集错误。
 */
static int _cellular_status_first_error(const cellular_status_info_t *info)
{
    const cellular_status_meta_t *metas[] =
    {
        &info->local.network_mode_meta,
        &info->local.sim_meta,
        &info->network.registration_meta,
        &info->network.serving_cell_meta,
        &info->modem_data.pdp_meta,
        &info->modem_data.pdp_address_meta,
        &info->modem_data.netdev_meta,
        &info->modem_data.expected_ipv4_meta,
        &info->modem_data.expected_ipv6_meta,
        &info->host.meta
    };
    size_t index;

    for (index = 0U; index < sizeof(metas) / sizeof(metas[0]); index++)
    {
        if (metas[index]->last_error != 0)
        {
            return metas[index]->last_error;
        }
    }

    return 0;
}

/**
 * @brief 完成本轮状态快照的整体元数据更新。
 */
static void _cellular_status_finalize_info(cellular_status_info_t *info, uint64_t attempt_ms)
{
    int error;

    error = _cellular_status_first_error(info);

    info->valid           = true;
    info->partial         = error != 0;
    info->last_attempt_ms = attempt_ms;
    info->last_error      = error;
    info->generation++;

    if (!info->partial)
    {
        info->last_success_ms = attempt_ms;
    }
}

/**
 * @brief 发布本轮完整内部状态快照。
 */
static void _cellular_status_publish(const cellular_status_info_t *info)
{
    bool     previous_partial;
    uint64_t generation;
    int      previous_error;
    bool     previous_valid;

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized || g_cellular_status.channel == NULL)
    {
        pthread_mutex_unlock(&g_cellular_status.lock);
        return;
    }

    previous_valid   = g_cellular_status.info.valid;
    previous_partial = g_cellular_status.info.partial;
    previous_error   = g_cellular_status.info.last_error;

    g_cellular_status.info = *info;
    generation             = g_cellular_status.info.generation;

    pthread_mutex_unlock(&g_cellular_status.lock);

    if (!previous_valid)
    {
        LINKG_LOG_DEBUG("CELL-STATUS: initial snapshot published, partial=%d, error=%d, generation=%llu",
                        info->partial ? 1 : 0,
                        info->last_error,
                        (unsigned long long)generation);
        return;
    }

    if (previous_error != info->last_error)
    {
        if (info->last_error == 0)
        {
            LINKG_LOG_INFO("CELL-STATUS: status collection recovered, generation=%llu",
                           (unsigned long long)generation);
        }
        else
        {
            LINKG_LOG_WARN("CELL-STATUS: status collection partial, error=%d, generation=%llu",
                           info->last_error,
                           (unsigned long long)generation);
        }
        return;
    }

    if (previous_partial != info->partial)
    {
        LINKG_LOG_DEBUG("CELL-STATUS: snapshot partial state changed, partial=%d, generation=%llu",
                        info->partial ? 1 : 0,
                        (unsigned long long)generation);
    }
}

/**
 * @brief 执行一次状态采集并发布全局唯一快照。
 * @note 本函数只采集和保存事实，不进行拨号、DHCP、接口重置或任何恢复动作。
 */
static int _cellular_status_refresh(bool refresh_modem, bool refresh_local)
{
    cellular_status_info_t info;
    at_channel_t          *channel;
    uint64_t               attempt_ms;
    int                    ret;

    ret = _cellular_status_get_runtime(&channel, &info);
    if (ret != 0)
    {
        return ret;
    }

    attempt_ms = linkg_time_elapsed_ms();

    if (refresh_local)
    {
        _cellular_status_refresh_local(channel, &info, attempt_ms);
    }

    if (refresh_modem)
    {
        _cellular_status_refresh_modem(channel, &info, attempt_ms);
    }

    _cellular_status_refresh_host(&info, attempt_ms);
    _cellular_status_finalize_info(&info, attempt_ms);
    _cellular_status_publish(&info);

    return info.last_error;
}

/****************************** 线程等待 ******************************/

/**
 * @brief 等待下一次状态检查周期或线程停止唤醒。
 */
static int _cellular_status_wait(linkg_thread_t *thread)
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
        ret = poll(&descriptor, 1U, (int)CELLULAR_STATUS_TICK_INTERVAL_MS);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (ret < 0)
    {
        return -errno;
    }

    if (ret == 0)
    {
        return 0;
    }

    if ((descriptor.revents & POLLIN) != 0)
    {
        return linkg_thread_clear_wakeup(thread);
    }

    if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    return 0;
}

/****************************** 状态线程 ******************************/

/**
 * @brief 蜂窝状态采集线程。
 */
static void _cellular_status_thread(linkg_thread_t *thread, void *user_data)
{
    uint64_t last_local_ms;
    uint64_t last_modem_ms;
    uint64_t now_ms;
    bool     local_collected;
    bool     modem_collected;
    bool     refresh_local;
    bool     refresh_modem;
    int      ret;

    (void)user_data;

    last_local_ms   = 0U;
    last_modem_ms   = 0U;
    local_collected = false;
    modem_collected = false;

    LINKG_LOG_DEBUG("CELL-STATUS: collection thread entered");

    while (linkg_thread_is_running(thread))
    {
        now_ms = linkg_time_elapsed_ms();

        refresh_local = !local_collected ||
            now_ms - last_local_ms >= CELLULAR_STATUS_LOCAL_REFRESH_INTERVAL_MS;

        refresh_modem = !modem_collected ||
            now_ms - last_modem_ms >= CELLULAR_STATUS_MODEM_REFRESH_INTERVAL_MS;

        (void)_cellular_status_refresh(refresh_modem, refresh_local);

        if (refresh_local)
        {
            local_collected = true;
            last_local_ms   = now_ms;
        }

        if (refresh_modem)
        {
            modem_collected = true;
            last_modem_ms   = now_ms;
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        ret = _cellular_status_wait(thread);
        if (ret == 0)
        {
            continue;
        }

        if (linkg_thread_is_running(thread))
        {
            LINKG_LOG_ERROR("CELL-STATUS: collection wait failed, error=%d", ret);
        }

        break;
    }

    pthread_mutex_lock(&g_cellular_status.lock);
    g_cellular_status.info.running = false;
    pthread_mutex_unlock(&g_cellular_status.lock);

    LINKG_LOG_DEBUG("CELL-STATUS: collection thread exited");
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝状态模块。
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

    ret = linkg_thread_init(&g_cellular_status.thread, CELLULAR_STATUS_THREAD_NAME, _cellular_status_thread, NULL);
    if (ret != 0)
    {
        _cellular_status_reset_context_locked();
        goto unlock;
    }

    g_cellular_status.initialized = true;

    pthread_mutex_unlock(&g_cellular_status.lock);

    LINKG_LOG_DEBUG("CELL-STATUS: module initialized, tick_ms=%u, modem_ms=%u, local_ms=%u",
                    CELLULAR_STATUS_TICK_INTERVAL_MS,
                    CELLULAR_STATUS_MODEM_REFRESH_INTERVAL_MS,
                    CELLULAR_STATUS_LOCAL_REFRESH_INTERVAL_MS);

    return 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/**
 * @brief 启动蜂窝状态采集线程。
 * @note channel在本次start到stop期间必须保持有效，调用方销毁AT通道前必须先停止本模块。
 */
int cellular_status_start(at_channel_t *channel)
{
    int ret;

    if (channel == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        ret = -ENODEV;
        goto unlock;
    }

    if (linkg_thread_is_started(&g_cellular_status.thread))
    {
        ret = -EALREADY;
        goto unlock;
    }

    g_cellular_status.channel      = channel;
    g_cellular_status.info.running = true;

    ret = linkg_thread_start(&g_cellular_status.thread);
    if (ret != 0)
    {
        g_cellular_status.channel      = NULL;
        g_cellular_status.info.running = false;
        goto unlock;
    }

    pthread_mutex_unlock(&g_cellular_status.lock);

    LINKG_LOG_DEBUG("CELL-STATUS: module started");

    return 0;

unlock:
    pthread_mutex_unlock(&g_cellular_status.lock);

    return ret;
}

/**
 * @brief 停止蜂窝状态采集线程。
 */
int cellular_status_stop(void)
{
    bool initialized;
    bool started;
    int  ret;

    pthread_mutex_lock(&g_cellular_status.lock);

    initialized = g_cellular_status.initialized;
    started     = initialized && linkg_thread_is_started(&g_cellular_status.thread);

    pthread_mutex_unlock(&g_cellular_status.lock);

    if (!initialized)
    {
        return 0;
    }

    if (started)
    {
        ret = linkg_thread_stop(&g_cellular_status.thread);
        if (ret != 0 && linkg_thread_is_started(&g_cellular_status.thread))
        {
            return ret;
        }

        if (ret != 0)
        {
            LINKG_LOG_WARN("CELL-STATUS: status thread stopped with cleanup error, error=%d", ret);
        }
    }

    pthread_mutex_lock(&g_cellular_status.lock);
    g_cellular_status.channel      = NULL;
    g_cellular_status.info.running = false;
    pthread_mutex_unlock(&g_cellular_status.lock);

    LINKG_LOG_DEBUG("CELL-STATUS: module stopped");

    return 0;
}

/**
 * @brief 反初始化蜂窝状态模块。
 */
void cellular_status_deinit(void)
{
    int ret;

    ret = cellular_status_stop();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("CELL-STATUS: stop before deinit failed, error=%d", ret);
        return;
    }

    pthread_mutex_lock(&g_cellular_status.lock);

    if (!g_cellular_status.initialized)
    {
        pthread_mutex_unlock(&g_cellular_status.lock);
        return;
    }

    linkg_thread_deinit(&g_cellular_status.thread);
    _cellular_status_reset_context_locked();

    pthread_mutex_unlock(&g_cellular_status.lock);

    LINKG_LOG_DEBUG("CELL-STATUS: module deinitialized");
}

/****************************** 状态读取 ******************************/

/**
 * @brief 获取当前蜂窝内部完整状态快照。
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
 * @brief 将内部完整状态转换为对外蜂窝状态快照。
 */
int cellular_status_get_snapshot(linkg_cellular_status_snapshot_t *snapshot)
{
    cellular_status_info_t info;
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

    memset(snapshot, 0, sizeof(*snapshot));

    snapshot->partial = info.partial;

    snapshot->local.network_mode = info.local.network_mode_meta.valid
        ? info.local.network_mode
        : LINKG_CELLULAR_NETWORK_MODE_UNKNOWN;
    snapshot->local.sim_state = info.local.sim_meta.valid
        ? info.local.sim_state
        : LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    snapshot->local.network_mode_updated_ms = info.local.network_mode_meta.updated_ms;
    snapshot->local.sim_updated_ms          = info.local.sim_meta.updated_ms;

    snapshot->network.registration = info.network.registration_meta.valid
        ? info.network.registration
        : LINKG_CELLULAR_REGISTRATION_STATE_UNKNOWN;
    snapshot->network.registration_updated_ms = info.network.registration_meta.updated_ms;

    snapshot->network.network_type = info.network.serving_cell_meta.valid
        ? info.network.serving_cell.network_type
        : LINKG_CELLULAR_NETWORK_TYPE_UNKNOWN;
    snapshot->network.network_type_updated_ms = info.network.serving_cell_meta.updated_ms;

    snapshot->network.serving_cell.valid      = info.network.serving_cell_meta.valid;
    snapshot->network.serving_cell.band       = info.network.serving_cell.band;
    snapshot->network.serving_cell.rsrp_dbm   = info.network.serving_cell.rsrp_dbm;
    snapshot->network.serving_cell.rsrp_valid = info.network.serving_cell.rsrp_valid;
    snapshot->network.serving_cell.rsrq_db    = info.network.serving_cell.rsrq_db;
    snapshot->network.serving_cell.rsrq_valid = info.network.serving_cell.rsrq_valid;
    snapshot->network.serving_cell.sinr_db    = info.network.serving_cell.sinr_db;
    snapshot->network.serving_cell.sinr_valid = info.network.serving_cell.sinr_valid;
    snapshot->network.serving_cell.updated_ms = info.network.serving_cell_meta.updated_ms;

    snapshot->data.pdp_valid      = info.modem_data.pdp_meta.valid;
    snapshot->data.pdp_active     = info.modem_data.pdp_meta.valid && info.modem_data.pdp_active;
    snapshot->data.pdp_updated_ms = info.modem_data.pdp_meta.updated_ms;

    snapshot->data.ipv4_valid = info.host.meta.valid && info.host.ipv4_valid;
    snapshot->data.ipv4       = info.host.ipv4;

    snapshot->data.global_ipv6_valid = info.host.meta.valid && info.host.global_ipv6_valid;
    snapshot->data.global_ipv6       = info.host.global_ipv6;
    snapshot->data.address_updated_ms = info.host.meta.updated_ms;

    return 0;
}
