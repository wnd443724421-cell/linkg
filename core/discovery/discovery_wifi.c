/**
 * @file discovery_wifi.c
 * @brief LinkG设备发现Wi-Fi Channel实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-23
 */

#include "discovery_wifi.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_device_config.h"
#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

#include "discovery_channel.h"
#include "discovery_internal.h"
#include "discovery_types.h"
#include "discovery_wire.h"

/****************************** 运行参数 ******************************/

#define LINKG_DISCOVERY_WIFI_THREAD_NAME          "discovery-wifi" // Wi-Fi Discovery线程名称
#define LINKG_DISCOVERY_WIFI_REPORT_INTERVAL_US    1000000ULL      // 已知Peer状态单播间隔，1s
#define LINKG_DISCOVERY_WIFI_BROADCAST_INTERVAL_US 5000000ULL      // 新节点发现广播间隔，5s
#define LINKG_DISCOVERY_WIFI_MONITOR_INTERVAL_MS   500             // 接口状态检查及等待兜底间隔
#define LINKG_DISCOVERY_WIFI_RX_BUDGET             64U             // 单次最多处理的接收报文数量
#define LINKG_DISCOVERY_WIFI_IP_TOS                0xC0            // Discovery控制流量，映射WMM AC_VO

/****************************** 模块上下文 ******************************/

typedef struct
{
    struct sockaddr_in address;      // STA的Wi-Fi Discovery控制地址
    uint64_t           session_id;   // 最近确认的STA Session ID
    uint64_t           revision;     // 最近确认的STA Report版本
    uint64_t           last_seen_us; // 最近收到有效STA Report的时间
    uint8_t            node_id;      // STA节点编号
    bool               valid;        // 该地址是否有效
} linkg_discovery_wifi_sta_address_t;

typedef struct
{
    pthread_mutex_t                    lock;                               // Wi-Fi Discovery运行状态保护锁
    linkg_thread_t                     thread;                             // Wi-Fi Discovery工作线程
    struct sockaddr_in                 broadcast_address;                  // AP新节点发现广播目标
    struct sockaddr_in                 ap_address;                         // STA当前学习到的AP控制地址
    linkg_discovery_wifi_sta_address_t sta_addresses[LINKG_NODE_PEER_MAX]; // AP已知STA控制地址
    struct in_addr                     bound_ipv4;                         // 当前Socket建立时的接口IPv4
    uint64_t                           next_report_us;                     // 下一次已知Peer单播时间
    uint64_t                           next_broadcast_us;                  // 下一次AP发现广播时间
    uint64_t                           ap_session_id;                      // STA当前AP Session ID
    uint64_t                           ap_revision;                        // STA当前AP Report版本
    linkg_device_role_t                role;                               // 本机设备角色
    unsigned int                       bound_ifindex;                      // 当前Socket绑定时的接口索引
    int                                socket_fd;                          // 广播/单播共用Wi-Fi UDP Socket
    int                                run_error;                          // Worker不可恢复错误
    bool                               ap_address_valid;                   // STA是否已学习AP控制地址
    bool                               channel_registered;                 // Core Wi-Fi Channel是否注册
    bool                               initialized;                        // 模块是否初始化
    bool                               running;                            // Start生命周期是否激活
} linkg_discovery_wifi_context_t;

/****************************** 全局上下文 ******************************/

static linkg_discovery_wifi_context_t g_discovery_wifi =
{
    .lock      = PTHREAD_MUTEX_INITIALIZER,
    .role      = LINKG_DEVICE_ROLE_UNKNOWN,
    .socket_fd = -1
};

/****************************** 前置声明 ******************************/

static void _linkg_discovery_wifi_thread(linkg_thread_t *thread, void *user_data);

/****************************** 上下文辅助 ******************************/

/** @brief 清空动态状态。调用方必须持有Wi-Fi状态锁，且Socket已关闭。 */
static void _linkg_discovery_wifi_reset_runtime_locked(void)
{
    memset(&g_discovery_wifi.broadcast_address, 0, sizeof(g_discovery_wifi.broadcast_address));
    memset(&g_discovery_wifi.ap_address, 0, sizeof(g_discovery_wifi.ap_address));
    memset(g_discovery_wifi.sta_addresses, 0, sizeof(g_discovery_wifi.sta_addresses));
    memset(&g_discovery_wifi.bound_ipv4, 0, sizeof(g_discovery_wifi.bound_ipv4));

    g_discovery_wifi.next_report_us     = 0U;
    g_discovery_wifi.next_broadcast_us  = 0U;
    g_discovery_wifi.ap_session_id      = 0U;
    g_discovery_wifi.ap_revision        = 0U;
    g_discovery_wifi.role               = LINKG_DEVICE_ROLE_UNKNOWN;
    g_discovery_wifi.bound_ifindex      = 0U;
    g_discovery_wifi.socket_fd          = -1;
    g_discovery_wifi.run_error          = 0;
    g_discovery_wifi.ap_address_valid   = false;
    g_discovery_wifi.channel_registered = false;
    g_discovery_wifi.running            = false;
}

/****************************** 地址辅助 ******************************/

/**
 * @brief 构造Wi-Fi Discovery子网广播地址。
 */
static int _linkg_discovery_wifi_build_broadcast_address(struct sockaddr_in *address)
{
    struct in_addr network;
    struct in_addr netmask;
    uint32_t       broadcast;

    if (address == NULL)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));
    memset(&network, 0, sizeof(network));
    memset(&netmask, 0, sizeof(netmask));

    if (!linkg_network_ipv4_from_string(LINKG_RESOURCE_WIFI_IPV4_NETWORK, &network))
    {
        return -EINVAL;
    }

    if (!linkg_network_ipv4_netmask_from_prefix(LINKG_RESOURCE_WIFI_IPV4_PREFIX, &netmask))
    {
        return -EINVAL;
    }

    broadcast = (ntohl(network.s_addr) & ntohl(netmask.s_addr)) | ~ntohl(netmask.s_addr);

    address->sin_family      = AF_INET;
    address->sin_addr.s_addr = htonl(broadcast);
    address->sin_port        = htons(LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY);

    return 0;
}

/**
 * @brief 校验本机Discovery状态是否具有有效节点角色。
 *
 * Wi-Fi Endpoint允许在Channel启动后异步出现。
 */
static int _linkg_discovery_wifi_validate_local_report(const linkg_discovery_report_t *report)
{
    if (report == NULL)
    {
        return -EINVAL;
    }

    if (report->node.role != LINKG_DEVICE_ROLE_AP &&
        report->node.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验Wi-Fi Discovery UDP来源地址。
 */
static bool _linkg_discovery_wifi_source_valid(const struct sockaddr_in *source)
{
    if (source == NULL)
    {
        return false;
    }

    if (source->sin_family != AF_INET)
    {
        return false;
    }

    if (!linkg_network_ipv4_address_valid(&source->sin_addr))
    {
        return false;
    }

    return source->sin_port == htons(LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY);
}

/**
 * @brief 校验Wi-Fi Discovery控制报文来源与可选Wi-Fi数据Endpoint的一致性。
 *
 * Discovery控制面不依赖Wi-Fi数据Path。未声明Wi-Fi数据Endpoint时，
 * 只要求Endpoint为空；声明了Wi-Fi数据Path时继续校验其IPv4与实际UDP来源一致。
 */
static bool _linkg_discovery_wifi_report_source_valid(const linkg_discovery_report_t *report, const struct sockaddr_in *source)
{
    const struct sockaddr_in *endpoint;

    if (report == NULL || source == NULL)
    {
        return false;
    }

    if ((report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) == 0U)
    {
        return report->wifi_endpoint.length == 0U;
    }

    if (report->wifi_endpoint.length != sizeof(struct sockaddr_in) ||
        report->wifi_endpoint.address.ss_family != AF_INET)
    {
        return false;
    }

    endpoint = (const struct sockaddr_in *)&report->wifi_endpoint.address;

    return endpoint->sin_addr.s_addr == source->sin_addr.s_addr;
}

/****************************** Socket辅助 ******************************/

/**
 * @brief 创建Wi-Fi Discovery UDP套接字。
 *
 * 套接字绑定wlan0但监听INADDR_ANY，使其同时接收本机单播和子网广播。
 */
static int _linkg_discovery_wifi_open_socket(void)
{
    struct sockaddr_in local;
    int                socket_fd;
    int                enable;
	int                tos;
    int                ret;

    socket_fd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
    if (socket_fd < 0)
    {
        return -errno;
    }

    enable = 1;

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, LINKG_RESOURCE_INTERFACE_WIFI, strlen(LINKG_RESOURCE_INTERFACE_WIFI));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    tos = LINKG_DISCOVERY_WIFI_IP_TOS;

    ret = setsockopt(socket_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    memset(&local, 0, sizeof(local));

    local.sin_family      = AF_INET;
    local.sin_addr.s_addr = htonl(INADDR_ANY);
    local.sin_port        = htons(LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY);

    ret = bind(socket_fd, (const struct sockaddr *)&local, sizeof(local));
    if (ret != 0)
    {
        ret = -errno;
        goto fail;
    }

    return socket_fd;

fail:
    close(socket_fd);

    return ret;
}

/****************************** 网络就绪 ******************************/

/**
 * @brief 读取Wi-Fi控制接口当前状态。
 *
 * @return 1表示接口UP且IPv4有效；0表示当前未就绪；负值表示状态读取失败。
 */
static int _linkg_discovery_wifi_read_interface(struct in_addr *address, unsigned int *ifindex)
{
    bool interface_up;
    int ret;

    if (address == NULL || ifindex == NULL)
    {
        return -EINVAL;
    }

    memset(address, 0, sizeof(*address));
    *ifindex = if_nametoindex(LINKG_RESOURCE_INTERFACE_WIFI);
    if (*ifindex == 0U)
    {
        return 0;
    }

    ret = linkg_network_interface_is_up(LINKG_RESOURCE_INTERFACE_WIFI, &interface_up);
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

    ret = linkg_network_interface_get_ipv4(LINKG_RESOURCE_INTERFACE_WIFI, address);
    if (ret == -ENODEV || ret == -ENXIO || ret == -EADDRNOTAVAIL)
    {
        return 0;
    }
    if (ret != 0)
    {
        return ret;
    }

    return linkg_network_ipv4_address_valid(address) ? 1 : 0;
}

/****************************** 已知Peer控制地址 ******************************/

/** @brief 清除某个STA控制地址。调用方持有Wi-Fi状态锁。 */
static void _linkg_discovery_wifi_forget_sta_locked(uint8_t node_id, uint64_t session_id, uint64_t revision)
{
    uint32_t index;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        linkg_discovery_wifi_sta_address_t *entry = &g_discovery_wifi.sta_addresses[index];

        if (entry->valid && entry->node_id == node_id && entry->session_id == session_id && revision >= entry->revision)
        {
            memset(entry, 0, sizeof(*entry));
            return;
        }
    }
}

/**
 * @brief 在Core接受STA完整状态后更新其Discovery控制地址。
 *
 * 本地版本检查用于避免同Session迟到旧报文覆盖最新地址；运行期超时与Core存活超时一致。
 */
static void _linkg_discovery_wifi_learn_sta(const linkg_discovery_report_t *report, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_wifi_sta_address_t *slot;
    uint32_t index;

    if (report == NULL || source == NULL)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    slot = NULL;
    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        linkg_discovery_wifi_sta_address_t *entry = &g_discovery_wifi.sta_addresses[index];

        if (entry->valid && entry->node_id == report->node.node_id)
        {
            if (entry->session_id == report->session_id && report->revision < entry->revision)
            {
                pthread_mutex_unlock(&g_discovery_wifi.lock);
                return;
            }

            slot = entry;
            break;
        }
        if (!entry->valid && slot == NULL)
        {
            slot = entry;
        }
    }

    if (slot != NULL)
    {
        slot->address      = *source;
        slot->session_id   = report->session_id;
        slot->revision     = report->revision;
        slot->last_seen_us = now_us;
        slot->node_id      = report->node.node_id;
        slot->valid        = true;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);
}

/** @brief 将过期的STA控制地址剔除，避免向Core已注销Peer长期单播。 */
static void _linkg_discovery_wifi_age_sta_addresses(uint64_t now_us)
{
    uint32_t index;

    pthread_mutex_lock(&g_discovery_wifi.lock);

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        linkg_discovery_wifi_sta_address_t *entry = &g_discovery_wifi.sta_addresses[index];

        if (entry->valid && now_us >= entry->last_seen_us && now_us - entry->last_seen_us >= LINKG_DISCOVERY_LIVENESS_TIMEOUT_US)
        {
            memset(entry, 0, sizeof(*entry));
        }
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);
}

/****************************** Socket发送 ******************************/

/** @brief 通过Wi-Fi控制Socket发送广播或单播；同Socket的LEAVE发送受锁保护。 */
static int _linkg_discovery_wifi_send_to(const struct sockaddr_in *destination, const uint8_t *buffer, uint32_t length)
{
    ssize_t sent;
    int error;

    if (destination == NULL || buffer == NULL || length == 0U)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.running || g_discovery_wifi.socket_fd < 0)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -ENETDOWN;
    }

    do
    {
        sent = sendto(g_discovery_wifi.socket_fd, buffer, length, MSG_DONTWAIT, (const struct sockaddr *)destination, sizeof(*destination));
    }
    while (sent < 0 && errno == EINTR);

    error = sent < 0 ? -errno : sent == (ssize_t)length ? 0 : -EIO;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return error;
}

/****************************** Wire发送 ******************************/

/**
 * @brief AP一次生成完整Sync：周期单播已知STA，低频广播寻找新STA。
 *
 * 每个目标使用同一份本轮完整状态，广播和单播不需要不同Socket。
 */
static int _linkg_discovery_wifi_send_ap_sync(uint64_t now_us, bool broadcast_due)
{
    linkg_discovery_wifi_sta_address_t targets[LINKG_NODE_PEER_MAX];
    uint8_t                            buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    linkg_discovery_ap_sync_t          sync;
    struct sockaddr_in                 broadcast;
    uint32_t                           length;
    uint32_t                           count;
    uint32_t                           index;
    int                                first_error;
    int                                ret;

    memset(&sync, 0, sizeof(sync));
    memset(buffer, 0, sizeof(buffer));
    memset(targets, 0, sizeof(targets));

    ret = linkg_discovery_channel_build_ap_sync(&sync);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_wire_encode_ap_sync(&sync, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_discovery_wifi_age_sta_addresses(now_us);

    count = 0U;
    pthread_mutex_lock(&g_discovery_wifi.lock);
    broadcast = g_discovery_wifi.broadcast_address;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        if (g_discovery_wifi.sta_addresses[index].valid)
        {
            targets[count++] = g_discovery_wifi.sta_addresses[index];
        }
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    first_error = 0;
    for (index = 0U; index < count; index++)
    {
        ret = _linkg_discovery_wifi_send_to(&targets[index].address, buffer, length);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (broadcast_due)
    {
        ret = _linkg_discovery_wifi_send_to(&broadcast, buffer, length);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/** @brief STA向已知AP单播当前完整Report。 */
static int _linkg_discovery_wifi_send_sta_report(void)
{
    uint8_t                  buffer[LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE];
    linkg_discovery_report_t report;
    struct sockaddr_in       destination;
    uint32_t                 length;
    bool                     valid;
    int                      ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    valid       = g_discovery_wifi.ap_address_valid;
    destination = g_discovery_wifi.ap_address;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (!valid)
    {
        return 0;
    }

    memset(&report, 0, sizeof(report));
    memset(buffer, 0, sizeof(buffer));

    ret = linkg_discovery_channel_get_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_discovery_wire_encode_sta_report(&report, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_discovery_wifi_send_to(&destination, buffer, length);
}

/**
 * @brief 发送本机主动LEAVE；必须在所有Worker已经quiesce时调用。
 *
 * AP向已知STA单播，同时保留一次广播尽力通知未知接收者。
 */
static int _linkg_discovery_wifi_send_leave(const linkg_discovery_leave_t *leave, void *user_data)
{
    linkg_discovery_wifi_context_t *context;
    uint8_t                         buffer[LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE];
    struct sockaddr_in              targets[LINKG_NODE_PEER_MAX + 1U];
    uint32_t                        length;
    uint32_t                        count;
    uint32_t                        index;
    int                             first_error;
    int                             ret;

    if (leave == NULL || user_data == NULL)
    {
        return -EINVAL;
    }

    context = user_data;
    memset(buffer, 0, sizeof(buffer));

    ret = linkg_discovery_wire_encode_peer_leave(leave, buffer, sizeof(buffer), &length);
    if (ret != 0)
    {
        return ret;
    }

    count = 0U;
    pthread_mutex_lock(&context->lock);

    if (!context->running || context->socket_fd < 0)
    {
        pthread_mutex_unlock(&context->lock);
        return -ENETDOWN;
    }

    if (context->role == LINKG_DEVICE_ROLE_AP)
    {
        for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
        {
            if (context->sta_addresses[index].valid)
            {
                targets[count++] = context->sta_addresses[index].address;
            }
        }
        targets[count++] = context->broadcast_address;
    }
    else if (context->role == LINKG_DEVICE_ROLE_STA && context->ap_address_valid)
    {
        targets[count++] = context->ap_address;
    }

    pthread_mutex_unlock(&context->lock);

    if (count == 0U)
    {
        return -ENOTCONN;
    }

    first_error = 0;
    for (index = 0U; index < count; index++)
    {
        ret = _linkg_discovery_wifi_send_to(&targets[index], buffer, length);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** Wire接收 ******************************/

/** @brief AP接收有效STA状态后学习其Wi-Fi控制地址。 */
static void _linkg_discovery_wifi_handle_sta_report(const uint8_t *buffer, uint32_t length, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_report_t report;
    int ret;

    memset(&report, 0, sizeof(report));

    ret = linkg_discovery_wire_decode_sta_report(buffer, length, &report);
    if (ret != 0 || !_linkg_discovery_wifi_report_source_valid(&report, source))
    {
        return;
    }

    ret = linkg_discovery_channel_handle_peer_report(LINKG_LINK_ACCESS_WIFI, &report, now_us);
    if (ret == LINKG_DISCOVERY_REPORT_IGNORED)
    {
        return;
    }

    if (ret < 0)
    {
        LINKG_LOG_WARN("DISCOVERY-WIFI: STA report rejected, node=%u session=%llu revision=%llu error=%d",
                    (unsigned int)report.node.node_id,
                    (unsigned long long)report.session_id,
                    (unsigned long long)report.revision, ret);
        return;
    }

    _linkg_discovery_wifi_learn_sta(&report, source, now_us);
}


/** @brief STA接收AP Sync，Core接受后更新AP的Wi-Fi控制地址。 */
static void _linkg_discovery_wifi_handle_ap_sync(const uint8_t *buffer, uint32_t length, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_ap_sync_t sync;
    bool address_changed;
    int ret;

    memset(&sync, 0, sizeof(sync));

    ret = linkg_discovery_wire_decode_ap_sync(buffer, length, &sync);
    if (ret != 0 || !_linkg_discovery_wifi_report_source_valid(&sync.ap, source))
    {
        return;
    }

    ret = linkg_discovery_channel_handle_ap_sync(LINKG_LINK_ACCESS_WIFI, &sync, now_us);
    if (ret == LINKG_DISCOVERY_REPORT_IGNORED)
    {
        return;
    }

    if (ret < 0)
    {
        LINKG_LOG_WARN("DISCOVERY-WIFI: AP sync rejected, node=%u session=%llu revision=%llu error=%d",
                    (unsigned int)sync.ap.node.node_id,
                    (unsigned long long)sync.ap.session_id,
                    (unsigned long long)sync.ap.revision, ret);
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (g_discovery_wifi.ap_address_valid && g_discovery_wifi.ap_session_id == sync.ap.session_id && sync.ap.revision < g_discovery_wifi.ap_revision)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return;
    }

    address_changed = !g_discovery_wifi.ap_address_valid ||
                      g_discovery_wifi.ap_address.sin_addr.s_addr != source->sin_addr.s_addr ||
                      g_discovery_wifi.ap_address.sin_port != source->sin_port;

    g_discovery_wifi.ap_address       = *source;
    g_discovery_wifi.ap_session_id    = sync.ap.session_id;
    g_discovery_wifi.ap_revision      = sync.ap.revision;
    g_discovery_wifi.ap_address_valid = true;

    if (address_changed && g_discovery_wifi.next_report_us > now_us)
    {
        g_discovery_wifi.next_report_us = now_us;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);
}

/** @brief 接收主动离开，清除与该Session匹配的控制地址。 */
static void _linkg_discovery_wifi_handle_peer_leave(const uint8_t *buffer, uint32_t length, uint64_t now_us)
{
    linkg_discovery_leave_t leave;
    int ret;

    memset(&leave, 0, sizeof(leave));

    ret = linkg_discovery_wire_decode_peer_leave(buffer, length, &leave);
    if (ret != 0)
    {
        return;
    }

    ret = linkg_discovery_channel_handle_peer_leave(&leave, now_us);
    if (ret != 0)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (g_discovery_wifi.role == LINKG_DEVICE_ROLE_AP)
    {
        _linkg_discovery_wifi_forget_sta_locked(leave.node_id, leave.session_id, leave.revision);
    }
    else if (g_discovery_wifi.role == LINKG_DEVICE_ROLE_STA && g_discovery_wifi.ap_address_valid &&
             g_discovery_wifi.ap_session_id == leave.session_id && leave.revision >= g_discovery_wifi.ap_revision)
    {
        memset(&g_discovery_wifi.ap_address, 0, sizeof(g_discovery_wifi.ap_address));
        g_discovery_wifi.ap_address_valid = false;
        g_discovery_wifi.ap_session_id    = 0U;
        g_discovery_wifi.ap_revision      = 0U;
    }

    pthread_mutex_unlock(&g_discovery_wifi.lock);
}

/** @brief 按本机角色分发Wi-Fi控制报文。 */
static void _linkg_discovery_wifi_handle_packet(const uint8_t *buffer, uint32_t length, const struct sockaddr_in *source, uint64_t now_us)
{
    linkg_discovery_wire_header_t header;
    linkg_device_role_t role;
    int ret;

    if (buffer == NULL || source == NULL || length < LINKG_DISCOVERY_WIRE_HEADER_SIZE || !_linkg_discovery_wifi_source_valid(source))
    {
        return;
    }

    memset(&header, 0, sizeof(header));
    ret = linkg_discovery_wire_decode_header(buffer, length, &header);
    if (ret != 0 || header.magic != LINKG_DISCOVERY_WIRE_MAGIC || header.version != LINKG_DISCOVERY_WIRE_VERSION)
    {
        return;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    role = g_discovery_wifi.role;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (header.type == LINKG_DISCOVERY_MESSAGE_PEER_LEAVE)
    {
        _linkg_discovery_wifi_handle_peer_leave(buffer, length, now_us);
    }
    else if (role == LINKG_DEVICE_ROLE_AP && header.type == LINKG_DISCOVERY_MESSAGE_STA_REPORT)
    {
        _linkg_discovery_wifi_handle_sta_report(buffer, length, source, now_us);
    }
    else if (role == LINKG_DEVICE_ROLE_STA && header.type == LINKG_DISCOVERY_MESSAGE_AP_SYNC)
    {
        _linkg_discovery_wifi_handle_ap_sync(buffer, length, source, now_us);
    }
}

/** @brief 单轮有界读取，避免满负载下读Socket无限循环影响周期发送和Stop。 */
static int _linkg_discovery_wifi_receive(linkg_thread_t *thread)
{
    uint8_t            buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    struct sockaddr_in source;
    struct iovec       iovec;
    struct msghdr      message;
    uint32_t           index;
    ssize_t            received;
    int                socket_fd;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    socket_fd = g_discovery_wifi.socket_fd;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    for (index = 0U; index < LINKG_DISCOVERY_WIFI_RX_BUDGET && linkg_thread_is_running(thread); index++)
    {
        memset(buffer, 0, sizeof(buffer));
        memset(&source, 0, sizeof(source));
        memset(&iovec, 0, sizeof(iovec));
        memset(&message, 0, sizeof(message));

        iovec.iov_base      = buffer;
        iovec.iov_len       = sizeof(buffer);
        message.msg_name   = &source;
        message.msg_namelen = sizeof(source);
        message.msg_iov    = &iovec;
        message.msg_iovlen = 1U;

        do
        {
            received = recvmsg(socket_fd, &message, MSG_DONTWAIT | MSG_TRUNC);
        }
        while (received < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (received < 0)
        {
            if (errno == EAGAIN || errno == EWOULDBLOCK || !linkg_thread_is_running(thread))
            {
                return 0;
            }
            return -errno;
        }

        if ((message.msg_flags & MSG_TRUNC) != 0 || received > (ssize_t)sizeof(buffer) ||
            received < (ssize_t)LINKG_DISCOVERY_WIRE_HEADER_SIZE || message.msg_namelen != sizeof(source))
        {
            continue;
        }

        _linkg_discovery_wifi_handle_packet(buffer, (uint32_t)received, &source, linkg_time_monotonic_us());
    }

    return 0;
}

/****************************** 周期任务 ******************************/

/** @brief 返回本轮通信和接口检查任务的下一次等待毫秒数。 */
static int _linkg_discovery_wifi_poll_timeout(uint64_t now_us)
{
    uint64_t next_us;
    uint64_t delay_ms;

    next_us = g_discovery_wifi.next_report_us;
    if (g_discovery_wifi.role == LINKG_DEVICE_ROLE_AP && g_discovery_wifi.next_broadcast_us < next_us)
    {
        next_us = g_discovery_wifi.next_broadcast_us;
    }
    if (next_us <= now_us)
    {
        return 0;
    }

    delay_ms = (next_us - now_us + 999ULL) / 1000ULL;
    if (delay_ms > LINKG_DISCOVERY_WIFI_MONITOR_INTERVAL_MS)
    {
        delay_ms = LINKG_DISCOVERY_WIFI_MONITOR_INTERVAL_MS;
    }

    return (int)delay_ms;
}

/** @brief 周期发送；AP广播附带的单播同步刷新下一次心跳时间。 */
static void _linkg_discovery_wifi_process_periodic(uint64_t now_us)
{
    bool report_due;
    bool broadcast_due;
    int ret;

    report_due    = now_us >= g_discovery_wifi.next_report_us;
    broadcast_due = g_discovery_wifi.role == LINKG_DEVICE_ROLE_AP && now_us >= g_discovery_wifi.next_broadcast_us;

    if (!report_due && !broadcast_due)
    {
        return;
    }

    ret = 0;

    if (g_discovery_wifi.role == LINKG_DEVICE_ROLE_AP)
    {
        ret = _linkg_discovery_wifi_send_ap_sync(now_us, broadcast_due);
    }
    else if (report_due && g_discovery_wifi.role == LINKG_DEVICE_ROLE_STA)
    {
        ret = _linkg_discovery_wifi_send_sta_report();
    }

    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-WIFI: periodic report send failed, role=%d error=%d", g_discovery_wifi.role, ret);
    }

    if (report_due)
    {
        ret = linkg_discovery_channel_age_peers(now_us);
        if (ret != 0)
        {
            LINKG_LOG_WARN("DISCOVERY-WIFI: peer aging failed, error=%d", ret);
        }
    }

    /* AP本轮无论由单播周期还是广播周期触发，都已经执行过单播。 */
    if (report_due || broadcast_due)
    {
        g_discovery_wifi.next_report_us = now_us + LINKG_DISCOVERY_WIFI_REPORT_INTERVAL_US;
    }

    if (broadcast_due)
    {
        g_discovery_wifi.next_broadcast_us = now_us + LINKG_DISCOVERY_WIFI_BROADCAST_INTERVAL_US;
    }
}

/****************************** Channel运行资源 ******************************/

/** @brief Wi-Fi接口就绪后创建Socket并注册Core Channel。 */
static int _linkg_discovery_wifi_activate(const struct in_addr *ipv4, unsigned int ifindex)
{
    linkg_discovery_report_t report;
    struct sockaddr_in       broadcast;
    uint64_t                 now_us;
    int                      socket_fd;
    int                      ret;

    memset(&report, 0, sizeof(report));
    memset(&broadcast, 0, sizeof(broadcast));

    ret = linkg_discovery_channel_get_local_report(&report);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_wifi_validate_local_report(&report);
    if (ret != 0)
    {
        return -EPROTO;
    }

    ret = _linkg_discovery_wifi_build_broadcast_address(&broadcast);
    if (ret != 0)
    {
        return -EPROTO;
    }

    socket_fd = _linkg_discovery_wifi_open_socket();
    if (socket_fd < 0)
    {
        return socket_fd;
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_discovery_wifi.lock);

    g_discovery_wifi.socket_fd         = socket_fd;
    g_discovery_wifi.broadcast_address = broadcast;
    g_discovery_wifi.role              = report.node.role;
    g_discovery_wifi.bound_ipv4        = *ipv4;
    g_discovery_wifi.bound_ifindex     = ifindex;
    g_discovery_wifi.next_report_us    = now_us;
    g_discovery_wifi.next_broadcast_us = now_us;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_discovery_channel_register(LINKG_LINK_ACCESS_WIFI, _linkg_discovery_wifi_send_leave, &g_discovery_wifi);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_discovery_wifi.lock);
        g_discovery_wifi.socket_fd = -1;
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        (void)close(socket_fd);
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    g_discovery_wifi.channel_registered = true;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    LINKG_LOG_INFO("DISCOVERY-WIFI: channel ready, interface=%s ifindex=%u", LINKG_RESOURCE_INTERFACE_WIFI, ifindex);
    return 0;
}

/**
 * @brief 网络资源实际失效时撤销Channel并关闭Socket。
 *
 * 正常quiesce不得调用本函数，否则Core将无法使用原Socket发送PEER_LEAVE。
 */
static int _linkg_discovery_wifi_deactivate(void)
{
    uint64_t now_us;
    int first_error;
    int ret;
    bool registered;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    registered = g_discovery_wifi.channel_registered;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    first_error = 0;
    now_us = linkg_time_monotonic_us();

    if (registered)
    {
        ret = linkg_discovery_channel_unregister(LINKG_LINK_ACCESS_WIFI, now_us);
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);

    g_discovery_wifi.channel_registered = false;

    if (g_discovery_wifi.socket_fd >= 0 && close(g_discovery_wifi.socket_fd) != 0 && first_error == 0)
    {
        first_error = -errno;
    }

    g_discovery_wifi.socket_fd = -1;
    memset(&g_discovery_wifi.bound_ipv4, 0, sizeof(g_discovery_wifi.bound_ipv4));
    memset(&g_discovery_wifi.ap_address, 0, sizeof(g_discovery_wifi.ap_address));
    memset(g_discovery_wifi.sta_addresses, 0, sizeof(g_discovery_wifi.sta_addresses));
    g_discovery_wifi.bound_ifindex      = 0U;
    g_discovery_wifi.ap_address_valid   = false;
    g_discovery_wifi.ap_session_id      = 0U;
    g_discovery_wifi.ap_revision        = 0U;
    g_discovery_wifi.next_report_us     = 0U;
    g_discovery_wifi.next_broadcast_us  = 0U;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return first_error;
}

/****************************** 工作线程 ******************************/

/** @brief 可中断等待：停止通知和Network状态变化共用wakeup FD。 */
static int _linkg_discovery_wifi_wait(linkg_thread_t *thread, int socket_fd, int timeout_ms, bool *socket_ready)
{
    struct pollfd descriptors[2];
    nfds_t count;
    int ret;

    if (thread == NULL || socket_ready == NULL)
    {
        return -EINVAL;
    }

    *socket_ready = false;
    memset(descriptors, 0, sizeof(descriptors));
    descriptors[0].fd     = linkg_thread_get_wakeup_fd(thread);
    descriptors[0].events = POLLIN;
    if (descriptors[0].fd < 0)
    {
        return -ENODEV;
    }

    count = 1U;
    if (socket_fd >= 0)
    {
        descriptors[1].fd = socket_fd;
        descriptors[1].events = POLLIN;
        count = 2U;
    }

    do
    {
        ret = poll(descriptors, count, timeout_ms);
    }
    while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

    if (!linkg_thread_is_running(thread))
    {
        return 0;
    }
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
        ret = linkg_thread_clear_wakeup(thread);
        if (ret != 0)
        {
            return ret;
        }
    }

    if (count == 2U)
    {
        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -ENETDOWN;
        }
        *socket_ready = (descriptors[1].revents & POLLIN) != 0;
    }

    return 0;
}

/**
 * @brief Wi-Fi Discovery运行主循环。
 *
 * Worker始终存在；Socket和Core Channel随wlan0 UP/IPv4/ifindex变化建立或撤销。
 */
static int _linkg_discovery_wifi_run(linkg_thread_t *thread)
{
    struct in_addr ipv4;
    unsigned int ifindex;
    bool socket_ready;
    bool active;
    int last_retry_error;
    int socket_fd;
    int timeout_ms;
    int ready;
    int ret;

    if (thread == NULL)
    {
        return -EINVAL;
    }

    active = false;
    last_retry_error = 0;

    while (linkg_thread_is_running(thread))
    {
        ready = _linkg_discovery_wifi_read_interface(&ipv4, &ifindex);
        if (ready < 0)
        {
            if (ready != last_retry_error)
            {
                LINKG_LOG_WARN("DISCOVERY-WIFI: read interface failed, error=%d", ready);
                last_retry_error = ready;
            }
        }


        if (active && (ready == 0 || (ready > 0 && (ifindex != g_discovery_wifi.bound_ifindex || ipv4.s_addr != g_discovery_wifi.bound_ipv4.s_addr))))
        {
            if (!linkg_thread_is_running(thread))
            {
                break;
            }

            LINKG_LOG_WARN("DISCOVERY-WIFI: interface changed or unavailable, rebuilding channel");
            ret = _linkg_discovery_wifi_deactivate();
            if (ret != 0)
            {
                LINKG_LOG_WARN("DISCOVERY-WIFI: deactivate failed, error=%d", ret);
            }
            active = false;
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if (!active && ready > 0)
        {
            ret = _linkg_discovery_wifi_activate(&ipv4, ifindex);
            if (ret == 0)
            {
                active = true;
                last_retry_error = 0;
            }
            else
            {
                if (ret == -EPROTO)
                {
                    return ret;
                }
                if (ret != last_retry_error)
                {
                    LINKG_LOG_WARN("DISCOVERY-WIFI: activate pending, error=%d", ret);
                    last_retry_error = ret;
                }
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if (active)
        {
            _linkg_discovery_wifi_process_periodic(linkg_time_monotonic_us());
        }

        timeout_ms = active ? _linkg_discovery_wifi_poll_timeout(linkg_time_monotonic_us()) : LINKG_DISCOVERY_WIFI_MONITOR_INTERVAL_MS;
        socket_fd = active ? g_discovery_wifi.socket_fd : -1;

        ret = _linkg_discovery_wifi_wait(thread, socket_fd, timeout_ms, &socket_ready);
        if (!linkg_thread_is_running(thread))
        {
            break;
        }
        if (ret != 0)
        {
            if (active && ret == -ENETDOWN)
            {
                LINKG_LOG_WARN("DISCOVERY-WIFI: socket unavailable, rebuilding channel");
                (void)_linkg_discovery_wifi_deactivate();
                active = false;
                continue;
            }
            return ret;
        }

        if (active && socket_ready)
        {
            ret = _linkg_discovery_wifi_receive(thread);
            if (ret != 0)
            {
                LINKG_LOG_WARN("DISCOVERY-WIFI: receive failed, rebuilding channel, error=%d", ret);
                (void)_linkg_discovery_wifi_deactivate();
                active = false;
            }
        }
    }

    // 正常quiesce只停止Worker，不关闭Socket，Core随后通过本Channel发送LEAVE。
    return 0;
}

/** @brief Wi-Fi Discovery Worker入口。 */
static void _linkg_discovery_wifi_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_discovery_wifi_context_t *context;
    int ret;
    int cleanup_ret;

    context = user_data;
    if (thread == NULL || context == NULL)
    {
        return;
    }

    ret = _linkg_discovery_wifi_run(thread);
    if (!linkg_thread_is_running(thread))
    {
        return;
    }

    // 仅不可恢复的Worker退出执行异常清理；正常quiesce保留Socket与注册状态。
    LINKG_LOG_ERROR("DISCOVERY-WIFI: worker exited unexpectedly, error=%d", ret);

    cleanup_ret = _linkg_discovery_wifi_deactivate();
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("DISCOVERY-WIFI: worker cleanup failed, error=%d", cleanup_ret);
    }

    pthread_mutex_lock(&context->lock);
    context->running   = false;
    context->run_error = ret != 0 ? ret : cleanup_ret != 0 ? cleanup_ret : -EIO;
    pthread_mutex_unlock(&context->lock);
}

/****************************** 生命周期 ******************************/

/** @brief 只初始化Worker对象和模块上下文，不创建Socket或访问wlan0。 */
int linkg_discovery_wifi_init(void)
{
    int ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    if (g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -EALREADY;
    }
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_thread_init(&g_discovery_wifi.thread, LINKG_DISCOVERY_WIFI_THREAD_NAME, _linkg_discovery_wifi_thread, &g_discovery_wifi);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    _linkg_discovery_wifi_reset_runtime_locked();
    g_discovery_wifi.initialized = true;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return 0;
}

/** @brief 启动Worker，不查询Core Report、不等待wlan0、不创建Socket。 */
int linkg_discovery_wifi_start(void)
{
    int ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);

    if (!g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -ENODEV;
    }
    if (g_discovery_wifi.running || linkg_thread_is_started(&g_discovery_wifi.thread))
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -EALREADY;
    }
    if (g_discovery_wifi.socket_fd >= 0 || g_discovery_wifi.channel_registered)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return -EBUSY;
    }

    _linkg_discovery_wifi_reset_runtime_locked();
    g_discovery_wifi.running = true;

    pthread_mutex_unlock(&g_discovery_wifi.lock);

    ret = linkg_thread_start(&g_discovery_wifi.thread);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_discovery_wifi.lock);
        g_discovery_wifi.running = false;
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return ret;
    }

    return 0;
}

/** @brief 通知网络状态已变化；未启动时Worker日后会自行检查，不缓存事件。 */
int linkg_discovery_wifi_notify_network_changed(void)
{
    bool wakeup;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    wakeup = g_discovery_wifi.initialized && g_discovery_wifi.running &&
             linkg_thread_is_started(&g_discovery_wifi.thread) && linkg_thread_is_running(&g_discovery_wifi.thread);
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return wakeup ? linkg_thread_wakeup(&g_discovery_wifi.thread) : 0;
}

/** @brief 停止并join Worker，保留已建立的Socket和Core注册以发送LEAVE。 */
int linkg_discovery_wifi_quiesce(void)
{
    bool initialized;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    initialized = g_discovery_wifi.initialized;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return initialized ? linkg_thread_stop(&g_discovery_wifi.thread) : 0;
}

/** @brief 冻结Worker、注销Channel、关闭Socket并清除本轮运行状态。 */
int linkg_discovery_wifi_stop(void)
{
    bool initialized;
    int first_error;
    int run_error;
    int ret;

    pthread_mutex_lock(&g_discovery_wifi.lock);
    initialized = g_discovery_wifi.initialized;
    pthread_mutex_unlock(&g_discovery_wifi.lock);
    if (!initialized)
    {
        return 0;
    }

    ret = linkg_discovery_wifi_quiesce();
    if (ret != 0)
    {
        return ret;
    }

    first_error = _linkg_discovery_wifi_deactivate();

    pthread_mutex_lock(&g_discovery_wifi.lock);
    run_error = g_discovery_wifi.run_error;
    _linkg_discovery_wifi_reset_runtime_locked();
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    if (run_error != 0)
    {
        LINKG_LOG_WARN("DISCOVERY-WIFI: previous Worker failure, error=%d", run_error);
    }

    return first_error;
}

/** @brief 释放Worker对象；调用方负责确保Core本轮Session已停止。 */
int linkg_discovery_wifi_deinit(void)
{
    int ret;

    ret = linkg_discovery_wifi_stop();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery_wifi.lock);
    if (!g_discovery_wifi.initialized)
    {
        pthread_mutex_unlock(&g_discovery_wifi.lock);
        return 0;
    }
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    linkg_thread_deinit(&g_discovery_wifi.thread);

    pthread_mutex_lock(&g_discovery_wifi.lock);
    _linkg_discovery_wifi_reset_runtime_locked();
    g_discovery_wifi.initialized = false;
    pthread_mutex_unlock(&g_discovery_wifi.lock);

    return 0;
}
