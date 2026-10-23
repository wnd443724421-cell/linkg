
/**
 * @file linkg_network.c
 * @brief LinkG网络服务实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-08-26
 */

#include "linkg_network.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <arpa/inet.h>

#include "linkg_config.h"
#include "linkg_ethernet.h"
#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

#include "network_cellular.h"
#include "network_internal.h"
#include "network_manager.h"
#include "network_wifi.h"
#include "linkg_wifi.h"
#include "linkg_switch.h"

/****************************** 模块常量 ******************************/

#define LINKG_NETWORK_NODE_ADDRESS_PREFIX     32U    // 本机节点IPv4地址固定前缀
#define LINKG_NETWORK_SYSTEM_READY_TIMEOUT_MS 10000U // 最长等待系统基础网络10秒
#define LINKG_NETWORK_SYSTEM_READY_POLL_MS    50U    // 每50ms检查一次
#define LINKG_NETWORK_SYSTEM_READY_STABLE_MS  250U   // 连续稳定500ms才认为就绪

/****************************** 全局上下文 ******************************/

linkg_network_context_t g_network =
{
    .state = LINKG_NETWORK_STATE_UNINITIALIZED,
    .role  = LINKG_DEVICE_ROLE_UNKNOWN
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 将Network上下文恢复为未初始化状态。
 *
 * 调用前必须确保全部模块资源及状态锁已经释放。
 */
static void _linkg_network_reset_context(void)
{
    memset(&g_network, 0, sizeof(g_network));

    g_network.state = LINKG_NETWORK_STATE_UNINITIALIZED;
    g_network.role  = LINKG_DEVICE_ROLE_UNKNOWN;
}

/**
 * @brief 记录清理阶段出现的首个错误。
 */
static void _linkg_network_record_first_error(int *first_error, int error)
{
    if (first_error != NULL && *first_error == 0 && error != 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 设置Network生命周期状态。
 */
static void _linkg_network_set_state(linkg_network_state_t state)
{
    pthread_mutex_lock(&g_network.lock);

    g_network.state = state;

    pthread_mutex_unlock(&g_network.lock);
}

/****************************** 系统网络就绪 ******************************/

/**
 * @brief 检查系统基础Loopback是否已经就绪。
 *
 * 只有当lo已经UP并且拥有127.0.0.1时才返回ready=true。
 * 本函数只检查系统基础地址，不创建或修改任何网络资源。
 */
static int _linkg_network_system_loopback_ready(bool *ready)
{
    struct in_addr loopback_address;
    bool           address_present;
    bool           interface_up;
    int            ret;

    if (ready == NULL)
    {
        return -EINVAL;
    }

    *ready = false;

    loopback_address.s_addr = htonl(INADDR_LOOPBACK);

    ret = linkg_network_interface_is_up(
        LINKG_RESOURCE_INTERFACE_LOOPBACK,
        &interface_up);

    /**
     * 系统启动早期接口尚未创建时，属于“尚未就绪”，
     * 不是LinkG内部致命错误。
     */
    if (ret == -ENODEV || ret == -ENXIO)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (!interface_up)
    {
        return 0;
    }

    ret = linkg_network_interface_has_ipv4(
        LINKG_RESOURCE_INTERFACE_LOOPBACK,
        &loopback_address,
        &address_present);

    if (ret != 0)
    {
        return ret;
    }

    *ready = address_present;

    return 0;
}

/**
 * @brief 等待系统基础网络完成Loopback配置。
 *
 * 必须在LinkG配置eth0或向lo添加172.28.<node>.1之前调用。
 * 除了要求lo为UP且拥有127.0.0.1，还要求状态连续稳定一段时间，
 * 避免与系统network/netifd初始化发生地址覆盖竞争。
 */
static int _linkg_network_wait_system_ready(void)
{
    uint32_t elapsed_ms;
    uint32_t stable_ms;
    bool     ready;
    bool     waiting_logged;
    int      ret;

    elapsed_ms    = 0U;
    stable_ms     = 0U;
    waiting_logged = false;

    while (elapsed_ms < LINKG_NETWORK_SYSTEM_READY_TIMEOUT_MS)
    {
        ready = false;

        ret = _linkg_network_system_loopback_ready(&ready);
        if (ret != 0)
        {
            return ret;
        }

        if (ready)
        {
            stable_ms += LINKG_NETWORK_SYSTEM_READY_POLL_MS;

            if (stable_ms >=
                LINKG_NETWORK_SYSTEM_READY_STABLE_MS)
            {
                LINKG_LOG_INFO(
                    "system network ready, "
                    "interface=%s, address=127.0.0.1, "
                    "wait_ms=%u",
                    LINKG_RESOURCE_INTERFACE_LOOPBACK,
                    (unsigned int)elapsed_ms);

                return 0;
            }
        }
        else
        {
            stable_ms = 0U;

            if (!waiting_logged)
            {
                LINKG_LOG_WARN(
                    "system network not ready, waiting, "
                    "interface=%s, address=127.0.0.1",
                    LINKG_RESOURCE_INTERFACE_LOOPBACK);

                waiting_logged = true;
            }
        }

        ret = linkg_time_sleep_ms(
            LINKG_NETWORK_SYSTEM_READY_POLL_MS);

        if (ret != 0)
        {
            return ret;
        }

        elapsed_ms += LINKG_NETWORK_SYSTEM_READY_POLL_MS;
    }

    return -ETIMEDOUT;
}

/****************************** 本机节点地址 ******************************/

/**
 * @brief 获取已初始化网络服务的地址快照，避免使用保存后尚未生效的地址。
 */
int linkg_network_get_running_config(linkg_network_config_t *out)
{
    if (out == NULL)
    {
        return -EINVAL;
    }

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    // 地址快照只在init中赋值，所有使用者停止后才由deinit释放。
    *out = g_network.network_config;
    return 0;
}

/**
 * @brief 配置本机节点IPv4地址。
 */
static int _linkg_network_node_address_start(void)
{
    struct in_addr address;
    int            ret;

    if (g_network.node_address_started)
    {
        return 0;
    }

    ret = linkg_network_config_get_node_address(&g_network.network_config, g_network.network_config.node_id, &address);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_interface_add_ipv4(LINKG_RESOURCE_INTERFACE_LOOPBACK, &address, LINKG_NETWORK_NODE_ADDRESS_PREFIX);
    if (ret != 0 && ret != -EEXIST)
    {
        return ret;
    }

    g_network.node_address_started = true;

    return 0;
}

/**
 * @brief 删除本机节点IPv4地址。
 */
static int _linkg_network_node_address_stop(void)
{
    struct in_addr address;
    int            ret;

    if (!g_network.node_address_started)
    {
        return 0;
    }

    ret = linkg_network_config_get_node_address(&g_network.network_config, g_network.network_config.node_id, &address);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_interface_remove_ipv4(LINKG_RESOURCE_INTERFACE_LOOPBACK, &address, LINKG_NETWORK_NODE_ADDRESS_PREFIX);
    if (ret != 0 && ret != -EADDRNOTAVAIL)
    {
        return ret;
    }

    g_network.node_address_started = false;

    return 0;
}

/****************************** IPv4转发 ******************************/

/**
 * @brief 启用IPv4转发。
 */
static int _linkg_network_ipv4_forwarding_start(void)
{
    int ret;

    if (g_network.ipv4_forwarding_enabled)
    {
        return 0;
    }

    ret = linkg_network_ipv4_forwarding_set(true);
    if (ret != 0)
    {
        return ret;
    }

    g_network.ipv4_forwarding_enabled = true;

    return 0;
}

/**
 * @brief 禁用IPv4转发。
 */
static int _linkg_network_ipv4_forwarding_stop(void)
{
    int ret;

    if (!g_network.ipv4_forwarding_enabled)
    {
        return 0;
    }

    ret = linkg_network_ipv4_forwarding_set(false);
    if (ret == 0)
    {
        g_network.ipv4_forwarding_enabled = false;
    }

    return ret;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Network及其内部管理资源。
 *
 * 同步初始化Ethernet和Network Manager线程对象。
 * 不在本阶段初始化WiFi或Cellular模块。
 */
int linkg_network_init(linkg_packet_pool_t *packet_pool)
{
    linkg_network_ipv4_config_t ethernet_config;
    linkg_device_config_t       device_config;
    int                         cleanup_ret;
    int                         ret;

    if (packet_pool == NULL)
    {
        return -EINVAL;
    }

    if (g_network.state != LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -EALREADY;
    }

    _linkg_network_reset_context();

    ret = pthread_mutex_init(&g_network.lock, NULL);
    if (ret != 0)
    {
        _linkg_network_reset_context();
        return -ret;
    }

    ret = linkg_config_get_device(&device_config);
    if (ret != 0)
    {
        goto fail_lock;
    }

    if (device_config.role != LINKG_DEVICE_ROLE_AP && device_config.role != LINKG_DEVICE_ROLE_STA)
    {
        ret = -EINVAL;
        goto fail_lock;
    }

    ret = linkg_config_get_network(&g_network.network_config);
    if (ret != 0)
    {
        goto fail_lock;
    }

    ret = linkg_config_get_wifi(&g_network.wifi_config);
    if (ret != 0)
    {
        goto fail_lock;
    }

    ret = linkg_config_get_cellular(&g_network.cellular_config);
    if (ret != 0)
    {
        goto fail_lock;
    }

    g_network.role        = device_config.role;
    g_network.packet_pool = packet_pool;

    ret = linkg_network_config_get_ethernet(&g_network.network_config, &ethernet_config);
    if (ret != 0)
    {
        goto fail_lock;
    }

    ret = linkg_ethernet_init(&ethernet_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize Ethernet failed, error=%d", ret);
        goto fail_lock;
    }

    g_network.ethernet_initialized = true;

    ret = _linkg_network_manager_init();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("initialize Network Manager failed, error=%d", ret);
        goto fail_ethernet;
    }

    _linkg_network_set_state(LINKG_NETWORK_STATE_STOPPED);

    return 0;

fail_ethernet:
    cleanup_ret = linkg_ethernet_deinit();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback Ethernet initialization failed, error=%d", cleanup_ret);
        return cleanup_ret;
    }

    g_network.ethernet_initialized = false;

fail_lock:
    cleanup_ret = pthread_mutex_destroy(&g_network.lock);
    if (cleanup_ret != 0)
    {
        return -cleanup_ret;
    }

    _linkg_network_reset_context();

    return ret;
}

/**
 * @brief 启动Network服务。
 *
 * Ethernet同步启动；WiFi和Cellular由Manager异步初始化及启动。
 * 不等待WiFi连接或Cellular拨号完成。
 */
int linkg_network_start(void)
{
    linkg_network_state_t state;
    int                   cleanup_ret;
    int                   ret;

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_network.lock);

    state = g_network.state;

    if (state == LINKG_NETWORK_STATE_RUNNING)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EALREADY;
    }

    if (state != LINKG_NETWORK_STATE_STOPPED)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EBUSY;
    }

    g_network.state = LINKG_NETWORK_STATE_STARTING;

    pthread_mutex_unlock(&g_network.lock);

    if (!g_network.ethernet_initialized)
    {
        ret = -ENODEV;
        goto fail;
    }

    ret = _linkg_network_wait_system_ready();
    if (ret != 0)
    {
        LINKG_LOG_ERROR( "wait for system network failed, error=%d", ret);
        _linkg_network_set_state(LINKG_NETWORK_STATE_STOPPED);

        return ret;
    }

    ret = linkg_ethernet_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR(
            "start Ethernet failed, error=%d",
            ret);

        goto fail;
    }

    ret = _linkg_network_node_address_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("configure local node address failed, error=%d", ret);
        goto fail;
    }

    ret = _linkg_network_ipv4_forwarding_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("enable IPv4 forwarding failed, error=%d", ret);
        goto fail;
    }

    ret = _linkg_network_manager_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start Network Manager failed, error=%d", ret);
        goto fail;
    }

    _linkg_network_set_state(LINKG_NETWORK_STATE_RUNNING);

    LINKG_LOG_INFO("network service started");

    return 0;

fail:
    cleanup_ret = _linkg_network_manager_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback Network Manager failed, error=%d", cleanup_ret);
        _linkg_network_set_state(LINKG_NETWORK_STATE_FAILED);
        return ret;
    }

    cleanup_ret = _linkg_network_ipv4_forwarding_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback IPv4 forwarding failed, error=%d", cleanup_ret);
        _linkg_network_set_state(LINKG_NETWORK_STATE_FAILED);
        return ret;
    }

    cleanup_ret = _linkg_network_node_address_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback local node address failed, error=%d", cleanup_ret);
        _linkg_network_set_state(LINKG_NETWORK_STATE_FAILED);
        return ret;
    }

    cleanup_ret = linkg_ethernet_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback Ethernet failed, error=%d", cleanup_ret);
        _linkg_network_set_state(LINKG_NETWORK_STATE_FAILED);
        return ret;
    }

    _linkg_network_set_state(LINKG_NETWORK_STATE_STOPPED);

    return ret;
}

/**
 * @brief 停止Network及全部内部运行资源。
 *
 * Manager先回收WiFi和Cellular Owner，随后停止公共网络资源及Ethernet。
 */
int linkg_network_stop(void)
{
    linkg_network_state_t state;
    int                   first_error;
    int                   ret;

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return 0;
    }

    pthread_mutex_lock(&g_network.lock);

    state = g_network.state;

    if (state == LINKG_NETWORK_STATE_STOPPED)
    {
        pthread_mutex_unlock(&g_network.lock);
        return 0;
    }

    if (state == LINKG_NETWORK_STATE_STARTING || state == LINKG_NETWORK_STATE_STOPPING)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EBUSY;
    }

    if (state != LINKG_NETWORK_STATE_RUNNING && state != LINKG_NETWORK_STATE_FAILED)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EINVAL;
    }

    g_network.state = LINKG_NETWORK_STATE_STOPPING;

    pthread_mutex_unlock(&g_network.lock);

    ret = _linkg_network_manager_stop();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("stop Network Manager failed, error=%d", ret);
        _linkg_network_set_state(LINKG_NETWORK_STATE_FAILED);
        return ret;
    }

    first_error = 0;

    ret = _linkg_network_ipv4_forwarding_stop();
    _linkg_network_record_first_error(&first_error, ret);

    ret = _linkg_network_node_address_stop();
    _linkg_network_record_first_error(&first_error, ret);

    if (g_network.ethernet_initialized)
    {
        ret = linkg_ethernet_stop();
        _linkg_network_record_first_error(&first_error, ret);
    }

    _linkg_network_set_state(first_error == 0 ? LINKG_NETWORK_STATE_STOPPED : LINKG_NETWORK_STATE_FAILED);

    if (first_error != 0)
    {
        LINKG_LOG_ERROR("stop network service failed, error=%d", first_error);
        return first_error;
    }

    LINKG_LOG_INFO("network service stopped");

    return 0;
}

/**
 * @brief 反初始化Network全部内部资源。
 *
 * 必须先停止全部Owner及Ethernet运行资源，最后销毁Network状态锁。
 */
int linkg_network_deinit(void)
{
    linkg_network_state_t state;
    int                   ret;

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return 0;
    }

    pthread_mutex_lock(&g_network.lock);

    state = g_network.state;

    pthread_mutex_unlock(&g_network.lock);

    if (state != LINKG_NETWORK_STATE_STOPPED && state != LINKG_NETWORK_STATE_FAILED)
    {
        return -EBUSY;
    }

    if (g_network.node_address_started || g_network.ipv4_forwarding_enabled)
    {
        return -EBUSY;
    }

    ret = _linkg_network_manager_deinit();
    if (ret != 0)
    {
        return ret;
    }

    if (g_network.ethernet_initialized)
    {
        ret = linkg_ethernet_deinit();
        if (ret != 0)
        {
            return ret;
        }

        g_network.ethernet_initialized = false;
    }

    ret = pthread_mutex_destroy(&g_network.lock);
    if (ret != 0)
    {
        return -ret;
    }

    _linkg_network_reset_context();

    LINKG_LOG_INFO("network service deinitialized");

    return 0;
}

/****************************** 独立重启 ******************************/

/**
 * @brief 请求Wi-Fi完整重启。
 *
 * 仅提交异步重启请求。
 * Maintenance由Network Manager在实际停止旧Owner前执行。
 */
int linkg_network_restart_wifi(void)
{
    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    return _linkg_network_wifi_restart();
}


/**
 * @brief 请求Cellular完整重启。
 *
 * 仅提交异步重启请求。
 * Maintenance由Network Manager在实际停止旧Owner前执行。
 */
int linkg_network_restart_cellular(void)
{
    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    return _linkg_network_cellular_restart();
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取Network整体生命周期状态。
 */
linkg_network_state_t linkg_network_get_state(void)
{
    linkg_network_state_t state;

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return LINKG_NETWORK_STATE_UNINITIALIZED;
    }

    pthread_mutex_lock(&g_network.lock);

    state = g_network.state;

    pthread_mutex_unlock(&g_network.lock);

    return state;
}

/**
 * @brief 更新Network保存的WiFi配置副本。
 */
int linkg_network_set_wifi_config(const linkg_wifi_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_network.lock);

    if (g_network.state != LINKG_NETWORK_STATE_STOPPED &&
        g_network.state != LINKG_NETWORK_STATE_RUNNING)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EBUSY;
    }

    g_network.wifi_config = *config;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}

/**
 * @brief 更新Network保存的Cellular配置副本。
 */
int linkg_network_set_cellular_config(const linkg_cellular_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_network.lock);

    if (g_network.state != LINKG_NETWORK_STATE_STOPPED &&
        g_network.state != LINKG_NETWORK_STATE_RUNNING)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EBUSY;
    }

    g_network.cellular_config = *config;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}

/**
 * @brief 动态修改WiFi窄带速率模式及速率。
 */
int linkg_network_set_wifi_narrow_config(linkg_wifi_narrow_mode_t mode, uint16_t rate)
{
    int ret;

    if (g_network.state == LINKG_NETWORK_STATE_UNINITIALIZED)
    {
        return -ENODEV;
    }

    ret = linkg_wifi_set_narrow_config(mode, rate);
    if (ret != 0)
    {
        return ret;
    }

    if (mode == LINKG_WIFI_NARROW_MODE_ADAPTIVE)
    {
        rate = 0U;
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.wifi_config.wideband.narrow_params.mode        = mode;
    g_network.wifi_config.wideband.narrow_params.manual_rate = rate;

    pthread_mutex_unlock(&g_network.lock);

    return 0;
}
