/**
 * @file linkg_udhcp.c
 * @brief LinkG Ethernet DHCP服务管理实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-09
 */

#include "linkg_udhcp.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <net/if.h>
#include <pthread.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "linkg_log.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_UDHCP_RUNTIME_ROOT                      "/var/run/linkg"                    // LinkG运行目录
#define LINKG_UDHCP_RUNTIME_DIRECTORY                 LINKG_UDHCP_RUNTIME_ROOT "/udhcp"    // DHCP运行目录
#define LINKG_UDHCP_CONFIG_FILE                       LINKG_UDHCP_RUNTIME_DIRECTORY "/udhcpd.conf" // udhcpd正式配置文件
#define LINKG_UDHCP_CONFIG_TEMP_FILE                  LINKG_UDHCP_RUNTIME_DIRECTORY "/udhcpd.conf.tmp" // udhcpd临时配置文件
#define LINKG_UDHCP_LEASE_FILE                        LINKG_UDHCP_RUNTIME_DIRECTORY "/udhcpd.leases" // DHCP租约文件
#define LINKG_UDHCP_PID_FILE                          LINKG_UDHCP_RUNTIME_DIRECTORY "/udhcpd.pid" // udhcpd PID文件
#define LINKG_UDHCP_EXECUTABLE_USR_SBIN               "/usr/sbin/udhcpd"                  // 独立udhcpd常用路径
#define LINKG_UDHCP_EXECUTABLE_SBIN                   "/sbin/udhcpd"                      // 独立udhcpd备用路径
#define LINKG_UDHCP_EXECUTABLE_USR_BIN                "/usr/bin/udhcpd"                   // udhcpd备用路径
#define LINKG_UDHCP_EXECUTABLE_BIN                    "/bin/udhcpd"                       // udhcpd备用路径
#define LINKG_UDHCP_BUSYBOX_USR_BIN                   "/usr/bin/busybox"                  // BusyBox常用路径
#define LINKG_UDHCP_BUSYBOX_BIN                       "/bin/busybox"                      // BusyBox备用路径
#define LINKG_UDHCP_POOL_FIRST_HOST_ID                2U                                  // DHCP地址池首个主机编号
#define LINKG_UDHCP_DNS_PRIMARY                       "223.5.5.5"                         // 默认主DNS服务器
#define LINKG_UDHCP_DNS_SECONDARY                     "223.6.6.6"                         // 默认备用DNS服务器
#define LINKG_UDHCP_LEASE_TIME_SEC                    600U                                // DHCP默认租约时长，单位秒
#define LINKG_UDHCP_LEASE_WRITE_INTERVAL_SEC          60U                                 // 租约文件自动写回周期，单位秒
#define LINKG_UDHCP_PROCESS_START_PROBE_COUNT         20U                                 // udhcpd启动存活探测次数
#define LINKG_UDHCP_PROCESS_START_PROBE_INTERVAL_US   10000U                              // udhcpd启动存活探测间隔
#define LINKG_UDHCP_PROCESS_STOP_WAIT_COUNT           50U                                 // udhcpd正常停止等待次数
#define LINKG_UDHCP_PROCESS_STOP_WAIT_INTERVAL_US     20000U                              // udhcpd正常停止等待间隔
#define LINKG_UDHCP_PROCESS_LEASE_FLUSH_WAIT_US       20000U                              // SIGUSR1租约写回等待时间

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_network_ipv4_config_t ethernet_network; // 本节点Ethernet网络
    linkg_network_ipv4_config_t ethernet;         // 本节点Ethernet接口IPv4配置
    linkg_network_ipv4_config_t virtual_network;  // LinkG虚拟聚合网络
    bool                        enabled;          // 是否启用DHCP服务
    bool                        default_gateway;  // 是否向客户端下发默认网关
} linkg_udhcp_runtime_config_t;

typedef struct
{
    pthread_mutex_t              control_lock; // 生命周期与动态配置串行锁
    linkg_udhcp_runtime_config_t config;       // 当前DHCP运行配置
    pid_t                        process_id;   // 当前托管的udhcpd子进程PID
    bool                         initialized;  // 模块是否已经初始化
    bool                         started;      // 模块生命周期是否已经启动
} linkg_udhcp_context_t;

/****************************** 全局上下文 ******************************/

/**
 * @brief DHCP模块全局运行上下文。
 *
 * @note control_lock允许Web配置更新与Network Service生命周期串行执行。
 *       进程启停和配置文件切换期间会持有该锁，避免两个udhcpd实例并发绑定DHCP端口。
 */
static linkg_udhcp_context_t g_udhcp =
{
    .control_lock = PTHREAD_MUTEX_INITIALIZER, // 生命周期与动态配置串行锁
    .process_id   = -1,                        // udhcpd尚未启动
    .initialized  = false,                     // DHCP模块尚未初始化
    .started      = false                      // DHCP模块尚未启动
};

/****************************** 配置辅助 ******************************/

/**
 * @brief 根据LinkG网络配置构造DHCP运行配置。
 */
static int _linkg_udhcp_build_runtime_config(const linkg_network_config_t *network_config, linkg_udhcp_runtime_config_t *runtime_config)
{
    int ret;

    if (network_config == NULL || runtime_config == NULL)
    {
        return -EINVAL;
    }

    memset(runtime_config, 0, sizeof(*runtime_config));

    ret = linkg_network_config_get_ethernet_network(network_config, &runtime_config->ethernet_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_ethernet(network_config, &runtime_config->ethernet);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_virtual_network(network_config, &runtime_config->virtual_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    runtime_config->enabled         = network_config->dhcp.enabled;
    runtime_config->default_gateway = network_config->dhcp.default_gateway;

    return 0;
}

/**
 * @brief 判断两份DHCP运行配置是否完全一致。
 */
static bool _linkg_udhcp_runtime_config_equal(const linkg_udhcp_runtime_config_t *left, const linkg_udhcp_runtime_config_t *right)
{
    if (left == NULL || right == NULL)
    {
        return false;
    }

    return left->ethernet_network.ip.s_addr == right->ethernet_network.ip.s_addr &&
           left->ethernet_network.netmask.s_addr == right->ethernet_network.netmask.s_addr &&
           left->ethernet.ip.s_addr == right->ethernet.ip.s_addr &&
           left->ethernet.netmask.s_addr == right->ethernet.netmask.s_addr &&
           left->virtual_network.ip.s_addr == right->virtual_network.ip.s_addr &&
           left->virtual_network.netmask.s_addr == right->virtual_network.netmask.s_addr &&
           left->enabled == right->enabled &&
           left->default_gateway == right->default_gateway;
}

/**
 * @brief 判断Ethernet地址池是否因为配置变化而改变。
 */
static bool _linkg_udhcp_ethernet_pool_changed(const linkg_udhcp_runtime_config_t *left, const linkg_udhcp_runtime_config_t *right)
{
    if (left == NULL || right == NULL)
    {
        return true;
    }

    return left->ethernet_network.ip.s_addr != right->ethernet_network.ip.s_addr ||
           left->ethernet_network.netmask.s_addr != right->ethernet_network.netmask.s_addr ||
           left->ethernet.ip.s_addr != right->ethernet.ip.s_addr;
}

/**
 * @brief 将连续IPv4子网掩码转换为CIDR前缀长度。
 */
static int _linkg_udhcp_netmask_to_prefix(const struct in_addr *netmask, uint8_t *prefix)
{
    uint32_t mask;
    uint32_t bit;
    uint8_t  count;
    bool     zero_seen;

    if (netmask == NULL || prefix == NULL)
    {
        return -EINVAL;
    }

    mask      = ntohl(netmask->s_addr);
    count     = 0U;
    zero_seen = false;

    for (bit = 0U; bit < 32U; bit++)
    {
        if ((mask & (1U << (31U - bit))) != 0U)
        {
            if (zero_seen)
            {
                return -EINVAL;
            }

            count++;
            continue;
        }

        zero_seen = true;
    }

    *prefix = count;

    return 0;
}

/**
 * @brief 将IPv4地址转换为点分十进制字符串。
 */
static int _linkg_udhcp_ipv4_to_string(const struct in_addr *address, char *buffer, size_t buffer_size)
{
    if (address == NULL || buffer == NULL || buffer_size < INET_ADDRSTRLEN)
    {
        return -EINVAL;
    }

    if (inet_ntop(AF_INET, address, buffer, (socklen_t)buffer_size) == NULL)
    {
        return errno != 0 ? -errno : -EINVAL;
    }

    return 0;
}

/**
 * @brief 根据Ethernet网络计算DHCP地址池起止地址和最大租约数量。
 */
static int _linkg_udhcp_build_pool(const linkg_udhcp_runtime_config_t *config, struct in_addr *start, struct in_addr *end, uint32_t *max_leases)
{
    uint32_t network;
    uint32_t netmask;
    uint32_t broadcast;
    uint32_t gateway;
    uint32_t start_host;
    uint32_t end_host;

    if (config == NULL || start == NULL || end == NULL || max_leases == NULL)
    {
        return -EINVAL;
    }

    network   = ntohl(config->ethernet_network.ip.s_addr);
    netmask   = ntohl(config->ethernet_network.netmask.s_addr);
    gateway   = ntohl(config->ethernet.ip.s_addr);
    network  &= netmask;
    broadcast = network | ~netmask;

    if (gateway != network + 1U)
    {
        return -EINVAL;
    }

    start_host = network + LINKG_UDHCP_POOL_FIRST_HOST_ID;

    if (broadcast == 0U)
    {
        return -ERANGE;
    }

    end_host = broadcast - 1U;

    if (start_host > end_host)
    {
        return -ERANGE;
    }

    if (gateway >= start_host && gateway <= end_host)
    {
        return -EINVAL;
    }

    start->s_addr = htonl(start_host);
    end->s_addr   = htonl(end_host);
    *max_leases   = end_host - start_host + 1U;

    return 0;
}

/****************************** 文件管理 ******************************/

/**
 * @brief 创建并校验指定运行目录。
 */
static int _linkg_udhcp_directory_prepare(const char *path)
{
    struct stat status;

    if (path == NULL)
    {
        return -EINVAL;
    }

    if (mkdir(path, 0755) == 0)
    {
        return 0;
    }

    if (errno != EEXIST)
    {
        return -errno;
    }

    if (stat(path, &status) != 0)
    {
        return -errno;
    }

    if (!S_ISDIR(status.st_mode))
    {
        return -ENOTDIR;
    }

    return 0;
}

/**
 * @brief 创建并校验DHCP运行目录。
 */
static int _linkg_udhcp_runtime_directory_prepare(void)
{
    int ret;

    ret = _linkg_udhcp_directory_prepare(LINKG_UDHCP_RUNTIME_ROOT);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_udhcp_directory_prepare(LINKG_UDHCP_RUNTIME_DIRECTORY);
}

/**
 * @brief 确保DHCP租约文件存在。
 */
static int _linkg_udhcp_lease_file_prepare(void)
{
    int fd;

    fd = open(LINKG_UDHCP_LEASE_FILE, O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0)
    {
        return -errno;
    }

    if (close(fd) != 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 删除旧Ethernet网段对应的DHCP租约。
 */
static int _linkg_udhcp_lease_file_reset(void)
{
    if (unlink(LINKG_UDHCP_LEASE_FILE) != 0 && errno != ENOENT)
    {
        return -errno;
    }

    return _linkg_udhcp_lease_file_prepare();
}

/**
 * @brief 将当前运行配置原子写入udhcpd配置文件。
 */
static int _linkg_udhcp_config_file_write(const linkg_udhcp_runtime_config_t *config)
{
    struct in_addr pool_start;
    struct in_addr pool_end;
    uint32_t       max_leases;
    uint8_t        virtual_prefix;
    char           pool_start_text[INET_ADDRSTRLEN];
    char           pool_end_text[INET_ADDRSTRLEN];
    char           gateway_text[INET_ADDRSTRLEN];
    char           netmask_text[INET_ADDRSTRLEN];
    char           virtual_network_text[INET_ADDRSTRLEN];
    FILE          *stream;
    int            saved_errno;
    int            ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_udhcp_build_pool(config, &pool_start, &pool_end, &max_leases);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_udhcp_netmask_to_prefix(&config->virtual_network.netmask, &virtual_prefix);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_udhcp_ipv4_to_string(&pool_start, pool_start_text, sizeof(pool_start_text));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_udhcp_ipv4_to_string(&pool_end, pool_end_text, sizeof(pool_end_text));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_udhcp_ipv4_to_string(&config->ethernet.ip, gateway_text, sizeof(gateway_text));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_udhcp_ipv4_to_string(&config->ethernet.netmask, netmask_text, sizeof(netmask_text));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_udhcp_ipv4_to_string(&config->virtual_network.ip, virtual_network_text, sizeof(virtual_network_text));
    if (ret != 0)
    {
        return ret;
    }

    stream = fopen(LINKG_UDHCP_CONFIG_TEMP_FILE, "w");
    if (stream == NULL)
    {
        return -errno;
    }

    if (fprintf(stream, "start %s\n", pool_start_text) < 0 ||
        fprintf(stream, "end %s\n", pool_end_text) < 0 ||
        fprintf(stream, "interface %s\n", LINKG_RESOURCE_INTERFACE_ETHERNET) < 0 ||
        fprintf(stream, "max_leases %u\n", (unsigned int)max_leases) < 0 ||
        fprintf(stream, "pidfile %s\n", LINKG_UDHCP_PID_FILE) < 0 ||
        fprintf(stream, "lease_file %s\n", LINKG_UDHCP_LEASE_FILE) < 0 ||
        fprintf(stream, "auto_time %u\n", LINKG_UDHCP_LEASE_WRITE_INTERVAL_SEC) < 0 ||
        fprintf(stream, "option subnet %s\n", netmask_text) < 0 ||
        fprintf(stream, "option lease %u\n", LINKG_UDHCP_LEASE_TIME_SEC) < 0)
    {
        saved_errno = errno != 0 ? errno : EIO;
        (void)fclose(stream);
        (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
        return -saved_errno;
    }

    /**
     * Virtual Network路由始终通过Option 121/249下发。
     * Option 121为RFC3442标准Classless Static Route；
     * Option 249用于兼容Microsoft DHCP客户端。
     *
     * 不发送Option 33：其Classful语义在现代Windows上会额外生成
     * 网络基地址对应的/32 Host Route，造成无意义的重复路由。
     */
    if (config->default_gateway)
    {
        /**
         * 支持Option 121的客户端按照RFC3442会忽略Router Option，
         * 因此开启默认网关时必须同时在121/249中加入0.0.0.0/0。
         *
         * DNS仅在LinkG接管客户端默认Internet出口时下发，避免Split Routing模式
         * 覆盖客户端其他网络接口已有的DNS配置。
         */
        if (fprintf(stream, "option router %s\n", gateway_text) < 0 ||
            fprintf(stream, "option dns %s %s\n", LINKG_UDHCP_DNS_PRIMARY, LINKG_UDHCP_DNS_SECONDARY) < 0 ||
            fprintf(stream,
                    "option staticroutes 0.0.0.0/0 %s, %s/%u %s\n",
                    gateway_text,
                    virtual_network_text,
                    (unsigned int)virtual_prefix,
                    gateway_text) < 0 ||
            fprintf(stream,
                    "option msstaticroutes 0.0.0.0/0 %s, %s/%u %s\n",
                    gateway_text,
                    virtual_network_text,
                    (unsigned int)virtual_prefix,
                    gateway_text) < 0)
        {
            saved_errno = errno != 0 ? errno : EIO;
            (void)fclose(stream);
            (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
            return -saved_errno;
        }
    }
    else
    {
        if (fprintf(stream,
                    "option staticroutes %s/%u %s\n",
                    virtual_network_text,
                    (unsigned int)virtual_prefix,
                    gateway_text) < 0 ||
            fprintf(stream,
                    "option msstaticroutes %s/%u %s\n",
                    virtual_network_text,
                    (unsigned int)virtual_prefix,
                    gateway_text) < 0)
        {
            saved_errno = errno != 0 ? errno : EIO;
            (void)fclose(stream);
            (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
            return -saved_errno;
        }
    }

    if (fflush(stream) != 0)
    {
        saved_errno = errno;
        (void)fclose(stream);
        (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
        return -saved_errno;
    }

    if (fsync(fileno(stream)) != 0)
    {
        saved_errno = errno;
        (void)fclose(stream);
        (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
        return -saved_errno;
    }

    if (fclose(stream) != 0)
    {
        saved_errno = errno;
        (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
        return -saved_errno;
    }

    if (rename(LINKG_UDHCP_CONFIG_TEMP_FILE, LINKG_UDHCP_CONFIG_FILE) != 0)
    {
        saved_errno = errno;
        (void)unlink(LINKG_UDHCP_CONFIG_TEMP_FILE);
        return -saved_errno;
    }

    LINKG_LOG_DEBUG("DHCP configuration generated, default_gateway=%d, option33=%d, dns=%d",
                    config->default_gateway ? 1 : 0,
                    option33_supported ? 1 : 0,
                    config->default_gateway ? 1 : 0);

    return 0;
}

/****************************** 进程管理 ******************************/

/**
 * @brief 查找目标系统可执行的udhcpd或BusyBox程序。
 */
static int _linkg_udhcp_executable_resolve(const char **path, bool *busybox)
{
    if (path == NULL || busybox == NULL)
    {
        return -EINVAL;
    }

    if (access(LINKG_UDHCP_EXECUTABLE_USR_SBIN, X_OK) == 0)
    {
        *path    = LINKG_UDHCP_EXECUTABLE_USR_SBIN;
        *busybox = false;
        return 0;
    }

    if (access(LINKG_UDHCP_EXECUTABLE_SBIN, X_OK) == 0)
    {
        *path    = LINKG_UDHCP_EXECUTABLE_SBIN;
        *busybox = false;
        return 0;
    }

    if (access(LINKG_UDHCP_EXECUTABLE_USR_BIN, X_OK) == 0)
    {
        *path    = LINKG_UDHCP_EXECUTABLE_USR_BIN;
        *busybox = false;
        return 0;
    }

    if (access(LINKG_UDHCP_EXECUTABLE_BIN, X_OK) == 0)
    {
        *path    = LINKG_UDHCP_EXECUTABLE_BIN;
        *busybox = false;
        return 0;
    }

    if (access(LINKG_UDHCP_BUSYBOX_USR_BIN, X_OK) == 0)
    {
        *path    = LINKG_UDHCP_BUSYBOX_USR_BIN;
        *busybox = true;
        return 0;
    }

    if (access(LINKG_UDHCP_BUSYBOX_BIN, X_OK) == 0)
    {
        *path    = LINKG_UDHCP_BUSYBOX_BIN;
        *busybox = true;
        return 0;
    }

    return -ENOENT;
}

/**
 * @brief 检查当前托管的udhcpd子进程是否仍然存活。
 *
 * @note 调用方必须持有control_lock。本函数会回收已经退出的子进程。
 */
static bool _linkg_udhcp_process_alive_locked(void)
{
    int   status;
    pid_t ret;

    if (g_udhcp.process_id <= 0)
    {
        return false;
    }

    do
    {
        ret = waitpid(g_udhcp.process_id, &status, WNOHANG);
    }
    while (ret < 0 && errno == EINTR);

    if (ret == 0)
    {
        return true;
    }

    if (ret == g_udhcp.process_id || (ret < 0 && errno == ECHILD))
    {
        g_udhcp.process_id = -1;
        (void)unlink(LINKG_UDHCP_PID_FILE);
        return false;
    }

    return true;
}

/**
 * @brief 启动前台udhcpd子进程并确认其完成初始配置加载。
 *
 * @note 调用方必须持有control_lock。udhcpd保持前台运行，由LinkG直接持有PID并负责回收。
 */
static int _linkg_udhcp_process_start_locked(void)
{
    const char *executable;
    bool        busybox;
    uint32_t    attempt;
    int         status;
    int         ret;
    pid_t       pid;
    pid_t       wait_result;
    sigset_t    empty_set;

    if (_linkg_udhcp_process_alive_locked())
    {
        return 0;
    }

    if (if_nametoindex(LINKG_RESOURCE_INTERFACE_ETHERNET) == 0U)
    {
        return errno != 0 ? -errno : -ENODEV;
    }

    executable = NULL;
    busybox    = false;

    ret = _linkg_udhcp_executable_resolve(&executable, &busybox);
    if (ret != 0)
    {
        return ret;
    }

    if (sigemptyset(&empty_set) != 0)
    {
        return -errno;
    }

    (void)unlink(LINKG_UDHCP_PID_FILE);

    pid = fork();
    if (pid < 0)
    {
        return -errno;
    }

    if (pid == 0)
    {
        char *const busybox_argv[] = {"busybox", "udhcpd", "-f", LINKG_UDHCP_CONFIG_FILE, NULL};
        char *const udhcpd_argv[]  = {"udhcpd", "-f", LINKG_UDHCP_CONFIG_FILE, NULL};

        // 清除父线程可能继承的阻塞信号集合，保证udhcpd能够处理TERM和USR1。
        if (sigprocmask(SIG_SETMASK, &empty_set, NULL) != 0)
        {
            _exit(126);
        }

        // LinkG主进程异常退出时同步终止udhcpd，避免残留DHCP服务继续响应客户端。
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) != 0)
        {
            _exit(126);
        }

        if (busybox)
        {
            execv(executable, busybox_argv);
        }
        else
        {
            execv(executable, udhcpd_argv);
        }

        _exit(127);
    }

    for (attempt = 0U; attempt < LINKG_UDHCP_PROCESS_START_PROBE_COUNT; attempt++)
    {
        do
        {
            wait_result = waitpid(pid, &status, WNOHANG);
        }
        while (wait_result < 0 && errno == EINTR);

        if (wait_result == 0)
        {
            if (attempt + 1U < LINKG_UDHCP_PROCESS_START_PROBE_COUNT)
            {
                usleep(LINKG_UDHCP_PROCESS_START_PROBE_INTERVAL_US);
            }

            continue;
        }

        if (wait_result == pid)
        {
            if (WIFEXITED(status) && WEXITSTATUS(status) == 127)
            {
                return -ENOENT;
            }

            LINKG_LOG_ERROR("udhcpd exited during startup, status=%d", status);
            return -EIO;
        }

        if (wait_result < 0)
        {
            return -errno;
        }
    }

    g_udhcp.process_id = pid;

    LINKG_LOG_INFO("udhcpd started, pid=%d, default_gateway=%d",
                   (int)g_udhcp.process_id,
                   g_udhcp.config.default_gateway ? 1 : 0);

    return 0;
}

/**
 * @brief 停止并回收当前托管的udhcpd子进程。
 *
 * @note 调用方必须持有control_lock。停止前先请求udhcpd立即写回租约文件。
 */
static int _linkg_udhcp_process_stop_locked(void)
{
    uint32_t attempt;
    int      first_error;
    int      status;
    pid_t    pid;
    pid_t    wait_result;

    if (!_linkg_udhcp_process_alive_locked())
    {
        g_udhcp.process_id = -1;
        (void)unlink(LINKG_UDHCP_PID_FILE);
        return 0;
    }

    pid         = g_udhcp.process_id;
    first_error = 0;

    // BusyBox udhcpd使用SIGUSR1立即刷新租约文件，尽量保留客户端现有租约。
    if (kill(pid, SIGUSR1) != 0 && errno != ESRCH)
    {
        first_error = -errno;
    }

    usleep(LINKG_UDHCP_PROCESS_LEASE_FLUSH_WAIT_US);

    if (kill(pid, SIGTERM) != 0 && errno != ESRCH && first_error == 0)
    {
        first_error = -errno;
    }

    for (attempt = 0U; attempt < LINKG_UDHCP_PROCESS_STOP_WAIT_COUNT; attempt++)
    {
        do
        {
            wait_result = waitpid(pid, &status, WNOHANG);
        }
        while (wait_result < 0 && errno == EINTR);

        if (wait_result == pid)
        {
            g_udhcp.process_id = -1;
            (void)unlink(LINKG_UDHCP_PID_FILE);
            return first_error;
        }

        if (wait_result < 0)
        {
            if (errno == ECHILD)
            {
                g_udhcp.process_id = -1;
                (void)unlink(LINKG_UDHCP_PID_FILE);
                return first_error;
            }

            if (first_error == 0)
            {
                first_error = -errno;
            }

            break;
        }

        usleep(LINKG_UDHCP_PROCESS_STOP_WAIT_INTERVAL_US);
    }

    if (kill(pid, SIGKILL) != 0 && errno != ESRCH)
    {
        if (first_error == 0)
        {
            first_error = -errno;
        }

        return first_error;
    }

    do
    {
        wait_result = waitpid(pid, &status, 0);
    }
    while (wait_result < 0 && errno == EINTR);

    if (wait_result < 0 && errno != ECHILD && first_error == 0)
    {
        first_error = -errno;
    }

    g_udhcp.process_id = -1;
    (void)unlink(LINKG_UDHCP_PID_FILE);

    LINKG_LOG_INFO("udhcpd stopped");

    return first_error;
}

/****************************** 服务控制 ******************************/

/**
 * @brief 按当前运行配置启动DHCP服务。
 *
 * @note 调用方必须持有control_lock。DHCP被禁用时只保持模块生命周期为启动状态，不创建udhcpd进程。
 */
static int _linkg_udhcp_service_start_locked(void)
{
    int ret;

    if (!g_udhcp.config.enabled)
    {
        return 0;
    }

    ret = _linkg_udhcp_runtime_directory_prepare();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("prepare DHCP runtime directory failed, path=%s, error=%d",
                        LINKG_UDHCP_RUNTIME_DIRECTORY,
                        ret);
        return ret;
    }

    ret = _linkg_udhcp_lease_file_prepare();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("prepare DHCP lease file failed, path=%s, error=%d",
                        LINKG_UDHCP_LEASE_FILE,
                        ret);
        return ret;
    }

    ret = _linkg_udhcp_config_file_write(&g_udhcp.config);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("write DHCP configuration failed, path=%s, error=%d",
                        LINKG_UDHCP_CONFIG_FILE,
                        ret);
        return ret;
    }

    ret = _linkg_udhcp_process_start_locked();
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start udhcpd process failed, error=%d", ret);
        return ret;
    }

    return 0;
}

/**
 * @brief 按当前运行状态停止DHCP服务。
 *
 * @note 调用方必须持有control_lock。
 */
static int _linkg_udhcp_service_stop_locked(void)
{
    return _linkg_udhcp_process_stop_locked();
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化DHCP服务管理模块。
 *
 * 初始化阶段仅缓存DHCP相关网络配置，不创建文件也不启动udhcpd。
 */
int linkg_udhcp_init(const linkg_network_config_t *network_config)
{
    linkg_udhcp_runtime_config_t runtime_config;
    int                          lock_ret;
    int                          ret;

    ret = _linkg_udhcp_build_runtime_config(network_config, &runtime_config);
    if (ret != 0)
    {
        return ret;
    }

    lock_ret = pthread_mutex_lock(&g_udhcp.control_lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (g_udhcp.initialized)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return 0;
    }

    g_udhcp.config      = runtime_config;
    g_udhcp.process_id  = -1;
    g_udhcp.initialized = true;
    g_udhcp.started     = false;

    ret = pthread_mutex_unlock(&g_udhcp.control_lock);
    if (ret != 0)
    {
        return -ret;
    }

    LINKG_LOG_INFO("DHCP module initialized, enabled=%d, default_gateway=%d",
                   runtime_config.enabled ? 1 : 0,
                   runtime_config.default_gateway ? 1 : 0);

    return 0;
}

/**
 * @brief 启动DHCP模块生命周期和当前配置对应的udhcpd服务。
 */
int linkg_udhcp_start(void)
{
    int lock_ret;
    int ret;

    lock_ret = pthread_mutex_lock(&g_udhcp.control_lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!g_udhcp.initialized)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return -ENODEV;
    }

    if (g_udhcp.started)
    {
        if (!g_udhcp.config.enabled || _linkg_udhcp_process_alive_locked())
        {
            (void)pthread_mutex_unlock(&g_udhcp.control_lock);
            return 0;
        }

        ret = _linkg_udhcp_service_start_locked();
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return ret;
    }

    ret = _linkg_udhcp_service_start_locked();
    if (ret == 0)
    {
        g_udhcp.started = true;
    }

    lock_ret = pthread_mutex_unlock(&g_udhcp.control_lock);
    if (lock_ret != 0 && ret == 0)
    {
        return -lock_ret;
    }

    if (ret != 0)
    {
        LINKG_LOG_ERROR("start DHCP module failed, error=%d", ret);
        return ret;
    }

    if (g_udhcp.config.enabled)
    {
        LINKG_LOG_INFO("DHCP module started");
    }
    else
    {
        LINKG_LOG_INFO("DHCP module started with service disabled");
    }

    return 0;
}

/**
 * @brief 停止udhcpd服务并结束DHCP模块运行状态。
 */
int linkg_udhcp_stop(void)
{
    int lock_ret;
    int ret;

    lock_ret = pthread_mutex_lock(&g_udhcp.control_lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!g_udhcp.initialized)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return -ENODEV;
    }

    if (!g_udhcp.started)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return 0;
    }

    ret = _linkg_udhcp_service_stop_locked();
    g_udhcp.started = false;

    lock_ret = pthread_mutex_unlock(&g_udhcp.control_lock);
    if (lock_ret != 0 && ret == 0)
    {
        return -lock_ret;
    }

    if (ret != 0)
    {
        LINKG_LOG_ERROR("stop DHCP module failed, error=%d", ret);
        return ret;
    }

    LINKG_LOG_INFO("DHCP module stopped");

    return 0;
}

/**
 * @brief 反初始化DHCP服务管理模块。
 *
 * @note 调用前模块必须已经停止。运行目录和租约文件保留，用于下次启动继续复用租约。
 */
int linkg_udhcp_deinit(void)
{
    int lock_ret;
    int ret;

    lock_ret = pthread_mutex_lock(&g_udhcp.control_lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!g_udhcp.initialized)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return 0;
    }

    if (g_udhcp.started)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return -EBUSY;
    }

    if (_linkg_udhcp_process_alive_locked())
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return -EBUSY;
    }

    memset(&g_udhcp.config, 0, sizeof(g_udhcp.config));

    g_udhcp.process_id  = -1;
    g_udhcp.initialized = false;
    g_udhcp.started     = false;

    ret = pthread_mutex_unlock(&g_udhcp.control_lock);
    if (ret != 0)
    {
        return -ret;
    }

    LINKG_LOG_INFO("DHCP module deinitialized");

    return 0;
}

/****************************** 配置接口 ******************************/

/**
 * @brief 动态应用新的DHCP网络配置。
 *
 * 未启动时只更新缓存；运行中则仅重启udhcpd子进程，不重启LinkG主程序。
 * Ethernet网段变化时旧租约失效并清空；仅切换default_gateway时保留原租约文件。
 */
int linkg_udhcp_reload(const linkg_network_config_t *network_config)
{
    linkg_udhcp_runtime_config_t new_config;
    linkg_udhcp_runtime_config_t old_config;
    bool                         ethernet_pool_changed;
    bool                         old_enabled;
    int                          first_error;
    int                          lock_ret;
    int                          ret;
    int                          rollback_ret;

    ret = _linkg_udhcp_build_runtime_config(network_config, &new_config);
    if (ret != 0)
    {
        return ret;
    }

    lock_ret = pthread_mutex_lock(&g_udhcp.control_lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!g_udhcp.initialized)
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return -ENODEV;
    }

    if (_linkg_udhcp_runtime_config_equal(&g_udhcp.config, &new_config))
    {
        (void)pthread_mutex_unlock(&g_udhcp.control_lock);
        return 0;
    }

    old_config            = g_udhcp.config;
    old_enabled           = old_config.enabled;
    ethernet_pool_changed = _linkg_udhcp_ethernet_pool_changed(&old_config, &new_config);

    if (!g_udhcp.started)
    {
        g_udhcp.config = new_config;

        if (ethernet_pool_changed)
        {
            ret = _linkg_udhcp_runtime_directory_prepare();
            if (ret == 0)
            {
                ret = _linkg_udhcp_lease_file_reset();
            }

            if (ret != 0)
            {
                g_udhcp.config = old_config;
                (void)pthread_mutex_unlock(&g_udhcp.control_lock);
                return ret;
            }
        }

        ret = pthread_mutex_unlock(&g_udhcp.control_lock);
        if (ret != 0)
        {
            return -ret;
        }

        LINKG_LOG_INFO("DHCP configuration updated while stopped, enabled=%d, default_gateway=%d",
                       new_config.enabled ? 1 : 0,
                       new_config.default_gateway ? 1 : 0);

        return 0;
    }

    first_error = 0;

    if (_linkg_udhcp_process_alive_locked())
    {
        ret = _linkg_udhcp_service_stop_locked();
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    if (first_error == 0 && ethernet_pool_changed)
    {
        ret = _linkg_udhcp_runtime_directory_prepare();
        if (ret == 0)
        {
            ret = _linkg_udhcp_lease_file_reset();
        }

        if (ret != 0)
        {
            first_error = ret;
        }
    }

    if (first_error == 0)
    {
        g_udhcp.config = new_config;

        ret = _linkg_udhcp_service_start_locked();
        if (ret == 0)
        {
            lock_ret = pthread_mutex_unlock(&g_udhcp.control_lock);
            if (lock_ret != 0)
            {
                return -lock_ret;
            }

            LINKG_LOG_INFO("DHCP configuration reloaded, enabled=%d, default_gateway=%d",
                           new_config.enabled ? 1 : 0,
                           new_config.default_gateway ? 1 : 0);

            return 0;
        }

        first_error = ret;
    }

    // 新配置应用失败时恢复旧配置，尽量保持原DHCP能力继续工作。
    g_udhcp.config = old_config;

    rollback_ret = 0;

    if (old_enabled)
    {
        rollback_ret = _linkg_udhcp_service_start_locked();
    }

    lock_ret = pthread_mutex_unlock(&g_udhcp.control_lock);
    if (lock_ret != 0 && rollback_ret == 0)
    {
        rollback_ret = -lock_ret;
    }

    if (rollback_ret != 0)
    {
        LINKG_LOG_ERROR("DHCP reload failed and rollback failed, reload_error=%d, rollback_error=%d",
                        first_error,
                        rollback_ret);
    }
    else
    {
        LINKG_LOG_ERROR("DHCP reload failed, old configuration restored, error=%d", first_error);
    }

    return first_error;
}
