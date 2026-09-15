/**
 * @file cellular_link_heartbeat.c
 * @brief LinkG蜂窝业务链路心跳实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-31
 */

#define _GNU_SOURCE

#include "cellular_link_heartbeat.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_node.h"
#include "linkg_path.h"
#include "linkg_switch.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_LINK_HEARTBEAT_THREAD_NAME             "cell-link-hb" // 心跳工作线程名称
#define LINKG_CELLULAR_LINK_HEARTBEAT_POLL_INTERVAL_MS        100U           // 心跳策略检查周期，单位毫秒
#define LINKG_CELLULAR_LINK_HEARTBEAT_FAST_INTERVAL_US        100000ULL      // 主用5G低流量心跳周期，单位微秒
#define LINKG_CELLULAR_LINK_HEARTBEAT_SLOW_INTERVAL_US        5000000ULL     // 备用或高流量心跳周期，单位微秒
#define LINKG_CELLULAR_LINK_HEARTBEAT_PPS_SAMPLE_INTERVAL_US  1000000ULL     // 真实业务PPS统计窗口，单位微秒
#define LINKG_CELLULAR_LINK_HEARTBEAT_HIGH_PPS_THRESHOLD      100U           // 高频真实业务PPS阈值
#define LINKG_CELLULAR_LINK_HEARTBEAT_PEER_STATE_COUNT        (LINKG_RESOURCE_NODE_ID_MAX + 1U) // Peer状态槽位数量
#define LINKG_CELLULAR_LINK_HEARTBEAT_MAGIC_SIZE              4U             // 心跳Magic长度
#define LINKG_CELLULAR_LINK_HEARTBEAT_VERSION                 1U             // 心跳协议版本
#define LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_0              0xA5U          // 心跳保留校验字节0
#define LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_1              0x5AU          // 心跳保留校验字节1
#define LINKG_CELLULAR_LINK_HEARTBEAT_TX_CONTROL_SIZE         CMSG_SPACE(sizeof(struct in6_pktinfo)) // IPv6发送辅助控制区大小

/****************************** 内部类型 ******************************/

typedef struct
{
    uint8_t magic[LINKG_CELLULAR_LINK_HEARTBEAT_MAGIC_SIZE]; // 心跳Magic
    uint8_t version;                                         // 心跳协议版本
    uint8_t tx_class;                                        // 当前业务Socket类别
    uint8_t reserved_0;                                      // 固定保留校验字节0
    uint8_t reserved_1;                                      // 固定保留校验字节1
} linkg_cellular_link_heartbeat_wire_t;

typedef struct
{
    uint64_t sample_started_us;     // 当前PPS统计窗口起始时间
    uint64_t last_business_packets; // 上次统计累计真实业务包数
    uint64_t last_heartbeat_us;     // 最近一次心跳尝试时间
    uint32_t current_pps;           // 当前Peer Cellular真实业务PPS
    bool     sample_initialized;    // PPS统计是否已经建立基线
    bool     tracked;               // 当前Peer是否存在活动Cellular Path
} linkg_cellular_link_heartbeat_peer_state_t;

struct linkg_cellular_link_heartbeat
{
    linkg_thread_t                              thread;         // 心跳工作线程
    int                                       *socket_fds;     // 借用Cellular Link三个业务Socket数组
    const uint16_t                            *service_ports;  // 借用Cellular Link三个业务端口数组
    const char                                *interface_name; // 借用蜂窝出口接口名称
    uint32_t                                   link_id;        // 当前Cellular Link运行实例标识
    linkg_cellular_link_heartbeat_peer_state_t peer_states[LINKG_CELLULAR_LINK_HEARTBEAT_PEER_STATE_COUNT]; // Per-Peer心跳状态
};

_Static_assert(sizeof(linkg_cellular_link_heartbeat_wire_t) == 8U, "invalid cellular link heartbeat wire size");

/****************************** 模块常量数据 ******************************/

static const uint8_t g_cellular_link_heartbeat_magic[LINKG_CELLULAR_LINK_HEARTBEAT_MAGIC_SIZE] =
{
    'L',
    'G',
    'H',
    'B'
};

/****************************** 参数校验 ******************************/

/**
 * @brief 校验蜂窝业务链路心跳配置。
 */
static int _linkg_cellular_link_heartbeat_validate_config(const linkg_cellular_link_heartbeat_config_t *config)
{
    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->socket_fds == NULL || config->service_ports == NULL)
    {
        return -EINVAL;
    }

    if (config->interface_name == NULL || config->interface_name[0] == '\0')
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验心跳目标是否为有效Global IPv6 Endpoint。
 */
static int _linkg_cellular_link_heartbeat_validate_destination(const linkg_path_endpoint_t *destination)
{
    const struct sockaddr_in6 *address;

    if (destination == NULL)
    {
        return -EINVAL;
    }

    if (destination->length != sizeof(struct sockaddr_in6))
    {
        return -EDESTADDRREQ;
    }

    if (destination->address.ss_family != AF_INET6)
    {
        return -EAFNOSUPPORT;
    }

    address = (const struct sockaddr_in6 *)&destination->address;

    if (!linkg_network_ipv6_address_is_global(&address->sin6_addr))
    {
        return -EDESTADDRREQ;
    }

    return 0;
}

/****************************** Wire辅助 ******************************/

/**
 * @brief 构造指定业务类别的蜂窝链路心跳报文。
 */
static int _linkg_cellular_link_heartbeat_build_wire(linkg_link_tx_class_t tx_class, linkg_cellular_link_heartbeat_wire_t *wire)
{
    if (wire == NULL)
    {
        return -EINVAL;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    memset(wire, 0, sizeof(*wire));
    memcpy(wire->magic, g_cellular_link_heartbeat_magic, sizeof(wire->magic));

    wire->version    = LINKG_CELLULAR_LINK_HEARTBEAT_VERSION;
    wire->tx_class   = (uint8_t)tx_class;
    wire->reserved_0 = LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_0;
    wire->reserved_1 = LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_1;

    return 0;
}

/****************************** 心跳策略 ******************************/

/**
 * @brief 重置指定Peer心跳运行状态。
 */
static void _linkg_cellular_link_heartbeat_reset_peer_state(linkg_cellular_link_heartbeat_peer_state_t *state, bool tracked)
{
    if (state == NULL)
    {
        return;
    }

    memset(state, 0, sizeof(*state));
    state->tracked = tracked;
}

/**
 * @brief 获取Path累计真实业务包数量。
 */
static uint64_t _linkg_cellular_link_heartbeat_business_packets(const linkg_path_stats_t *stats)
{
    if (stats == NULL)
    {
        return 0U;
    }

    return stats->tx_packets + stats->rx_packets;
}

/**
 * @brief 更新指定Peer当前Cellular真实业务PPS。
 */
static void _linkg_cellular_link_heartbeat_update_pps(linkg_cellular_link_heartbeat_peer_state_t *state, const linkg_path_stats_t *stats, uint64_t now_us)
{
    uint64_t total_packets;
    uint64_t elapsed_us;
    uint64_t delta_packets;
    uint64_t pps;

    if (state == NULL || stats == NULL)
    {
        return;
    }

    total_packets = _linkg_cellular_link_heartbeat_business_packets(stats);

    if (!state->sample_initialized ||
        total_packets < state->last_business_packets ||
        now_us < state->sample_started_us)
    {
        state->sample_started_us     = now_us;
        state->last_business_packets = total_packets;
        state->last_heartbeat_us     = 0U;
        state->current_pps           = 0U;
        state->sample_initialized    = true;

        return;
    }

    elapsed_us = now_us - state->sample_started_us;

    if (elapsed_us < LINKG_CELLULAR_LINK_HEARTBEAT_PPS_SAMPLE_INTERVAL_US)
    {
        return;
    }

    delta_packets = total_packets - state->last_business_packets;
    pps           = (delta_packets * 1000000ULL) / elapsed_us;

    state->current_pps = pps > UINT32_MAX ? UINT32_MAX : (uint32_t)pps;

    state->sample_started_us     = now_us;
    state->last_business_packets = total_packets;
}

/**
 * @brief 判断指定Peer当前Send Plan是否实际使用Cellular Link发送业务。
 */
static int _linkg_cellular_link_heartbeat_plan_uses_cellular(uint8_t peer_node_id, uint32_t cellular_link_id, bool *used)
{
    linkg_send_plan_t plan;
    int               ret;

    if (used == NULL || cellular_link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    *used = false;

    ret = linkg_switch_get_plan(peer_node_id, &plan);
    if (ret == -ENOENT)
    {
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    if (plan.mode == LINKG_SEND_MODE_SINGLE)
    {
        *used = plan.primary_link_id == cellular_link_id;
        return 0;
    }

    if (plan.mode == LINKG_SEND_MODE_REDUNDANT)
    {
        *used = plan.primary_link_id == cellular_link_id ||
                plan.secondary_link_id == cellular_link_id;
    }

    return 0;
}

/**
 * @brief 根据Send Plan和真实业务PPS选择指定Peer心跳周期。
 */
static uint64_t _linkg_cellular_link_heartbeat_select_interval(bool cellular_used, uint32_t pps)
{
    if (cellular_used && pps <= LINKG_CELLULAR_LINK_HEARTBEAT_HIGH_PPS_THRESHOLD)
    {
        return LINKG_CELLULAR_LINK_HEARTBEAT_FAST_INTERVAL_US;
    }

    return LINKG_CELLULAR_LINK_HEARTBEAT_SLOW_INTERVAL_US;
}

/**
 * @brief 判断指定Peer本轮是否到达心跳发送时间。
 */
static bool _linkg_cellular_link_heartbeat_due(const linkg_cellular_link_heartbeat_peer_state_t *state, uint64_t now_us, uint64_t interval_us)
{
    if (state == NULL || interval_us == 0U)
    {
        return false;
    }

    if (state->last_heartbeat_us == 0U || now_us < state->last_heartbeat_us)
    {
        return true;
    }

    return now_us - state->last_heartbeat_us >= interval_us;
}

/**
 * @brief 获取当前已经跟踪的Cellular Peer数量。
 */
static uint32_t _linkg_cellular_link_heartbeat_tracked_count(const linkg_cellular_link_heartbeat_t *heartbeat)
{
    uint32_t node_id;
    uint32_t count;

    if (heartbeat == NULL)
    {
        return 0U;
    }

    count = 0U;

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        if (heartbeat->peer_states[node_id].tracked)
        {
            count++;
        }
    }

    return count;
}

/**
 * @brief 重新扫描当前Cellular Link上的直接Peer并同步本地跟踪状态。
 *
 * @note 仅在Path数量变化或已跟踪Path失效时调用，避免每100ms遍历全部Node ID。
 */
static int _linkg_cellular_link_heartbeat_refresh_tracking(linkg_cellular_link_heartbeat_t *heartbeat)
{
    linkg_cellular_link_heartbeat_peer_state_t *state;
    linkg_path_endpoint_t                       destination;
    linkg_path_t                               *path;
    uint32_t                                    node_id;
    int                                         first_error;
    int                                         ret;

    if (heartbeat == NULL || heartbeat->link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    first_error = 0;

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        state = &heartbeat->peer_states[node_id];
        path  = NULL;

        memset(&destination, 0, sizeof(destination));

        ret = linkg_node_acquire_path((uint8_t)node_id,
                                      heartbeat->link_id,
                                      &path,
                                      &destination);
        if (ret == 0)
        {
            linkg_path_release(path);

            if (!state->tracked)
            {
                _linkg_cellular_link_heartbeat_reset_peer_state(state, true);
            }

            continue;
        }

        if (ret == -ENOENT || ret == -ENODEV)
        {
            if (state->tracked)
            {
                _linkg_cellular_link_heartbeat_reset_peer_state(state, false);
            }

            continue;
        }

        if (first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 接口辅助 ******************************/

/**
 * @brief 获取当前蜂窝出口接口索引。
 */
static int _linkg_cellular_link_heartbeat_interface_index(const linkg_cellular_link_heartbeat_t *heartbeat, unsigned int *interface_index)
{
    if (heartbeat == NULL || interface_index == NULL)
    {
        return -EINVAL;
    }

    errno = 0;
    *interface_index = if_nametoindex(heartbeat->interface_name);

    if (*interface_index == 0U)
    {
        return errno != 0 ? -errno : -ENODEV;
    }

    return 0;
}

/**
 * @brief 为心跳IPv6 UDP消息构造IPV6_PKTINFO控制信息。
 */
static int _linkg_cellular_link_heartbeat_prepare_pktinfo(struct msghdr *header, void *control, size_t control_size, unsigned int interface_index)
{
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;

    if (header == NULL || control == NULL || control_size < LINKG_CELLULAR_LINK_HEARTBEAT_TX_CONTROL_SIZE || interface_index == 0U)
    {
        return -EINVAL;
    }

    memset(control, 0, control_size);

    header->msg_control    = control;
    header->msg_controllen = control_size;

    cmsg = CMSG_FIRSTHDR(header);
    if (cmsg == NULL)
    {
        return -EIO;
    }

    cmsg->cmsg_level = IPPROTO_IPV6;
    cmsg->cmsg_type  = IPV6_PKTINFO;
    cmsg->cmsg_len   = CMSG_LEN(sizeof(*pktinfo));

    pktinfo = (struct in6_pktinfo *)CMSG_DATA(cmsg);

    memset(pktinfo, 0, sizeof(*pktinfo));
    pktinfo->ipi6_ifindex = interface_index;

    return 0;
}

/****************************** 心跳发送 ******************************/

/**
 * @brief 使用指定业务Socket向一个Cellular Peer发送心跳。
 */
static int _linkg_cellular_link_heartbeat_send_class(linkg_cellular_link_heartbeat_t *heartbeat, const linkg_path_endpoint_t *destination, linkg_link_tx_class_t tx_class, unsigned int interface_index)
{
    unsigned char                        control[LINKG_CELLULAR_LINK_HEARTBEAT_TX_CONTROL_SIZE];
    linkg_cellular_link_heartbeat_wire_t wire;
    struct sockaddr_in6                  target;
    struct msghdr                        message;
    struct iovec                         iovec;
    ssize_t                              sent;
    int                                  socket_fd;
    int                                  ret;

    if (heartbeat == NULL || destination == NULL)
    {
        return -EINVAL;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    ret = _linkg_cellular_link_heartbeat_validate_destination(destination);
    if (ret != 0)
    {
        return ret;
    }

    socket_fd = heartbeat->socket_fds[tx_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    if (heartbeat->service_ports[tx_class] == 0U)
    {
        return -EDESTADDRREQ;
    }

    ret = _linkg_cellular_link_heartbeat_build_wire(tx_class, &wire);
    if (ret != 0)
    {
        return ret;
    }

    target = *(const struct sockaddr_in6 *)&destination->address;
    target.sin6_port = htons(heartbeat->service_ports[tx_class]);

    memset(&message, 0, sizeof(message));
    memset(&iovec, 0, sizeof(iovec));

    iovec.iov_base = &wire;
    iovec.iov_len  = sizeof(wire);

    message.msg_name    = &target;
    message.msg_namelen = sizeof(target);
    message.msg_iov     = &iovec;
    message.msg_iovlen  = 1U;

    ret = _linkg_cellular_link_heartbeat_prepare_pktinfo(&message, control, sizeof(control), interface_index);
    if (ret != 0)
    {
        return ret;
    }

    do
    {
        sent = sendmsg(socket_fd, &message, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    while (sent < 0 && errno == EINTR);

    if (sent < 0)
    {
        return -errno;
    }

    if ((size_t)sent != sizeof(wire))
    {
        return -EIO;
    }

    return 0;
}

/**
 * @brief 使用三个业务Socket向一个Cellular Peer发送本轮全部心跳。
 */
static int _linkg_cellular_link_heartbeat_send_peer(linkg_cellular_link_heartbeat_t *heartbeat, const linkg_path_endpoint_t *destination, unsigned int interface_index)
{
    static const linkg_link_tx_class_t classes[LINKG_LINK_TX_CLASS_COUNT] =
    {
        LINKG_LINK_TX_CLASS_DATA,
        LINKG_LINK_TX_CLASS_REALTIME,
        LINKG_LINK_TX_CLASS_VIDEO
    };

    uint32_t index;
    int      first_error;
    int      ret;

    first_error = 0;

    for (index = 0U; index < LINKG_LINK_TX_CLASS_COUNT; index++)
    {
        ret = _linkg_cellular_link_heartbeat_send_class(heartbeat, destination, classes[index], interface_index);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 心跳维护 ******************************/

/**
 * @brief 按Peer当前Send Plan和真实业务PPS维护Cellular业务链路心跳。
 */
static int _linkg_cellular_link_heartbeat_process(linkg_cellular_link_heartbeat_t *heartbeat)
{
    linkg_path_endpoint_t                       path_endpoints[LINKG_NODE_PEER_MAX];
    linkg_cellular_link_heartbeat_peer_state_t *state;
    linkg_path_endpoint_t                       destination;
    linkg_path_stats_t                          stats;
    linkg_path_t                               *path;
    unsigned int                                interface_index;
    uint64_t                                    heartbeat_interval_us;
    uint64_t                                    now_us;
    uint32_t                                    path_count;
    uint32_t                                    tracked_count;
    uint32_t                                    node_id;
    bool                                        cellular_used;
    bool                                        tracking_changed;
    int                                         first_error;
    int                                         ret;

    if (heartbeat == NULL || heartbeat->link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    ret = _linkg_cellular_link_heartbeat_interface_index(heartbeat, &interface_index);
    if (ret != 0)
    {
        return ret;
    }

    memset(path_endpoints, 0, sizeof(path_endpoints));

    path_count = 0U;

    ret = linkg_node_get_path_endpoints(heartbeat->link_id,
                                        path_endpoints,
                                        LINKG_NODE_PEER_MAX,
                                        &path_count);
    if (ret != 0)
    {
        return ret;
    }

    tracked_count = _linkg_cellular_link_heartbeat_tracked_count(heartbeat);
    first_error   = 0;

    if (tracked_count != path_count)
    {
        ret = _linkg_cellular_link_heartbeat_refresh_tracking(heartbeat);
        if (ret != 0)
        {
            first_error = ret;
        }
    }

    now_us = linkg_time_monotonic_us();
    if (now_us == 0U)
    {
        return first_error != 0 ? first_error : -EIO;
    }

    tracking_changed = false;

    for (node_id = LINKG_RESOURCE_NODE_ID_MIN; node_id <= LINKG_RESOURCE_NODE_ID_MAX; node_id++)
    {
        state = &heartbeat->peer_states[node_id];
        if (!state->tracked)
        {
            continue;
        }

        path = NULL;

        memset(&destination, 0, sizeof(destination));
        memset(&stats, 0, sizeof(stats));

        ret = linkg_node_acquire_path((uint8_t)node_id,
                                      heartbeat->link_id,
                                      &path,
                                      &destination);
        if (ret == -ENOENT || ret == -ENODEV)
        {
            _linkg_cellular_link_heartbeat_reset_peer_state(state, false);
            tracking_changed = true;
            continue;
        }

        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        ret = linkg_path_get_stats(path, &stats);
        linkg_path_release(path);

        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        _linkg_cellular_link_heartbeat_update_pps(state, &stats, now_us);

        cellular_used = false;

        ret = _linkg_cellular_link_heartbeat_plan_uses_cellular((uint8_t)node_id,
                                                                 heartbeat->link_id,
                                                                 &cellular_used);
        if (ret != 0)
        {
            // Send Plan查询异常时按备用链路低频策略处理，不放大单个Peer异常。
            cellular_used = false;

            if (first_error == 0)
            {
                first_error = ret;
            }
        }

        heartbeat_interval_us = _linkg_cellular_link_heartbeat_select_interval(cellular_used,
                                                                                state->current_pps);

        if (!_linkg_cellular_link_heartbeat_due(state, now_us, heartbeat_interval_us))
        {
            continue;
        }

        // 先记录本轮时间，发送失败时也等待当前策略周期后再重试，避免100ms持续异常重发。
        state->last_heartbeat_us = now_us;

        ret = _linkg_cellular_link_heartbeat_send_peer(heartbeat,
                                                       &destination,
                                                       interface_index);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (tracking_changed)
    {
        ret = _linkg_cellular_link_heartbeat_refresh_tracking(heartbeat);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 工作线程 ******************************/

/**
 * @brief 运行蜂窝业务链路心跳周期循环。
 */
static int _linkg_cellular_link_heartbeat_run(linkg_cellular_link_heartbeat_t *heartbeat)
{
    struct pollfd descriptor;
    int           poll_result;
    int           wakeup_fd;
    int           ret;

    if (heartbeat == NULL)
    {
        return -EINVAL;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(&heartbeat->thread);
    if (wakeup_fd < 0)
    {
        return wakeup_fd;
    }

    memset(&descriptor, 0, sizeof(descriptor));

    descriptor.fd     = wakeup_fd;
    descriptor.events = POLLIN;

    while (linkg_thread_is_running(&heartbeat->thread))
    {
        descriptor.revents = 0;

        do
        {
            poll_result = poll(&descriptor, 1U, LINKG_CELLULAR_LINK_HEARTBEAT_POLL_INTERVAL_MS);
        }
        while (poll_result < 0 && errno == EINTR && linkg_thread_is_running(&heartbeat->thread));

        if (poll_result < 0)
        {
            return -errno;
        }

        if ((descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            return -EIO;
        }

        if ((descriptor.revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(&heartbeat->thread);
            if (ret != 0)
            {
                return ret;
            }

            if (!linkg_thread_is_running(&heartbeat->thread))
            {
                break;
            }
        }

        if (poll_result == 0)
        {
            // 单个Peer或接口临时失败不终止周期维护，下一轮继续重试。
            (void)_linkg_cellular_link_heartbeat_process(heartbeat);
        }
    }

    return 0;
}

/**
 * @brief 蜂窝业务链路心跳线程入口。
 */
static void _linkg_cellular_link_heartbeat_thread(linkg_thread_t *thread, void *user_data)
{
    linkg_cellular_link_heartbeat_t *heartbeat;
    int                              ret;

    heartbeat = user_data;

    if (thread == NULL || heartbeat == NULL)
    {
        return;
    }

    ret = _linkg_cellular_link_heartbeat_run(heartbeat);
    if (ret != 0 && linkg_thread_is_running(thread))
    {
        LINKG_LOG_ERROR("cellular link heartbeat thread exited unexpectedly, error=%d", ret);
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝业务链路心跳模块。
 */
int linkg_cellular_link_heartbeat_create(const linkg_cellular_link_heartbeat_config_t *config, linkg_cellular_link_heartbeat_t **out)
{
    linkg_cellular_link_heartbeat_t *heartbeat;
    int                              ret;

    if (out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    ret = _linkg_cellular_link_heartbeat_validate_config(config);
    if (ret != 0)
    {
        return ret;
    }

    heartbeat = calloc(1, sizeof(*heartbeat));
    if (heartbeat == NULL)
    {
        return -ENOMEM;
    }

    heartbeat->socket_fds     = config->socket_fds;
    heartbeat->service_ports  = config->service_ports;
    heartbeat->interface_name = config->interface_name;
    heartbeat->link_id        = LINKG_LINK_ID_INVALID;

    ret = linkg_thread_init(&heartbeat->thread,
                            LINKG_CELLULAR_LINK_HEARTBEAT_THREAD_NAME,
                            _linkg_cellular_link_heartbeat_thread,
                            heartbeat);
    if (ret != 0)
    {
        free(heartbeat);
        return ret;
    }

    *out = heartbeat;

    return 0;
}

/**
 * @brief 启动蜂窝业务链路心跳维护。
 */
int linkg_cellular_link_heartbeat_start(linkg_cellular_link_heartbeat_t *heartbeat, uint32_t link_id)
{
    int ret;

    if (heartbeat == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    if (linkg_thread_is_started(&heartbeat->thread))
    {
        return -EALREADY;
    }

    heartbeat->link_id = link_id;
    memset(heartbeat->peer_states, 0, sizeof(heartbeat->peer_states));

    ret = linkg_thread_start(&heartbeat->thread);
    if (ret != 0)
    {
        heartbeat->link_id = LINKG_LINK_ID_INVALID;
        return ret;
    }

    return 0;
}

/**
 * @brief 停止蜂窝业务链路心跳维护。
 */
int linkg_cellular_link_heartbeat_stop(linkg_cellular_link_heartbeat_t *heartbeat)
{
    int ret;

    if (heartbeat == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_thread_stop(&heartbeat->thread);
    if (ret != 0)
    {
        return ret;
    }

    heartbeat->link_id = LINKG_LINK_ID_INVALID;
    memset(heartbeat->peer_states, 0, sizeof(heartbeat->peer_states));

    return 0;
}

/**
 * @brief 销毁蜂窝业务链路心跳模块。
 */
void linkg_cellular_link_heartbeat_destroy(linkg_cellular_link_heartbeat_t *heartbeat)
{
    if (heartbeat == NULL)
    {
        return;
    }

    linkg_thread_deinit(&heartbeat->thread);
    free(heartbeat);
}

/****************************** 报文识别 ******************************/

/**
 * @brief 判断收到的数据是否为当前业务Socket对应的蜂窝链路心跳。
 */
bool linkg_cellular_link_heartbeat_is_packet(const uint8_t *data, uint32_t length, linkg_link_tx_class_t tx_class)
{
    const linkg_cellular_link_heartbeat_wire_t *wire;

    if (data == NULL || length != sizeof(linkg_cellular_link_heartbeat_wire_t))
    {
        return false;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return false;
    }

    wire = (const linkg_cellular_link_heartbeat_wire_t *)data;

    if (memcmp(wire->magic, g_cellular_link_heartbeat_magic, sizeof(wire->magic)) != 0)
    {
        return false;
    }

    if (wire->version != LINKG_CELLULAR_LINK_HEARTBEAT_VERSION)
    {
        return false;
    }

    if (wire->tx_class != (uint8_t)tx_class)
    {
        return false;
    }

    if (wire->reserved_0 != LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_0)
    {
        return false;
    }

    return wire->reserved_1 == LINKG_CELLULAR_LINK_HEARTBEAT_RESERVED_1;
}
