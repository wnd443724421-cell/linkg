/**
 * @file linkg_ethernet.c
 * @brief LinkG Ethernet模块实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-16
 */

#include "linkg_ethernet.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "linkg_file.h"
#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_ETHERNET_INTERFACE_WAIT_TIMEOUT_MS 3000U        // Ethernet接口等待超时
#define LINKG_ETHERNET_MONITOR_INTERVAL_MS       500          // 链路状态检查周期
#define LINKG_ETHERNET_ANNOUNCE_COUNT            2U           // 每次地址变化的ARP宣告次数
#define LINKG_ETHERNET_ANNOUNCE_INTERVAL_US      2000000ULL   // 两次ARP宣告间隔
#define LINKG_ETHERNET_RX_IRQ_CPU_ID             0U           // Ethernet接收IRQ固定CPU编号
#define LINKG_ETHERNET_IRQ_LINE_MAX              512U         // /proc/interrupts单行缓冲区长度
#define LINKG_ETHERNET_DEVICE_NAME_MAX           128U         // Ethernet平台设备名称缓冲区长度
#define LINKG_ETHERNET_IRQ_AFFINITY_VALUE_MAX    32U          // IRQ affinity写入值缓冲区长度

/****************************** 类型定义 ******************************/

typedef enum
{
    LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED = 0, // 未初始化
    LINKG_ETHERNET_LIFECYCLE_STOPPED,           // 已初始化且已停止
    LINKG_ETHERNET_LIFECYCLE_STARTING,          // 正在启动
    LINKG_ETHERNET_LIFECYCLE_RUNNING,           // 正在运行
    LINKG_ETHERNET_LIFECYCLE_STOPPING           // 正在停止
} linkg_ethernet_lifecycle_t;

typedef struct
{
    pthread_mutex_t             lock;      // Ethernet模块状态锁
    linkg_network_ipv4_config_t config;    // Ethernet IPv4配置快照
    linkg_ethernet_lifecycle_t  lifecycle; // Ethernet模块生命周期状态
} linkg_ethernet_context_t;

/****************************** 全局上下文 ******************************/

static linkg_ethernet_context_t g_ethernet =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,            // 初始化静态状态锁
    .lifecycle = LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED // 初始生命周期状态
};

/****************************** 上下文辅助 ******************************/

/**
 * @brief 获取Ethernet模块生命周期状态。
 */
static linkg_ethernet_lifecycle_t _linkg_ethernet_get_lifecycle(void)
{
    linkg_ethernet_lifecycle_t lifecycle;

    pthread_mutex_lock(&g_ethernet.lock);
    lifecycle = g_ethernet.lifecycle;
    pthread_mutex_unlock(&g_ethernet.lock);

    return lifecycle;
}

/**
 * @brief 设置Ethernet模块生命周期状态。
 */
static void _linkg_ethernet_set_lifecycle(linkg_ethernet_lifecycle_t lifecycle)
{
    pthread_mutex_lock(&g_ethernet.lock);
    g_ethernet.lifecycle = lifecycle;
    pthread_mutex_unlock(&g_ethernet.lock);
}

/****************************** IRQ绑定 ******************************/

/**
 * @brief 获取Ethernet接口对应的平台设备名称。
 */
static int _linkg_ethernet_get_device_name(const char *ifname, char *device_name, size_t device_name_size)
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
    name                = strrchr(device_link, '/');
    name                = name == NULL ? device_link : name + 1;

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
 * @brief 获取Ethernet平台设备对应的IRQ编号。
 */
static int _linkg_ethernet_get_irq(const char *ifname, unsigned int *irq)
{
    FILE         *stream;
    char          device_name[LINKG_ETHERNET_DEVICE_NAME_MAX];
    char          line[LINKG_ETHERNET_IRQ_LINE_MAX];
    char         *end;
    unsigned long value;
    int           close_ret;
    int           ret;

    if (ifname == NULL || irq == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_ethernet_get_device_name(ifname, device_name, sizeof(device_name));
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
 */
static int _linkg_ethernet_set_irq_cpu(unsigned int irq, unsigned int cpu_id)
{
    char affinity_path[LINKG_FILE_PATH_MAX];
    char affinity_value[LINKG_ETHERNET_IRQ_AFFINITY_VALUE_MAX];
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
 * @brief 将Ethernet接收中断固定到指定CPU。
 */
static int _linkg_ethernet_bind_rx_irq(void)
{
    unsigned int irq;
    int          ret;

    ret = _linkg_ethernet_get_irq(LINKG_RESOURCE_INTERFACE_ETHERNET, &irq);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_ethernet_set_irq_cpu(irq, LINKG_ETHERNET_RX_IRQ_CPU_ID);
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("Ethernet RX IRQ bound, interface=%s, irq=%u, cpu=%u",
                   LINKG_RESOURCE_INTERFACE_ETHERNET,
                   irq,
                   LINKG_ETHERNET_RX_IRQ_CPU_ID);

    return 0;
}

/****************************** 链路监控 ******************************/

/**
 * @brief 读取Ethernet接口当前就绪状态和地址。
 */
static int _linkg_ethernet_read_state(bool *ready, struct in_addr *address, uint8_t mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH])
{
    bool carrier_up;
    bool interface_up;
    int  ret;

    if (ready == NULL || address == NULL || mac == NULL)
    {
        return -EINVAL;
    }

    *ready = false;

    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_ETHERNET, &interface_up);
    if (ret != 0 || !interface_up)
    {
        return ret;
    }

    ret = linkg_network_interface_carrier_is_up(LINKG_RESOURCE_INTERFACE_ETHERNET, &carrier_up);
    if (ret != 0 || !carrier_up)
    {
        return ret;
    }

    ret = linkg_network_interface_get_ipv4(LINKG_RESOURCE_INTERFACE_ETHERNET, address);
    if (ret != 0)
    {
        return ret;
    }

    if (!linkg_network_ipv4_address_valid(address))
    {
        return -EADDRNOTAVAIL;
    }

    ret = linkg_network_interface_get_mac(LINKG_RESOURCE_INTERFACE_ETHERNET, mac);
    if (ret != 0)
    {
        return ret;
    }

    *ready = true;

    return 0;
}

/**
 * @brief 等待Ethernet监控线程的下一次检查或停止通知。
 */
static int _linkg_ethernet_wait_monitor(linkg_thread_t *owner_thread, struct pollfd *descriptor)
{
    int ret;

    do
    {
        ret = poll(descriptor, 1, LINKG_ETHERNET_MONITOR_INTERVAL_MS);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(owner_thread));

    if (ret < 0)
    {
        return -errno;
    }

    if ((descriptor->revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
    {
        return -EIO;
    }

    if ((descriptor->revents & POLLIN) != 0)
    {
        ret = linkg_thread_clear_wakeup(owner_thread);
        if (ret != 0)
        {
            return ret;
        }
    }

    descriptor->revents = 0;

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Ethernet模块。
 */
int linkg_ethernet_init(const linkg_network_ipv4_config_t *config)
{
    if (config == NULL ||
        !linkg_network_ipv4_address_valid(&config->ip) ||
        !linkg_network_ipv4_netmask_valid(&config->netmask))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_ethernet.lock);

    if (g_ethernet.lifecycle != LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_ethernet.lock);
        return -EALREADY;
    }

    g_ethernet.config    = *config;
    g_ethernet.lifecycle = LINKG_ETHERNET_LIFECYCLE_STOPPED;

    pthread_mutex_unlock(&g_ethernet.lock);

    LINKG_LOG_INFO("Ethernet module initialized");

    return 0;
}

/**
 * @brief 启动Ethernet模块。
 */
int linkg_ethernet_start(void)
{
    linkg_network_ipv4_config_t config;
    const char                 *phase;
    int                         ret;

    pthread_mutex_lock(&g_ethernet.lock);

    if (g_ethernet.lifecycle == LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_ethernet.lock);
        return -ENODEV;
    }

    if (g_ethernet.lifecycle != LINKG_ETHERNET_LIFECYCLE_STOPPED)
    {
        pthread_mutex_unlock(&g_ethernet.lock);
        return -EALREADY;
    }

    config               = g_ethernet.config;
    g_ethernet.lifecycle = LINKG_ETHERNET_LIFECYCLE_STARTING;

    pthread_mutex_unlock(&g_ethernet.lock);

    phase = "wait_interface";
    ret = linkg_network_interface_wait(LINKG_RESOURCE_INTERFACE_ETHERNET, LINKG_ETHERNET_INTERFACE_WAIT_TIMEOUT_MS);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "set_ipv4";
    ret = linkg_network_interface_set_ipv4(LINKG_RESOURCE_INTERFACE_ETHERNET, &config.ip, &config.netmask);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "interface_up";
    ret = linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_ETHERNET, true);
    if (ret != 0)
    {
        goto fail;
    }

    phase = "bind_rx_irq";
    ret = _linkg_ethernet_bind_rx_irq();
    if (ret != 0)
    {
        goto fail;
    }

    _linkg_ethernet_set_lifecycle(LINKG_ETHERNET_LIFECYCLE_RUNNING);

    LINKG_LOG_INFO("Ethernet module started, interface=%s", LINKG_RESOURCE_INTERFACE_ETHERNET);

    return 0;

fail:
    _linkg_ethernet_set_lifecycle(LINKG_ETHERNET_LIFECYCLE_STOPPED);

    LINKG_LOG_ERROR("Ethernet module start failed, phase=%s, interface=%s, error=%d",
                    phase,
                    LINKG_RESOURCE_INTERFACE_ETHERNET,
                    ret);

    return ret;
}

/**
 * @brief 运行Ethernet链路和地址监控。
 */
int linkg_ethernet_run(linkg_thread_t *owner_thread)
{
    struct pollfd descriptor;
    struct in_addr last_address = {0};
    uint8_t last_mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH] = {0};
    uint64_t next_announcement_us;
    unsigned int announcements_left;
    bool previously_ready;
    int ret;

    if (owner_thread == NULL)
    {
        return -EINVAL;
    }

    if (_linkg_ethernet_get_lifecycle() != LINKG_ETHERNET_LIFECYCLE_RUNNING)
    {
        return -EBUSY;
    }

    memset(&descriptor, 0, sizeof(descriptor));
    descriptor.fd     = linkg_thread_get_wakeup_fd(owner_thread);
    descriptor.events = POLLIN;
    if (descriptor.fd < 0)
    {
        return descriptor.fd;
    }

    next_announcement_us = 0U;
    announcements_left   = 0U;
    previously_ready     = false;

    while (linkg_thread_is_running(owner_thread))
    {
        struct in_addr address = {0};
        uint8_t        mac[LINKG_NETWORK_MAC_ADDRESS_LENGTH] = {0};
        uint64_t       now_us;
        bool           ready;

        ret = _linkg_ethernet_read_state(&ready, &address, mac);
        ready = ret == 0 && ready;

        if (!ready)
        {
            previously_ready   = false;
            announcements_left = 0U;
        }
        else
        {
            now_us = linkg_time_monotonic_us();

            if (!previously_ready ||
                address.s_addr != last_address.s_addr ||
                memcmp(mac, last_mac, sizeof(mac)) != 0)
            {
                last_address        = address;
                previously_ready    = true;
                announcements_left  = LINKG_ETHERNET_ANNOUNCE_COUNT;
                next_announcement_us = now_us;
                memcpy(last_mac, mac, sizeof(mac));
            }

            if (announcements_left > 0U && now_us >= next_announcement_us)
            {
                char text[INET_ADDRSTRLEN] = {0};

                ret = linkg_network_interface_announce_ipv4(LINKG_RESOURCE_INTERFACE_ETHERNET);
                (void)linkg_network_ipv4_to_string(&address, text, sizeof(text));

                if (ret == 0)
                {
                    LINKG_LOG_INFO("Ethernet ARP announcement, interface=%s, ip=%s, remaining=%u",
                                   LINKG_RESOURCE_INTERFACE_ETHERNET,
                                   text,
                                   announcements_left - 1U);
                }
                else
                {
                    LINKG_LOG_WARN("Ethernet ARP announcement failed, interface=%s, error=%d",
                                   LINKG_RESOURCE_INTERFACE_ETHERNET,
                                   ret);
                }

                announcements_left--;
                next_announcement_us = now_us + LINKG_ETHERNET_ANNOUNCE_INTERVAL_US;
            }
        }

        ret = _linkg_ethernet_wait_monitor(owner_thread, &descriptor);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 停止Ethernet模块。
 */
int linkg_ethernet_stop(void)
{
    linkg_ethernet_lifecycle_t lifecycle;

    lifecycle = _linkg_ethernet_get_lifecycle();
    if (lifecycle == LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED ||
        lifecycle == LINKG_ETHERNET_LIFECYCLE_STOPPED)
    {
        return 0;
    }

    if (lifecycle != LINKG_ETHERNET_LIFECYCLE_RUNNING)
    {
        return -EBUSY;
    }

    _linkg_ethernet_set_lifecycle(LINKG_ETHERNET_LIFECYCLE_STOPPING);

    // 保持当前行为，停止模块时不主动关闭系统Ethernet接口。
    _linkg_ethernet_set_lifecycle(LINKG_ETHERNET_LIFECYCLE_STOPPED);

    LINKG_LOG_INFO("Ethernet module stopped");

    return 0;
}

/**
 * @brief 反初始化Ethernet模块。
 */
int linkg_ethernet_deinit(void)
{
    int ret;

    ret = linkg_ethernet_stop();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_ethernet.lock);

    if (g_ethernet.lifecycle == LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_ethernet.lock);
        return 0;
    }

    memset(&g_ethernet.config, 0, sizeof(g_ethernet.config));
    g_ethernet.lifecycle = LINKG_ETHERNET_LIFECYCLE_UNINITIALIZED;

    pthread_mutex_unlock(&g_ethernet.lock);

    LINKG_LOG_INFO("Ethernet module deinitialized");

    return 0;
}
