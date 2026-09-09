/**
 * @file linkg_nat.c
 * @brief LinkG Fast NAT用户态控制实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-09-01
 */

#include "linkg_nat.h"

#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "linkg_fast_nat_uapi.h"
#include "linkg_network_ops.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_NAT_SNAT_PORT_START                  40000U                                                             // Fast SNAT转换端口池起始端口
#define LINKG_NAT_DRIVER_MODULE_NAME               "linkg_fast_nat"                                                   // Fast NAT内核模块名称
#define LINKG_NAT_DRIVER_MODULE_DIRECTORY          "/app/current/drivers"                                             // Fast NAT内核模块目录
#define LINKG_NAT_DRIVER_MODULE_FILE               "linkg_fast_nat.ko"                                                // Fast NAT内核模块文件名
#define LINKG_NAT_DRIVER_MODULE_PATH               LINKG_NAT_DRIVER_MODULE_DIRECTORY "/" LINKG_NAT_DRIVER_MODULE_FILE // Fast NAT内核模块完整路径
#define LINKG_NAT_DRIVER_MODULE_LINE_MAX           256U                                                               // /proc/modules单行缓冲区长度
#define LINKG_NAT_DRIVER_OPEN_RETRY_COUNT          100U                                                               // 字符设备节点等待重试次数
#define LINKG_NAT_DRIVER_OPEN_RETRY_US             10000U                                                             // 字符设备节点等待间隔

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_network_ipv4_config_t virtual_network;      // LinkG虚拟聚合网络
    linkg_network_ipv4_config_t local_virtual_subnet; // 本节点虚拟Endpoint子网
    linkg_network_ipv4_config_t tun_network;          // LinkG TUN节点网络
    linkg_network_ipv4_config_t ethernet_network;     // 本节点Ethernet网络
    linkg_network_ipv4_config_t ethernet;             // 本节点Ethernet接口IPv4配置
    int                         driver_fd;            // Fast NAT字符设备句柄
    bool                        uplink_enabled;       // 外部蜂窝上行是否启用
    bool                        driver_loaded;        // Fast NAT内核模块是否由当前NAT生命周期持有
    bool                        initialized;          // 模块是否已经初始化
    bool                        started;              // Fast NAT是否已经启动
} linkg_nat_context_t;

/****************************** 全局上下文 ******************************/

static linkg_nat_context_t g_nat =
{
    .driver_fd      = -1,    // Fast NAT字符设备尚未打开
    .uplink_enabled = false, // 外部蜂窝上行默认未启用
    .driver_loaded  = false, // Fast NAT内核模块尚未加载
    .initialized    = false, // NAT控制模块尚未初始化
    .started        = false  // Fast NAT尚未启动
};

/****************************** 配置辅助 ******************************/

/**
 * @brief 根据LinkG网络配置构造NAT用户态上下文。
 */
static int _linkg_nat_build_context(const linkg_network_config_t *network_config, linkg_nat_context_t *context)
{
    int ret;

    if (network_config == NULL || context == NULL)
    {
        return -EINVAL;
    }

    if (network_config->node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        network_config->node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    memset(context, 0, sizeof(*context));

    context->driver_fd = -1;

    ret = linkg_network_config_get_virtual_network(network_config, &context->virtual_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_node_virtual_subnet(network_config, network_config->node_id, &context->local_virtual_subnet);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_tun(network_config, &context->tun_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    // 将本节点TUN地址规范化为TUN网络地址。
    context->tun_network.ip.s_addr &= context->tun_network.netmask.s_addr;

    ret = linkg_network_config_get_ethernet_network(network_config, &context->ethernet_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_ethernet(network_config, &context->ethernet);
    if (ret != 0)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 将LinkG IPv4配置转换为Fast NAT子网配置。
 */
static void _linkg_nat_build_subnet(const linkg_network_ipv4_config_t *source, linkg_fast_nat_ipv4_subnet_t *destination)
{
    destination->network = source->ip.s_addr & source->netmask.s_addr;
    destination->netmask = source->netmask.s_addr;
}

/**
 * @brief 获取指定网络接口的ifindex。
 */
static int _linkg_nat_get_ifindex(const char *ifname, int32_t *ifindex)
{
    unsigned int index;

    if (ifname == NULL || ifindex == NULL)
    {
        return -EINVAL;
    }

    errno = 0;

    index = if_nametoindex(ifname);
    if (index == 0U)
    {
        return errno != 0 ? -errno : -ENODEV;
    }

    if (index > INT32_MAX)
    {
        return -ERANGE;
    }

    *ifindex = (int32_t)index;

    return 0;
}

/**
 * @brief 构造下发给Fast NAT驱动的运行配置。
 */
static int _linkg_nat_build_driver_config(linkg_fast_nat_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    memset(config, 0, sizeof(*config));

    config->version     = LINKG_FAST_NAT_UAPI_VERSION;
    config->struct_size = sizeof(*config);

    _linkg_nat_build_subnet(&g_nat.ethernet_network, &config->ethernet_network);
    _linkg_nat_build_subnet(&g_nat.tun_network, &config->tun_network);
    _linkg_nat_build_subnet(&g_nat.virtual_network, &config->virtual_network);
    _linkg_nat_build_subnet(&g_nat.local_virtual_subnet, &config->local_virtual_subnet);

    config->ethernet_ip = g_nat.ethernet.ip.s_addr;

    ret = _linkg_nat_get_ifindex(LINKG_RESOURCE_INTERFACE_ETHERNET, &config->ethernet_ifindex);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_get_ifindex(LINKG_RESOURCE_INTERFACE_TUN, &config->tun_ifindex);
    if (ret != 0)
    {
        return ret;
    }

    if (g_nat.uplink_enabled)
    {
        ret = _linkg_nat_get_ifindex(LINKG_RESOURCE_INTERFACE_CELLULAR, &config->uplink_ifindex);
        if (ret != 0)
        {
            return ret;
        }
    }

    config->snat_port_start = LINKG_NAT_SNAT_PORT_START;

    return 0;
}

/****************************** 驱动模块 ******************************/

/**
 * @brief 检查Fast NAT内核模块是否已经加载。
 */
static bool _linkg_nat_driver_module_loaded(void)
{
    FILE  *stream;
    char   line[LINKG_NAT_DRIVER_MODULE_LINE_MAX];
    size_t name_length;
    bool   loaded;

    stream      = NULL;
    name_length = strlen(LINKG_NAT_DRIVER_MODULE_NAME);
    loaded      = false;

    stream = fopen("/proc/modules", "r");
    if (stream == NULL)
    {
        return false;
    }

    while (fgets(line, sizeof(line), stream) != NULL)
    {
        if (strncmp(line, LINKG_NAT_DRIVER_MODULE_NAME, name_length) != 0)
        {
            continue;
        }

        if (line[name_length] != ' ')
        {
            continue;
        }

        loaded = true;
        break;
    }

    (void)fclose(stream);

    return loaded;
}

/**
 * @brief 加载Fast NAT内核模块。
 *
 * 模块已经加载时直接接管该模块生命周期；
 * insmod失败后再次确认实际状态，兼容重复加载场景。
 */
static int _linkg_nat_driver_module_load(void)
{
    int saved_errno;
    int ret;

    if (_linkg_nat_driver_module_loaded())
    {
        g_nat.driver_loaded = true;
        return 0;
    }

    if (access(LINKG_NAT_DRIVER_MODULE_PATH, R_OK) != 0)
    {
        saved_errno = errno;
        return -saved_errno;
    }

    ret = linkg_os_run("insmod", LINKG_NAT_DRIVER_MODULE_PATH, NULL);
    if (ret != 0 && !_linkg_nat_driver_module_loaded())
    {
        return ret;
    }

    if (!_linkg_nat_driver_module_loaded())
    {
        return -ENODEV;
    }

    g_nat.driver_loaded = true;

    return 0;
}

/**
 * @brief 卸载Fast NAT内核模块。
 *
 * 调用前必须关闭字符设备句柄，释放file_operations.owner产生的模块引用。
 */
static int _linkg_nat_driver_module_unload(void)
{
    int ret;

    if (!_linkg_nat_driver_module_loaded())
    {
        g_nat.driver_loaded = false;
        return 0;
    }

    ret = linkg_os_run("rmmod", LINKG_NAT_DRIVER_MODULE_NAME, NULL);
    if (ret != 0 && _linkg_nat_driver_module_loaded())
    {
        return ret;
    }

    if (_linkg_nat_driver_module_loaded())
    {
        return -EBUSY;
    }

    g_nat.driver_loaded = false;

    return 0;
}

/****************************** 驱动控制 ******************************/

/**
 * @brief 打开Fast NAT字符设备。
 */
static int _linkg_nat_driver_open(void)
{
    uint32_t attempt;
    int      saved_errno;
    int      fd;

    if (g_nat.driver_fd >= 0)
    {
        return 0;
    }

    saved_errno = ENODEV;

    for (attempt = 0U; attempt < LINKG_NAT_DRIVER_OPEN_RETRY_COUNT; attempt++)
    {
        fd = open(LINKG_FAST_NAT_DEVICE_PATH, O_RDWR | O_CLOEXEC);
        if (fd >= 0)
        {
            g_nat.driver_fd = fd;
            return 0;
        }

        saved_errno = errno;
        if (saved_errno != ENOENT && saved_errno != ENODEV)
        {
            return -saved_errno;
        }

        usleep(LINKG_NAT_DRIVER_OPEN_RETRY_US);
    }

    return -saved_errno;
}

/**
 * @brief 关闭Fast NAT字符设备。
 */
static int _linkg_nat_driver_close(void)
{
    int ret;

    if (g_nat.driver_fd < 0)
    {
        return 0;
    }

    ret = close(g_nat.driver_fd);

    g_nat.driver_fd = -1;

    if (ret != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 向Fast NAT驱动下发ioctl命令。
 */
static int _linkg_nat_driver_ioctl(unsigned long command, void *argument)
{
    int ret;

    if (g_nat.driver_fd < 0)
    {
        return -ENODEV;
    }

    ret = ioctl(g_nat.driver_fd, command, argument);
    if (ret < 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 向Fast NAT驱动设置本节点运行配置。
 */
static int _linkg_nat_driver_configure(void)
{
    linkg_fast_nat_config_t config;
    int                     ret;

    memset(&config, 0, sizeof(config));

    ret = _linkg_nat_build_driver_config(&config);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_nat_driver_ioctl(LINKG_FAST_NAT_IOC_SET_CONFIG, &config);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化NAT管理模块。
 *
 * 初始化阶段缓存当前节点网络参数和外部上行启用状态。
 * Fast NAT驱动需要依赖运行中的linkg0，因此字符设备配置在start阶段完成。
 */
int linkg_nat_init(const linkg_network_config_t *network_config, bool uplink_enabled)
{
    linkg_nat_context_t context;
    int                 ret;

    if (network_config == NULL)
    {
        return -EINVAL;
    }

    if (g_nat.initialized)
    {
        return 0;
    }

    memset(&context, 0, sizeof(context));

    ret = _linkg_nat_build_context(network_config, &context);
    if (ret != 0)
    {
        return ret;
    }

    context.uplink_enabled = uplink_enabled;
    context.initialized    = true;

    g_nat = context;

    return 0;
}

/**
 * @brief 启动Fast NAT。
 *
 * linkg0已经由TUN模块创建后解析Ethernet和TUN接口ifindex；
 * 外部蜂窝上行启用时额外解析Uplink接口ifindex，否则向驱动下发0表示禁用Uplink NAT。
 */
int linkg_nat_start(void)
{
    int ret;

    if (!g_nat.initialized)
    {
        return -ENODEV;
    }

    if (g_nat.started)
    {
        return 0;
    }

    /**
     * NAT控制模块拥有Fast NAT内核模块完整生命周期。
     * start首先加载ko，然后打开字符设备、下发配置并启动数据面。
     */
    ret = _linkg_nat_driver_module_load();
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_driver_open();
    if (ret != 0)
    {
        (void)_linkg_nat_driver_module_unload();
        return ret;
    }

    ret = _linkg_nat_driver_configure();
    if (ret != 0)
    {
        (void)_linkg_nat_driver_close();
        (void)_linkg_nat_driver_module_unload();
        return ret;
    }

    ret = _linkg_nat_driver_ioctl(LINKG_FAST_NAT_IOC_START, NULL);
    if (ret != 0)
    {
        (void)_linkg_nat_driver_close();
        (void)_linkg_nat_driver_module_unload();
        return ret;
    }

    g_nat.started = true;

    return 0;
}

/**
 * @brief 停止Fast NAT。
 */
int linkg_nat_stop(void)
{
    int first_error;
    int ret;

    if (!g_nat.initialized)
    {
        return -ENODEV;
    }

    first_error = 0;

    /**
     * 即使异常流程中started未置位，只要字符设备仍然打开，
     * 仍尝试STOP；驱动侧STOP本身为幂等操作。
     */
    if (g_nat.driver_fd >= 0)
    {
        ret = _linkg_nat_driver_ioctl(LINKG_FAST_NAT_IOC_STOP, NULL);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    g_nat.started = false;

    /**
     * 必须先close字符设备，释放THIS_MODULE引用，
     * 再卸载linkg_fast_nat.ko。
     */
    ret = _linkg_nat_driver_close();
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_driver_module_unload();
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/**
 * @brief 反初始化NAT管理模块。
 *
 * 调用前Fast NAT必须已经停止。
 */
int linkg_nat_deinit(void)
{
    int first_error;
    int ret;

    if (!g_nat.initialized)
    {
        return 0;
    }

    if (g_nat.started)
    {
        return -EBUSY;
    }

    first_error = 0;

    /**
     * 正常路径下stop已经完成close+rmmod。
     * 这里保留兜底清理，处理start中途失败等异常状态。
     */
    ret = _linkg_nat_driver_close();
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_driver_module_unload();
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    if (first_error != 0)
    {
        return first_error;
    }

    memset(&g_nat, 0, sizeof(g_nat));

    g_nat.driver_fd = -1;

    return 0;
}
