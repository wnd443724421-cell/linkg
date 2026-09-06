/**
 * @file linkg_network.c
 * @brief LinkG网络服务实现
 * @author Dawn
 * @version 2.0.0
 * @date 2026-08-26
 */

#include "linkg_network.h"

#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_file.h"
#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_wifi.h"
#include "linkg_cellular.h"

#include "network_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_NETWORK_WIFI_THREAD_NAME                   "network-wifi"    // Wi-Fi Owner线程名称
#define LINKG_NETWORK_CELLULAR_THREAD_NAME               "network-cell"    // 蜂窝管理线程名称
#define LINKG_NETWORK_ETHERNET_WAIT_TIMEOUT_MS           3000U             // Ethernet接口等待超时
#define LINKG_NETWORK_ETHERNET_RX_IRQ_CPU_ID             0U                // Ethernet接收IRQ固定CPU编号
#define LINKG_NETWORK_ETHERNET_IRQ_LINE_MAX              512U              // /proc/interrupts单行缓冲区长度
#define LINKG_NETWORK_ETHERNET_DEVICE_NAME_MAX           128U              // Ethernet平台设备名称缓冲区长度
#define LINKG_NETWORK_ETHERNET_IRQ_AFFINITY_VALUE_MAX    32U               // IRQ affinity写入值缓冲区长度
#define LINKG_NETWORK_NODE_ADDRESS_PREFIX                32U               // 本机节点IPv4地址固定前缀

/****************************** 全局上下文 ******************************/

static linkg_network_context_t g_network =
{
    .state = LINKG_NETWORK_STATE_UNINITIALIZED, // 网络服务初始状态
    .role  = LINKG_DEVICE_ROLE_UNKNOWN          // 初始设备角色
};

/****************************** 上下文辅助 ******************************/

/**
 * @brief 将网络服务上下文恢复为未初始化状态。
 *
 * 调用前必须确保互斥锁、条件变量和全部子模块资源已经释放。
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
    if (first_error == NULL)
    {
        return;
    }

    if (*first_error == 0 && error != 0)
    {
        *first_error = error;
    }
}


/****************************** 本机节点地址 ******************************/

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

/****************************** 工作线程辅助 ******************************/

/**
 * @brief 等待普通工作线程停止请求。
 *
 * 仅供没有独立run函数的工作线程使用。
 */
static int _linkg_network_worker_wait_stop(linkg_thread_t *thread)
{
    struct pollfd descriptor;
    int           wakeup_fd;
    int           ret;

    if (thread == NULL)
    {
        return -EINVAL;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    memset(&descriptor, 0, sizeof(descriptor));

    descriptor.fd     = wakeup_fd;
    descriptor.events = POLLIN;

    while (linkg_thread_is_running(thread))
    {
        do
        {
            ret = poll(&descriptor, 1U, -1);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (ret < 0)
        {
            return -errno;
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                return ret;
            }
        }

        descriptor.revents = 0;
    }

    return 0;
}

/**
 * @brief 发布工作线程启动结果并唤醒启动等待方。
 */
static void _linkg_network_worker_publish_start(linkg_network_worker_t *worker, int result)
{
    pthread_mutex_lock(&g_network.lock);

    worker->start_result    = result;
    worker->start_completed = true;

    pthread_cond_broadcast(&g_network.worker_condition);

    pthread_mutex_unlock(&g_network.lock);
}

/**
 * @brief 发布工作线程运行结果，并在异常退出时将网络服务标记为失败。
 */
static void _linkg_network_worker_publish_run(linkg_network_worker_t *worker, linkg_thread_t *thread, int result)
{
    bool unexpected_exit;

    unexpected_exit = linkg_thread_is_running(thread);

    if (unexpected_exit && result == 0)
    {
        result = -EIO;
    }

    pthread_mutex_lock(&g_network.lock);

    worker->run_result    = result;
    worker->run_completed = true;

    if (unexpected_exit &&
        (g_network.state == LINKG_NETWORK_STATE_STARTING ||
         g_network.state == LINKG_NETWORK_STATE_RUNNING))
    {
        g_network.state = LINKG_NETWORK_STATE_FAILED;
    }

    pthread_cond_broadcast(&g_network.worker_condition);

    pthread_mutex_unlock(&g_network.lock);

    if (unexpected_exit)
    {
        LINKG_LOG_ERROR("network worker runtime exited unexpectedly, thread=%s, error=%d", thread->name, result);
    }
    else if (result != 0)
    {
        LINKG_LOG_WARN("network worker runtime returned error during stop, thread=%s, error=%d", thread->name, result);
    }
}

/**
 * @brief 发布工作线程停止结果。
 */
static void _linkg_network_worker_publish_stop(linkg_network_worker_t *worker, int result)
{
    pthread_mutex_lock(&g_network.lock);
    worker->stop_result = result;
    pthread_mutex_unlock(&g_network.lock);
}

/**
 * @brief 运行网络子模块管理线程。
 *
 * start成功后，run存在时由run直接接管Owner循环；run为空时保留
 * 传统的等待停止模型。无论运行阶段如何结束，最终均由本线程调用stop。
 */
static void _linkg_network_worker_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_network_worker_t *worker;
    int                     run_result;
    int                     start_result;
    int                     stop_result;

    worker = user_data;

    if (thread == NULL ||
        worker == NULL ||
        worker->start == NULL ||
        worker->stop == NULL)
    {
        return;
    }

    start_result = linkg_thread_get_wakeup_fd(thread);
    if (start_result >= 0)
    {
        start_result = worker->start();
    }

    _linkg_network_worker_publish_start(worker, start_result);

    if (start_result != 0)
    {
        return;
    }

    if (worker->run != NULL)
    {
        run_result = worker->run(thread);
    }
    else
    {
        run_result = _linkg_network_worker_wait_stop(thread);
    }

    _linkg_network_worker_publish_run(worker, thread, run_result);

    stop_result = worker->stop();

    _linkg_network_worker_publish_stop(worker, stop_result);

    if (stop_result != 0)
    {
        LINKG_LOG_ERROR("network worker stop failed, thread=%s, error=%d", thread->name, stop_result);
    }
}

/**
 * @brief 初始化网络子模块管理线程。
 */
static int _linkg_network_worker_init(linkg_network_worker_t *worker, const char *name, linkg_network_worker_start_func_t start, linkg_network_worker_run_func_t run, linkg_network_worker_stop_func_t stop)
{
    int ret;

    if (worker == NULL || name == NULL || start == NULL || stop == NULL)
    {
        return -EINVAL;
    }

    memset(worker, 0, sizeof(*worker));

    worker->start = start;
    worker->run   = run;
    worker->stop  = stop;

    ret = linkg_thread_init(&worker->thread, name, _linkg_network_worker_thread, worker);
    if (ret != 0)
    {
        memset(worker, 0, sizeof(*worker));
        return ret;
    }

    worker->initialized = true;

    return 0;
}

/**
 * @brief 反初始化网络子模块管理线程。
 */
static void _linkg_network_worker_deinit(linkg_network_worker_t *worker)
{
    if (worker == NULL || !worker->initialized)
    {
        return;
    }

    linkg_thread_deinit(&worker->thread);
    memset(worker, 0, sizeof(*worker));
}

/**
 * @brief 启动网络子模块管理线程。
 */
static int _linkg_network_worker_start(linkg_network_worker_t *worker)
{
    int ret;

    if (worker == NULL || !worker->initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_network.lock);

    worker->start_result    = 0;
    worker->run_result      = 0;
    worker->stop_result     = 0;
    worker->start_completed = false;
    worker->run_completed   = false;

    pthread_mutex_unlock(&g_network.lock);

    ret = linkg_thread_start(&worker->thread);
    if (ret != 0)
    {
        return ret;
    }

    return 0;
}

/**
 * @brief 等待网络子模块启动完成。
 */
static int _linkg_network_worker_wait_start(linkg_network_worker_t *worker)
{
    int ret;

    if (worker == NULL || !worker->initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_network.lock);

    while (!worker->start_completed)
    {
        ret = pthread_cond_wait(&g_network.worker_condition, &g_network.lock);
        if (ret != 0)
        {
            pthread_mutex_unlock(&g_network.lock);
            return -ret;
        }
    }

    ret = worker->start_result;

    pthread_mutex_unlock(&g_network.lock);

    return ret;
}

/**
 * @brief 停止并回收网络子模块管理线程。
 */
static int _linkg_network_worker_stop(linkg_network_worker_t *worker)
{
    int stop_result;
    int ret;

    if (worker == NULL || !worker->initialized)
    {
        return -ENODEV;
    }

    if (!linkg_thread_is_started(&worker->thread))
    {
        return 0;
    }

    ret = linkg_thread_stop(&worker->thread);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_network.lock);
    stop_result = worker->stop_result;
    pthread_mutex_unlock(&g_network.lock);

    return stop_result;
}

/**
 * @brief 获取已经记录的首个工作线程运行期失败。
 *
 * 调用方必须持有g_network.lock。
 */
static int _linkg_network_get_worker_failure_locked(void)
{
    if (g_network.wifi_worker.run_completed &&
        g_network.wifi_worker.run_result != 0)
    {
        return g_network.wifi_worker.run_result;
    }

    if (g_network.cellular_worker.run_completed &&
        g_network.cellular_worker.run_result != 0)
    {
        return g_network.cellular_worker.run_result;
    }

    return -EIO;
}

/****************************** Ethernet ******************************/

/**
 * @brief 获取Ethernet接口对应的平台设备名称。
 *
 * /sys/class/net/<ifname>/device指向实际平台设备，
 * sunxi-gmac在/proc/interrupts中使用该设备名称注册gmacirq。
 */
static int _linkg_network_ethernet_get_device_name(const char *ifname, char *device_name, size_t device_name_size)
{
    char    device_path[LINKG_FILE_PATH_MAX];
    char    device_link[LINKG_FILE_PATH_MAX];
    char   *name;
    size_t  name_length;
    ssize_t length;
    int     written;

    if (ifname == NULL || device_name == NULL || device_name_size == 0U)
    {
        return -EINVAL;
    }

    written = snprintf(device_path, sizeof(device_path), "/sys/class/net/%s/device", ifname);
    if (written < 0 || (size_t)written >= sizeof(device_path))
    {
        return -ENAMETOOLONG;
    }

    length = readlink(device_path, device_link, sizeof(device_link) - 1U);
    if (length < 0)
    {
        return -errno;
    }

    if ((size_t)length >= sizeof(device_link) - 1U)
    {
        return -ENAMETOOLONG;
    }

    device_link[length] = '\0';

    name = strrchr(device_link, '/');
    if (name != NULL)
    {
        name++;
    }
    else
    {
        name = device_link;
    }

    if (name[0] == '\0')
    {
        return -ENODEV;
    }

    name_length = strlen(name);
    if (name_length >= device_name_size)
    {
        return -ENAMETOOLONG;
    }

    memcpy(device_name, name, name_length + 1U);

    return 0;
}

/**
 * @brief 从/proc/interrupts解析Ethernet平台设备对应的IRQ编号。
 */
static int _linkg_network_ethernet_get_irq(const char *ifname, unsigned int *irq)
{
    FILE         *stream;
    char          device_name[LINKG_NETWORK_ETHERNET_DEVICE_NAME_MAX];
    char          line[LINKG_NETWORK_ETHERNET_IRQ_LINE_MAX];
    char         *end;
    unsigned long value;
    int           close_ret;
    int           ret;

    if (ifname == NULL || irq == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_network_ethernet_get_device_name(ifname, device_name, sizeof(device_name));
    if (ret != 0)
    {
        return ret;
    }

    stream = NULL;

    ret = linkg_file_stream_open("/proc/interrupts", "r", &stream);
    if (ret != 0)
    {
        return ret;
    }

    ret = -ENODEV;

    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (strstr(line, device_name) == NULL)
        {
            continue;
        }

        errno = 0;
        value = strtoul(line, &end, 10);
        if (errno != 0 || end == line || *end != ':')
        {
            continue;
        }

        if (value > UINT_MAX)
        {
            ret = -ERANGE;
            break;
        }

        *irq = (unsigned int)value;
        ret  = 0;
        break;
    }

    if (ret == -ENODEV && ferror(stream))
    {
        ret = errno != 0 ? -errno : -EIO;
    }

    close_ret = linkg_file_stream_close(&stream);
    if (ret == 0 && close_ret != 0)
    {
        ret = close_ret;
    }

    return ret;
}

/**
 * @brief 将指定IRQ绑定到固定CPU。
 *
 * 优先写smp_affinity_list；旧内核没有该节点时，
 * 回退到smp_affinity位掩码。
 */
static int _linkg_network_ethernet_set_irq_cpu(unsigned int irq, unsigned int cpu_id)
{
    char affinity_path[LINKG_FILE_PATH_MAX];
    char affinity_value[LINKG_NETWORK_ETHERNET_IRQ_AFFINITY_VALUE_MAX];
    int  value_length;
    int  written;
    int  ret;

    written = snprintf(affinity_path, sizeof(affinity_path), "/proc/irq/%u/smp_affinity_list", irq);
    if (written < 0 || (size_t)written >= sizeof(affinity_path))
    {
        return -ENAMETOOLONG;
    }

    value_length = snprintf(affinity_value, sizeof(affinity_value), "%u\n", cpu_id);
    if (value_length < 0 || (size_t)value_length >= sizeof(affinity_value))
    {
        return -EOVERFLOW;
    }

    ret = linkg_file_write_all(affinity_path, affinity_value, (size_t)value_length);
    if (ret == 0)
    {
        return 0;
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    if (cpu_id >= 32U)
    {
        return -ERANGE;
    }

    written = snprintf(affinity_path, sizeof(affinity_path), "/proc/irq/%u/smp_affinity", irq);
    if (written < 0 || (size_t)written >= sizeof(affinity_path))
    {
        return -ENAMETOOLONG;
    }

    value_length = snprintf(affinity_value, sizeof(affinity_value), "%x\n", 1U << cpu_id);
    if (value_length < 0 || (size_t)value_length >= sizeof(affinity_value))
    {
        return -EOVERFLOW;
    }

    return linkg_file_write_all(affinity_path, affinity_value, (size_t)value_length);
}

/**
 * @brief 将Ethernet接收中断固定到CPU1。
 *
 * T113当前sunxi-gmac驱动的RX/TX共用同一个gmacirq，
 * 因此绑定该GMAC IRQ即可保证Ethernet接收中断固定在CPU1。
 */
static int _linkg_network_ethernet_bind_rx_irq(void)
{
    unsigned int irq;
    int          ret;

    ret = _linkg_network_ethernet_get_irq(LINKG_RESOURCE_INTERFACE_ETHERNET, &irq);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_network_ethernet_set_irq_cpu(irq, LINKG_NETWORK_ETHERNET_RX_IRQ_CPU_ID);
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("Ethernet RX IRQ bound, interface=%s, irq=%u, cpu=%u", LINKG_RESOURCE_INTERFACE_ETHERNET, irq, LINKG_NETWORK_ETHERNET_RX_IRQ_CPU_ID);

    return 0;
}

/**
 * @brief 启动Ethernet接口。
 */
static int _linkg_network_ethernet_start(void)
{
    linkg_network_ipv4_config_t ethernet;
    int                         ret;

    if (g_network.ethernet_started)
    {
        return 0;
    }

    ret = linkg_network_interface_wait(LINKG_RESOURCE_INTERFACE_ETHERNET, LINKG_NETWORK_ETHERNET_WAIT_TIMEOUT_MS);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_config_get_ethernet(&g_network.network_config, &ethernet);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_interface_set_ipv4(LINKG_RESOURCE_INTERFACE_ETHERNET, &ethernet.ip, &ethernet.netmask);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_ETHERNET, true);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_network_ethernet_bind_rx_irq();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("bind Ethernet RX IRQ failed, interface=%s, cpu=%u, error=%d", LINKG_RESOURCE_INTERFACE_ETHERNET, LINKG_NETWORK_ETHERNET_RX_IRQ_CPU_ID, ret);

        return ret;
    }

    g_network.ethernet_started = true;

    return 0;
}

/**
 * @brief 停止网络服务对Ethernet接口的管理状态。
 *
 * 保持现有行为：停止网络服务时不主动关闭系统Ethernet接口。
 */
static int _linkg_network_ethernet_stop(void)
{
    if (!g_network.ethernet_started)
    {
        return 0;
    }

    g_network.ethernet_started = false;

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
 * @brief 初始化网络服务及已启用的接入模块。
 */
int linkg_network_init(void)
{
    linkg_device_config_t device_config;
    int                   cleanup_ret;
    int                   ret;

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

    ret = pthread_cond_init(&g_network.worker_condition, NULL);
    if (ret != 0)
    {
        pthread_mutex_destroy(&g_network.lock);
        _linkg_network_reset_context();
        return -ret;
    }

    ret = linkg_config_get_device(&device_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("get device configuration failed, error=%d", ret);
        goto fail;
    }

    if (device_config.role != LINKG_DEVICE_ROLE_AP &&
        device_config.role != LINKG_DEVICE_ROLE_STA)
    {
        ret = -EINVAL;
        LINKG_LOG_ERROR("invalid device role, role=%d", (int)device_config.role);
        goto fail;
    }

    ret = linkg_config_get_network(&g_network.network_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("get network configuration failed, error=%d", ret);
        goto fail;
    }

    ret = linkg_config_get_wifi(&g_network.wifi_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("get Wi-Fi configuration failed, error=%d", ret);
        goto fail;
    }

    ret = linkg_config_get_cellular(&g_network.cellular_config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("get cellular configuration failed, error=%d", ret);
        goto fail;
    }

    g_network.role = device_config.role;

    if (g_network.wifi_config.enabled)
    {
        ret = linkg_wifi_init(g_network.role, g_network.network_config.node_id, &g_network.wifi_config);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("initialize Wi-Fi module failed, role=%d, node_id=%u, error=%d", (int)g_network.role, (unsigned int)g_network.network_config.node_id, ret);
            goto fail;
        }

        g_network.wifi_initialized = true;

        ret = _linkg_network_worker_init(&g_network.wifi_worker, LINKG_NETWORK_WIFI_THREAD_NAME, linkg_wifi_start, linkg_wifi_run, linkg_wifi_stop);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("initialize Wi-Fi Owner worker failed, error=%d", ret);
            goto fail;
        }
    }

    if (g_network.cellular_config.enabled)
    {
        ret = linkg_cellular_init(&g_network.cellular_config);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("initialize cellular module failed, error=%d", ret);
            goto fail;
        }

        g_network.cellular_initialized = true;

        ret = _linkg_network_worker_init(&g_network.cellular_worker, LINKG_NETWORK_CELLULAR_THREAD_NAME, linkg_cellular_start, linkg_cellular_run, linkg_cellular_stop);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("initialize cellular worker failed, error=%d", ret);
            goto fail;
        }
    }

    pthread_mutex_lock(&g_network.lock);
    g_network.state = LINKG_NETWORK_STATE_STOPPED;
    pthread_mutex_unlock(&g_network.lock);

    LINKG_LOG_INFO("network service initialized, role=%d, node_id=%u, wifi_enabled=%d, cellular_enabled=%d", (int)g_network.role, (unsigned int)g_network.network_config.node_id, g_network.wifi_config.enabled, g_network.cellular_config.enabled);

    return 0;

fail:
    _linkg_network_worker_deinit(&g_network.cellular_worker);

    if (g_network.cellular_initialized)
    {
        cleanup_ret = linkg_cellular_deinit();
        if (cleanup_ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize cellular module after init failure failed, error=%d", cleanup_ret);
        }

        g_network.cellular_initialized = false;
    }

    _linkg_network_worker_deinit(&g_network.wifi_worker);

    if (g_network.wifi_initialized)
    {
        cleanup_ret = linkg_wifi_deinit();
        if (cleanup_ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize Wi-Fi module after init failure failed, error=%d", cleanup_ret);
        }

        g_network.wifi_initialized = false;
    }

    cleanup_ret = pthread_cond_destroy(&g_network.worker_condition);
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("destroy network worker condition after init failure failed, error=%d", cleanup_ret);
    }

    cleanup_ret = pthread_mutex_destroy(&g_network.lock);
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("destroy network service lock after init failure failed, error=%d", cleanup_ret);
    }

    _linkg_network_reset_context();

    return ret;
}

/**
 * @brief 启动网络服务及所有已启用的网络接入模块。
 */
int linkg_network_start(void)
{
    linkg_network_state_t state;
    int                   cleanup_error;
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

    if (g_network.wifi_config.enabled)
    {
        if (!g_network.wifi_initialized)
        {
            ret = -ENODEV;
            goto fail;
        }

        ret = _linkg_network_worker_start(&g_network.wifi_worker);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("start Wi-Fi Owner worker failed, error=%d", ret);
            goto fail;
        }
    }

    if (g_network.cellular_config.enabled)
    {
        if (!g_network.cellular_initialized)
        {
            ret = -ENODEV;
            goto fail;
        }

        ret = _linkg_network_worker_start(&g_network.cellular_worker);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("start cellular worker failed, error=%d", ret);
            goto fail;
        }
    }

    // Wi-Fi与蜂窝模块后台启动期间并行配置Ethernet。
    ret = _linkg_network_ethernet_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start Ethernet failed, interface=%s, error=%d", LINKG_RESOURCE_INTERFACE_ETHERNET, ret);
        goto fail;
    }

    ret = _linkg_network_node_address_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start local node address failed, error=%d", ret);
        goto fail;
    }

    if (g_network.wifi_config.enabled)
    {
        ret = _linkg_network_worker_wait_start(&g_network.wifi_worker);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("start Wi-Fi module failed, error=%d", ret);
            goto fail;
        }
    }

    if (g_network.cellular_config.enabled)
    {
        ret = _linkg_network_worker_wait_start(&g_network.cellular_worker);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("start cellular lifecycle failed, error=%d", ret);
            goto fail;
        }
    }

    ret = _linkg_network_ipv4_forwarding_start();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("enable IPv4 forwarding failed, error=%d", ret);
        goto fail;
    }

    pthread_mutex_lock(&g_network.lock);

    if (g_network.state != LINKG_NETWORK_STATE_STARTING)
    {
        ret = _linkg_network_get_worker_failure_locked();
        pthread_mutex_unlock(&g_network.lock);
        goto fail;
    }

    g_network.state = LINKG_NETWORK_STATE_RUNNING;

    pthread_mutex_unlock(&g_network.lock);

    LINKG_LOG_INFO("network service started");

    return 0;

fail:
    cleanup_error = 0;

    cleanup_ret = _linkg_network_ipv4_forwarding_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback IPv4 forwarding failed, error=%d", cleanup_ret);
        _linkg_network_record_first_error(&cleanup_error, cleanup_ret);
    }

    if (g_network.cellular_config.enabled &&
        g_network.cellular_worker.initialized)
    {
        cleanup_ret = _linkg_network_worker_stop(&g_network.cellular_worker);
        if (cleanup_ret != 0)
        {
            LINKG_LOG_ERROR("rollback cellular worker failed, error=%d", cleanup_ret);
            _linkg_network_record_first_error(&cleanup_error, cleanup_ret);
        }
    }

    cleanup_ret = _linkg_network_node_address_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback local node address failed, error=%d", cleanup_ret);
        _linkg_network_record_first_error(&cleanup_error, cleanup_ret);
    }

    cleanup_ret = _linkg_network_ethernet_stop();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback Ethernet failed, interface=%s, error=%d", LINKG_RESOURCE_INTERFACE_ETHERNET, cleanup_ret);

        _linkg_network_record_first_error(&cleanup_error, cleanup_ret);
    }

    if (g_network.wifi_config.enabled &&
        g_network.wifi_worker.initialized)
    {
        cleanup_ret = _linkg_network_worker_stop(&g_network.wifi_worker);
        if (cleanup_ret != 0)
        {
            LINKG_LOG_ERROR("rollback Wi-Fi Owner worker failed, error=%d", cleanup_ret);
            _linkg_network_record_first_error(&cleanup_error, cleanup_ret);
        }
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.state = cleanup_error == 0 ? LINKG_NETWORK_STATE_STOPPED : LINKG_NETWORK_STATE_FAILED;

    pthread_mutex_unlock(&g_network.lock);

    LINKG_LOG_ERROR("start network service failed, error=%d", ret);

    return ret;
}

/**
 * @brief 停止网络服务并回收全部运行资源。
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

    if (state == LINKG_NETWORK_STATE_STARTING ||
        state == LINKG_NETWORK_STATE_STOPPING)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EBUSY;
    }

    if (state != LINKG_NETWORK_STATE_RUNNING &&
        state != LINKG_NETWORK_STATE_FAILED)
    {
        pthread_mutex_unlock(&g_network.lock);
        return -EINVAL;
    }

    g_network.state = LINKG_NETWORK_STATE_STOPPING;

    pthread_mutex_unlock(&g_network.lock);

    first_error = 0;

    ret = _linkg_network_ipv4_forwarding_stop();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("disable IPv4 forwarding failed, error=%d", ret);
        _linkg_network_record_first_error(&first_error, ret);
    }

    if (g_network.cellular_config.enabled &&
        g_network.cellular_worker.initialized)
    {
        ret = _linkg_network_worker_stop(&g_network.cellular_worker);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop cellular worker failed, error=%d", ret);
            _linkg_network_record_first_error(&first_error, ret);
        }
    }
    ret = _linkg_network_node_address_stop();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("stop local node address failed, error=%d", ret);
        _linkg_network_record_first_error(&first_error, ret);
    }

    ret = _linkg_network_ethernet_stop();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("stop Ethernet failed, interface=%s, error=%d", LINKG_RESOURCE_INTERFACE_ETHERNET, ret);

        _linkg_network_record_first_error(&first_error, ret);
    }

    if (g_network.wifi_config.enabled &&
        g_network.wifi_worker.initialized)
    {
        ret = _linkg_network_worker_stop(&g_network.wifi_worker);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop Wi-Fi Owner worker failed, error=%d", ret);
            _linkg_network_record_first_error(&first_error, ret);
        }
    }

    pthread_mutex_lock(&g_network.lock);

    g_network.state = first_error == 0 ? LINKG_NETWORK_STATE_STOPPED : LINKG_NETWORK_STATE_FAILED;

    pthread_mutex_unlock(&g_network.lock);

    if (first_error != 0)
    {
        return first_error;
    }

    LINKG_LOG_INFO("network service stopped");

    return 0;
}

/**
 * @brief 反初始化网络服务及全部已初始化子模块。
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

    if (state != LINKG_NETWORK_STATE_STOPPED &&
        state != LINKG_NETWORK_STATE_FAILED)
    {
        return -EBUSY;
    }

    if (g_network.ethernet_started ||
    g_network.node_address_started ||
    g_network.ipv4_forwarding_enabled ||
    (g_network.wifi_worker.initialized &&
     linkg_thread_is_started(&g_network.wifi_worker.thread)) ||
    (g_network.cellular_worker.initialized &&
     linkg_thread_is_started(&g_network.cellular_worker.thread)))
    {
        return -EBUSY;
    }

    _linkg_network_worker_deinit(&g_network.cellular_worker);

    if (g_network.cellular_initialized)
    {
        ret = linkg_cellular_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize cellular module failed, error=%d", ret);
            return ret;
        }

        g_network.cellular_initialized = false;
    }

    _linkg_network_worker_deinit(&g_network.wifi_worker);

    if (g_network.wifi_initialized)
    {
        ret = linkg_wifi_deinit();
        if (ret != 0)
        {
            LINKG_LOG_ERROR("deinitialize Wi-Fi module failed, error=%d", ret);
            return ret;
        }

        g_network.wifi_initialized = false;
    }

    ret = pthread_cond_destroy(&g_network.worker_condition);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("destroy network worker condition failed, error=%d", ret);
        return -ret;
    }

    ret = pthread_mutex_destroy(&g_network.lock);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("destroy network service lock failed, error=%d", ret);
        return -ret;
    }

    _linkg_network_reset_context();

    LINKG_LOG_INFO("network service deinitialized");

    return 0;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取网络服务当前生命周期状态。
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
