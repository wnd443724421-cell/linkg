/**
 * @file linkg_tun.c
 * @brief LinkG TUN数据面实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-11
 */

#include "linkg_tun.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/if_tun.h>
#include <linux/types.h>
#include <net/if.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_scheduler.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"
#include "linkg_transport.h"

/****************************** 兼容定义 ******************************/

// 兼容尚未安装私有UAPI头的用户态构建环境。
#ifndef LQ_TUN_IOC_READ_BATCH
#define LQ_TUN_BATCH_MAX                          1024U
#define LQ_TUN_BATCH_TIMEOUT_MAX_US               1000U
#define LQ_TUN_TRAFFIC_CLASS_REALTIME              0U
#define LQ_TUN_TRAFFIC_CLASS_VIDEO                 1U
#define LQ_TUN_TRAFFIC_CLASS_DATA                  2U
#define LQ_TUN_TRAFFIC_CLASS_COUNT                 3U
#define LQ_TUN_REALTIME_QUEUE_SIZE                 128U
#define LQ_TUN_VIDEO_QUEUE_SIZE                    256U
#define LQ_TUN_DATA_QUEUE_SIZE                     512U
#define LQ_TUN_TRAFFIC_RULE_MAX                    16U
#define LQ_TUN_PORT_PROTOCOL_TCP                   6U
#define LQ_TUN_PORT_PROTOCOL_UDP                   17U
#define LQ_TUN_IOC_READ_BATCH                     _IOWR('T', 240, struct lq_tun_batch_read)
#define LQ_TUN_IOC_WRITE_BATCH                    _IOWR('T', 241, struct lq_tun_batch_write)
#define LQ_TUN_IOC_SET_TRAFFIC_CONFIG             _IOW('T', 242, struct lq_tun_traffic_config)

struct lq_tun_traffic_rule
{
    __u8  traffic_class;
    __u8  protocol;
    __u16 start_port;
    __u16 end_port;
    __u16 reserved;
};

struct lq_tun_traffic_config
{
    __u32                      count;
    __u32                      reserved;
    struct lq_tun_traffic_rule rules[LQ_TUN_TRAFFIC_RULE_MAX];
};

struct lq_tun_batch_entry
{
    __aligned_u64 data;
    __u32         length;
    __u32         capacity;
};

struct lq_tun_batch_read
{
    __aligned_u64 entries;
    __u32         traffic_class;
    __u32         max_pkts;
    __u32         min_pkts;
    __u32         timeout_us;
    __u32         read_pkts;
    __u32         read_bytes;
    __s32         status;
};

struct lq_tun_batch_write
{
    __aligned_u64 entries;
    __u32         pkt_count;
    __u32         written_pkts;
    __u32         written_bytes;
    __s32         status;
};
#endif

/****************************** 模块常量 ******************************/

#define LINKG_TUN_DEVICE_PATH                     "/dev/net/tun"                                          // TUN字符设备路径
#define LINKG_TUN_THREAD_NAME                     "tun-rx"                                                // TUN读取线程名称

#ifndef LINKG_TUN_THREAD_CPU_CORE
#define LINKG_TUN_THREAD_CPU_CORE                 1                                                       // TUN读取线程绑定CPU1
#endif
#ifndef LINKG_TUN_THREAD_SCHED_PRIORITY
#define LINKG_TUN_THREAD_SCHED_PRIORITY           0                                                       // 0表示保持SCHED_OTHER
#endif
#define LINKG_TUN_BATCH_SIZE                      16U                                                     // 单次批量读写最大包数
#define LINKG_TUN_BATCH_MIN_PKTS                  5U                                                      // VIDEO/DATA批量读取最小聚合目标
#define LINKG_TUN_BATCH_TIMEOUT_US                60U                                                     // VIDEO/DATA批量读取最大聚合等待时间
#define LINKG_TUN_REALTIME_BATCH_MIN_PKTS         1U                                                      // REALTIME有包立即读取
#define LINKG_TUN_REALTIME_BATCH_TIMEOUT_US       0U                                                      // REALTIME禁止聚合等待
#define LINKG_TUN_TX_QUEUE_LENGTH                 512U                                                    // linkg0网络设备发送队列长度
#define LINKG_TUN_POLL_FD_COUNT                   2U                                                      // poll描述符数量
#define LINKG_TUN_POOL_RETRY_MS                   1U                                                      // 数据包池耗尽重试间隔
#define LINKG_TUN_IPV4_HEADER_MIN                 20U                                                     // IPv4最小头长度
#define LINKG_TUN_IPV4_VERSION                    4U                                                      // IPv4版本号
#define LINKG_TUN_IPV4_FRAGMENT_OFFSET_MASK       0x1FFFU                                                 // IPv4分片偏移掩码
#define LINKG_TUN_L4_PORT_BYTES                   4U                                                      // 源端口和目的端口总长度
#define LINKG_TUN_VIRTUAL_NODE_SHIFT              8U                                                      // 虚拟地址中Node ID位移
#define LINKG_TUN_VIRTUAL_HOST_MASK               0xFFU                                                   // 节点虚拟子网Host位掩码
#define LINKG_TUN_VIRTUAL_HOST_NETWORK            0U                                                      // 节点虚拟子网网络地址Host
#define LINKG_TUN_VIRTUAL_HOST_BROADCAST          255U                                                    // 节点虚拟子网广播地址Host

_Static_assert(LINKG_TUN_BATCH_SIZE <= LQ_TUN_BATCH_MAX, "TUN batch size exceeds kernel ABI limit");
_Static_assert(LINKG_TUN_BATCH_TIMEOUT_US <= LQ_TUN_BATCH_TIMEOUT_MAX_US, "TUN batch timeout exceeds kernel ABI limit");
_Static_assert(sizeof(struct lq_tun_batch_entry) == 16U, "invalid TUN batch entry ABI size");
_Static_assert(sizeof(struct lq_tun_batch_read) == 40U, "invalid TUN batch read ABI size");
_Static_assert(sizeof(struct lq_tun_batch_write) == 24U, "invalid TUN batch write ABI size");
_Static_assert(LINKG_NETWORK_TRAFFIC_RULE_MAX <= LQ_TUN_TRAFFIC_RULE_MAX, "network traffic rule count exceeds kernel ABI limit");

/****************************** 日志定义 ******************************/

#define LINKG_TUN_LOG_TAG                         "TUN"                                                    // TUN模块日志标签
#define LINKG_TUN_DEBUG(fmt, ...)                 LINKG_LOG_DEBUG("%s: " fmt, LINKG_TUN_LOG_TAG, ##__VA_ARGS__) // TUN调试日志
#define LINKG_TUN_INFO(fmt, ...)                  LINKG_LOG_INFO("%s: " fmt, LINKG_TUN_LOG_TAG, ##__VA_ARGS__)  // TUN信息日志
#define LINKG_TUN_WARN(fmt, ...)                  LINKG_LOG_WARN("%s: " fmt, LINKG_TUN_LOG_TAG, ##__VA_ARGS__)  // TUN警告日志
#define LINKG_TUN_ERROR(fmt, ...)                 LINKG_LOG_ERROR("%s: " fmt, LINKG_TUN_LOG_TAG, ##__VA_ARGS__) // TUN错误日志

/****************************** 内部类型 ******************************/

typedef struct
{
    struct in_addr source;           // 源IPv4地址
    struct in_addr destination;      // 目标IPv4地址
    uint16_t       source_port;      // 源端口
    uint16_t       destination_port; // 目标端口
    uint8_t        protocol;         // IPv4上层协议
    bool           ports_valid;      // TCP/UDP端口是否有效
} linkg_tun_ipv4_info_t;

typedef struct
{
    pthread_mutex_t                lock;                                             // TUN生命周期和写入路径保护锁
    linkg_thread_t                 read_thread;                                      // TUN读取线程
    linkg_packet_pool_t           *packet_pool;                                      // 外部Packet Pool，不拥有生命周期
    linkg_network_ipv4_config_t    tun_ipv4;                                         // linkg0自身IPv4配置
    linkg_network_ipv4_config_t    virtual_network;                                  // LinkG用户虚拟聚合网络
    linkg_network_traffic_config_t traffic;                                          // 用户业务分类配置
    linkg_packet_t                *read_packets[LINKG_TUN_BATCH_SIZE];               // 批量读取Packet
    struct lq_tun_batch_entry      read_entries[LINKG_TUN_BATCH_SIZE];               // 内核批量读取ABI条目
    linkg_scheduler_tx_item_t      tx_items[LINKG_TUN_BATCH_SIZE];                   // 当前单Class Scheduler发送元素
    int                            fd;                                               // TUN设备描述符
    uint8_t                        local_node_id;                                    // 本机逻辑节点编号
    bool                           initialized;                                      // 模块是否初始化
    bool                           started;                                          // TUN数据面是否启动
    bool                           write_failed;                                     // TUN写入路径故障状态
} linkg_tun_context_t;

/****************************** 全局上下文 ******************************/

static linkg_tun_context_t g_tun; // TUN模块全局上下文

/****************************** IPv4解析 ******************************/

/**
 * @brief 解析TUN读取的IPv4数据包。
 */
static int _linkg_tun_parse_ipv4(const linkg_packet_t *packet, linkg_tun_ipv4_info_t *info)
{
    const uint8_t *data;
    uint32_t       header_length;
    uint16_t       fragment;
    uint16_t       total_length;
    uint16_t       value;

    if (packet == NULL || info == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length < LINKG_TUN_IPV4_HEADER_MIN)
    {
        return -EINVAL;
    }

    data = linkg_packet_const_data(packet);
    if (data == NULL)
    {
        return -EINVAL;
    }

    if ((data[0] >> 4U) != LINKG_TUN_IPV4_VERSION)
    {
        return -EPROTONOSUPPORT;
    }

    header_length = (uint32_t)(data[0] & 0x0FU) * 4U;
    if (header_length < LINKG_TUN_IPV4_HEADER_MIN || header_length > packet->data_length)
    {
        return -EINVAL;
    }

    memcpy(&value, data + 2U, sizeof(value));

    total_length = ntohs(value);
    if (total_length != packet->data_length || total_length < header_length)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));

    info->protocol = data[9U];
    memcpy(&info->source.s_addr, data + 12U, sizeof(info->source.s_addr));
    memcpy(&info->destination.s_addr, data + 16U, sizeof(info->destination.s_addr));

    if (info->protocol != IPPROTO_TCP && info->protocol != IPPROTO_UDP)
    {
        return 0;
    }

    /**
     * 非首个IPv4分片没有TCP/UDP头，不能将后续分片载荷误解析为端口。
     * 首片即使设置MF仍然包含L4头，可以继续读取端口。
     */
    memcpy(&value, data + 6U, sizeof(value));
    fragment = ntohs(value);
    if ((fragment & LINKG_TUN_IPV4_FRAGMENT_OFFSET_MASK) != 0U)
    {
        return 0;
    }

    if ((uint32_t)total_length < header_length + LINKG_TUN_L4_PORT_BYTES)
    {
        return -EINVAL;
    }

    memcpy(&value, data + header_length, sizeof(value));
    info->source_port = ntohs(value);

    memcpy(&value, data + header_length + sizeof(value), sizeof(value));
    info->destination_port = ntohs(value);

    info->ports_valid = true;

    return 0;
}

/****************************** 业务分类 ******************************/

/**
 * @brief 将网络业务类型转换为内核TUN业务类型。
 */
static int _linkg_tun_traffic_class_to_uapi(linkg_network_traffic_class_t traffic_class, __u8 *uapi_class)
{
    if (uapi_class == NULL)
    {
        return -EINVAL;
    }

    switch (traffic_class)
    {
        case LINKG_NETWORK_TRAFFIC_CLASS_REALTIME:
            *uapi_class = LQ_TUN_TRAFFIC_CLASS_REALTIME;
            return 0;

        case LINKG_NETWORK_TRAFFIC_CLASS_VIDEO:
            *uapi_class = LQ_TUN_TRAFFIC_CLASS_VIDEO;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 将网络端口协议转换为内核TUN协议号。
 */
static int _linkg_tun_protocol_to_uapi(linkg_network_port_protocol_t protocol, __u8 *uapi_protocol)
{
    if (uapi_protocol == NULL)
    {
        return -EINVAL;
    }

    switch (protocol)
    {
        case LINKG_NETWORK_PORT_PROTOCOL_TCP:
            *uapi_protocol = LQ_TUN_PORT_PROTOCOL_TCP;
            return 0;

        case LINKG_NETWORK_PORT_PROTOCOL_UDP:
            *uapi_protocol = LQ_TUN_PORT_PROTOCOL_UDP;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 将Transport业务类型转换为内核TUN业务类型。
 */
static int _linkg_tun_transport_class_to_uapi(linkg_transport_class_t traffic_class, __u32 *uapi_class)
{
    if (uapi_class == NULL)
    {
        return -EINVAL;
    }

    switch (traffic_class)
    {
        case LINKG_TRANSPORT_CLASS_REALTIME:
            *uapi_class = LQ_TUN_TRAFFIC_CLASS_REALTIME;
            return 0;

        case LINKG_TRANSPORT_CLASS_VIDEO:
            *uapi_class = LQ_TUN_TRAFFIC_CLASS_VIDEO;
            return 0;

        case LINKG_TRANSPORT_CLASS_DATA:
            *uapi_class = LQ_TUN_TRAFFIC_CLASS_DATA;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 将用户业务规则转换为固定宽度内核TUN规则快照。
 */
static int _linkg_tun_build_kernel_traffic_config(const linkg_network_traffic_config_t *traffic, struct lq_tun_traffic_config *kernel_config)
{
    uint32_t index;
    int      ret;

    if (traffic == NULL || kernel_config == NULL)
    {
        return -EINVAL;
    }
    if (traffic->count > LINKG_NETWORK_TRAFFIC_RULE_MAX ||
        traffic->count > LQ_TUN_TRAFFIC_RULE_MAX)
    {
        return -E2BIG;
    }

    memset(kernel_config, 0, sizeof(*kernel_config));
    kernel_config->count = traffic->count;

    for (index = 0U; index < traffic->count; index++)
    {
        if (traffic->rules[index].start_port > traffic->rules[index].end_port)
        {
            return -EINVAL;
        }

        ret = _linkg_tun_traffic_class_to_uapi(traffic->rules[index].traffic_class, &kernel_config->rules[index].traffic_class);
        if (ret != 0)
        {
            return ret;
        }

        ret = _linkg_tun_protocol_to_uapi(traffic->rules[index].protocol, &kernel_config->rules[index].protocol);
        if (ret != 0)
        {
            return ret;
        }

        kernel_config->rules[index].start_port = traffic->rules[index].start_port;
        kernel_config->rules[index].end_port   = traffic->rules[index].end_port;
    }

    return 0;
}

/**
 * @brief 将业务规则快照同步到内核TUN。
 */
static int _linkg_tun_set_kernel_traffic_config(int fd, const linkg_network_traffic_config_t *traffic)
{
    struct lq_tun_traffic_config kernel_config;
    int                          ret;

    if (fd < 0 || traffic == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_tun_build_kernel_traffic_config(traffic, &kernel_config);
    if (ret != 0)
    {
        return ret;
    }

    do
    {
        ret = ioctl(fd, LQ_TUN_IOC_SET_TRAFFIC_CONFIG, &kernel_config);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        return -errno;
    }

    return 0;
}

/**
 * @brief 将Transport业务分类写入Packet元数据。
 */
static void _linkg_tun_packet_set_traffic_class(linkg_packet_t *packet, linkg_transport_class_t traffic_class)
{
    if (packet == NULL)
    {
        return;
    }

    switch (traffic_class)
    {
        case LINKG_TRANSPORT_CLASS_REALTIME:
            linkg_packet_set_realtime(packet, true);
            break;

        case LINKG_TRANSPORT_CLASS_VIDEO:
            linkg_packet_set_video(packet, true);
            break;

        case LINKG_TRANSPORT_CLASS_DATA:
        default:
            linkg_packet_set_data(packet);
            break;
    }
}

/****************************** 目标解析 ******************************/

/**
 * @brief 将TUN读取的IPv4目的地址解析为最终逻辑Node ID。
 *
 * 支持两类LinkG目的地址：
 * 1. TUN节点地址：tun_network /24 → <network>.<node_id>
 * 2. 用户虚拟地址：virtual_network /16 → <network>.<node_id>.<host>
 *
 * TUN只负责地址到最终逻辑Node ID的确定性映射；
 * 节点在线状态由Linux路由和后续Node/Scheduler共同约束。
 */
static int _linkg_tun_resolve_destination_node_id(const struct in_addr *destination, uint8_t *node_id)
{
    uint32_t address;
    uint32_t tun_network;
    uint32_t tun_netmask;
    uint32_t virtual_network;
    uint32_t virtual_netmask;
    uint32_t host_part;
    uint32_t endpoint_host;
    uint8_t  resolved_node_id;

    if (destination == NULL || node_id == NULL)
    {
        return -EINVAL;
    }

    address         = ntohl(destination->s_addr);
    tun_network     = ntohl(g_tun.tun_ipv4.ip.s_addr);
    tun_netmask     = ntohl(g_tun.tun_ipv4.netmask.s_addr);
    virtual_network = ntohl(g_tun.virtual_network.ip.s_addr);
    virtual_netmask = ntohl(g_tun.virtual_network.netmask.s_addr);

    /**
     * LinkG TUN节点地址：
     * 172.31.8.<node_id>
     *
     * TUN固定为/24，Host部分直接等于Node ID。
     */
    if ((address & tun_netmask) == (tun_network & tun_netmask))
    {
        host_part = address & ~tun_netmask;

        if (host_part > UINT8_MAX)
        {
            return -EHOSTUNREACH;
        }

        resolved_node_id = (uint8_t)host_part;
        if (resolved_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
            resolved_node_id > LINKG_RESOURCE_NODE_ID_MAX)
        {
            return -EHOSTUNREACH;
        }

        if (resolved_node_id == g_tun.local_node_id)
        {
            return -EHOSTUNREACH;
        }

        *node_id = resolved_node_id;

        return 0;
    }

    /**
     * 用户虚拟地址：
     * 172.28.<node_id>.<host>
     */
    if ((address & virtual_netmask) != (virtual_network & virtual_netmask))
    {
        return -EHOSTUNREACH;
    }

    host_part = address & ~virtual_netmask;

    resolved_node_id = (uint8_t)((host_part >> LINKG_TUN_VIRTUAL_NODE_SHIFT) & UINT8_MAX);
    endpoint_host    = host_part & LINKG_TUN_VIRTUAL_HOST_MASK;

    if (resolved_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        resolved_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EHOSTUNREACH;
    }

    if (resolved_node_id == g_tun.local_node_id)
    {
        return -EHOSTUNREACH;
    }

    if (endpoint_host == LINKG_TUN_VIRTUAL_HOST_NETWORK ||
        endpoint_host == LINKG_TUN_VIRTUAL_HOST_BROADCAST)
    {
        return -EHOSTUNREACH;
    }

    *node_id = resolved_node_id;

    return 0;
}

/****************************** TUN设备 ******************************/

/**
 * @brief 打开并绑定LinkG TUN字符设备。
 */
static int _linkg_tun_open(void)
{
    struct ifreq ifr;
    int          fd;
    int          ret;

    fd = open(LINKG_TUN_DEVICE_PATH, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
    {
        return -errno;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = (short)(IFF_TUN | IFF_NO_PI | IFF_TUN_EXCL);
    if (snprintf(ifr.ifr_name, sizeof(ifr.ifr_name), "%s", LINKG_RESOURCE_INTERFACE_TUN) >= (int)sizeof(ifr.ifr_name))
    {
        close(fd);
        return -ENAMETOOLONG;
    }

    if (ioctl(fd, TUNSETIFF, &ifr) != 0)
    {
        ret = -errno;

        close(fd);

        return ret;
    }

    return fd;
}

/**
 * @brief 配置LinkG TUN网络接口。
 */
static int _linkg_tun_configure_interface(void)
{
    int ret;

    ret = linkg_network_interface_set_mtu(LINKG_RESOURCE_INTERFACE_TUN, LINKG_RESOURCE_TUN_MTU);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_interface_set_tx_queue_length(LINKG_RESOURCE_INTERFACE_TUN, LINKG_TUN_TX_QUEUE_LENGTH);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_interface_set_ipv4(LINKG_RESOURCE_INTERFACE_TUN, &g_tun.tun_ipv4.ip, &g_tun.tun_ipv4.netmask);
    if (ret != 0)
    {
        return ret;
    }

    return linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_TUN, true);
}

/**
 * @brief 关闭TUN描述符。
 */
static int _linkg_tun_close_fd(int fd)
{
    if (fd < 0)
    {
        return 0;
    }

    if (close(fd) != 0)
    {
        return -errno;
    }

    return 0;
}

/****************************** 批量读取 ******************************/

/**
 * @brief 从TUN批量读取数据到Packet Pool。
 *
 * read_count返回已经成功写入read_packets的数据包数量。
 * 当部分数据成功后再出现内核错误时，返回错误同时保留read_count。
 */
static int _linkg_tun_read_batch(linkg_transport_class_t traffic_class, uint32_t packet_count, uint32_t *read_count)
{
    struct lq_tun_batch_read request;
    linkg_packet_t          *packet;
    uint32_t                 capacity;
    uint32_t                 index;
    __u32                    uapi_class;
    int                      ret;

    if (traffic_class < LINKG_TRANSPORT_CLASS_REALTIME ||
        traffic_class >= LINKG_TRANSPORT_CLASS_COUNT ||
        packet_count == 0U ||
        packet_count > LINKG_TUN_BATCH_SIZE ||
        read_count == NULL)
    {
        return -EINVAL;
    }

    ret = _linkg_tun_transport_class_to_uapi(traffic_class, &uapi_class);
    if (ret != 0)
    {
        return ret;
    }

    *read_count = 0U;

    for (index = 0U; index < packet_count; index++)
    {
        packet = g_tun.read_packets[index];
        if (packet == NULL)
        {
            return -EINVAL;
        }

        capacity = linkg_packet_capacity(packet);
        if (capacity == 0U || capacity > UINT16_MAX)
        {
            return -EMSGSIZE;
        }

        g_tun.read_entries[index].data     = (__u64)(uintptr_t)linkg_packet_data(packet);
        g_tun.read_entries[index].length   = 0U;
        g_tun.read_entries[index].capacity = capacity;
    }

    memset(&request, 0, sizeof(request));

    request.entries       = (__u64)(uintptr_t)g_tun.read_entries;
    request.traffic_class = uapi_class;
    request.max_pkts      = packet_count;

    if (traffic_class == LINKG_TRANSPORT_CLASS_REALTIME)
    {
        request.min_pkts   = LINKG_TUN_REALTIME_BATCH_MIN_PKTS;
        request.timeout_us = LINKG_TUN_REALTIME_BATCH_TIMEOUT_US;
    }
    else
    {
        request.min_pkts   = packet_count < LINKG_TUN_BATCH_MIN_PKTS ? packet_count : LINKG_TUN_BATCH_MIN_PKTS;
        request.timeout_us = LINKG_TUN_BATCH_TIMEOUT_US;
    }

    do
    {
        ret = ioctl(g_tun.fd, LQ_TUN_IOC_READ_BATCH, &request);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        return -errno;
    }

    if (request.read_pkts > packet_count || ret != (int)request.read_pkts)
    {
        return -EPROTO;
    }

    *read_count = request.read_pkts;

    for (index = 0U; index < request.read_pkts; index++)
    {
        if (g_tun.read_entries[index].length == 0U ||
            g_tun.read_entries[index].length > g_tun.read_entries[index].capacity)
        {
            return -EPROTO;
        }

        g_tun.read_packets[index]->data_length = g_tun.read_entries[index].length;
    }

    if (request.status < 0)
    {
        return request.status;
    }

    return 0;
}

/**
 * @brief 处理一个内核已经完成业务分类的TUN读取批次并提交Scheduler。
 *
 * @note 当前批次全部来自同一个内核Class Ring，用户态不再执行端口业务分类。
 */
static void _linkg_tun_process_read_batch(linkg_transport_class_t traffic_class, uint32_t packet_count)
{
    linkg_scheduler_tx_context_t context;
    linkg_tun_ipv4_info_t        info;
    linkg_packet_t              *packet;
    uint32_t                     item_count;
    uint32_t                     index;
    uint8_t                      destination_node_id;
    int                          ret;

    if (traffic_class < LINKG_TRANSPORT_CLASS_REALTIME ||
        traffic_class >= LINKG_TRANSPORT_CLASS_COUNT ||
        packet_count == 0U ||
        packet_count > LINKG_TUN_BATCH_SIZE)
    {
        return;
    }

    item_count = 0U;

    for (index = 0U; index < packet_count; index++)
    {
        packet = g_tun.read_packets[index];
        if (packet == NULL || packet->data_length == 0U)
        {
            continue;
        }

        ret = _linkg_tun_parse_ipv4(packet, &info);
        if (ret != 0)
        {
            continue;
        }

        ret = _linkg_tun_resolve_destination_node_id(&info.destination, &destination_node_id);
        if (ret != 0)
        {
            continue;
        }

        _linkg_tun_packet_set_traffic_class(packet, traffic_class);

        g_tun.tx_items[item_count].packet              = packet;
        g_tun.tx_items[item_count].type                = LINKG_TRANSPORT_TYPE_USER_DATA;
        g_tun.tx_items[item_count].destination_node_id = destination_node_id;
        g_tun.tx_items[item_count].result              = -EINPROGRESS;
        item_count++;
    }

    if (item_count == 0U)
    {
        return;
    }

    memset(&context, 0, sizeof(context));

    context.traffic_class     = traffic_class;
    context.policy            = LINKG_SCHEDULER_POLICY_DEFAULT;
    context.specified_link_id = LINKG_LINK_ID_INVALID;

    ret = linkg_scheduler_submit_batch(&context, g_tun.tx_items, item_count);
    if (ret < 0)
    {
        return;
    }
}

/****************************** 批量写入 ******************************/

/**
 * @brief 将Transport本机USER_DATA批量写入TUN。
 */
static int _linkg_tun_write_batch(const linkg_transport_delivery_t *items, uint32_t count)
{
    struct lq_tun_batch_entry entries[LINKG_TUN_BATCH_SIZE];
    struct lq_tun_batch_write request;
    linkg_packet_t            *packet;
    uint32_t                   chunk_count;
    uint32_t                   capacity;
    uint32_t                   offset;
    uint32_t                   index;
    int                        error;
    int                        ret;

    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    offset = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_TUN_BATCH_SIZE)
        {
            chunk_count = LINKG_TUN_BATCH_SIZE;
        }

        memset(entries, 0, sizeof(entries));

        for (index = 0U; index < chunk_count; index++)
        {
            packet = items[offset + index].packet;
            if (packet == NULL || packet->data_length == 0U)
            {
                return -EINVAL;
            }

            capacity = linkg_packet_capacity(packet);

            if (packet->data_length > capacity || capacity > UINT16_MAX)
            {
                return -EMSGSIZE;
            }

            entries[index].data     = (__u64)(uintptr_t)linkg_packet_data(packet);
            entries[index].length   = packet->data_length;
            entries[index].capacity = capacity;
        }

        memset(&request, 0, sizeof(request));

        request.entries   = (__u64)(uintptr_t)entries;
        request.pkt_count = chunk_count;

        do
        {
            ret = ioctl(g_tun.fd, LQ_TUN_IOC_WRITE_BATCH, &request);
        }
        while (ret < 0 && errno == EINTR);

        if (ret < 0)
        {
            return -errno;
        }

        if (request.written_pkts > chunk_count || ret != (int)request.written_pkts)
        {
            return -EPROTO;
        }

        if (request.written_pkts != chunk_count)
        {
            error = request.status < 0 ? request.status : -EAGAIN;
            return error;
        }

        if (request.status < 0)
        {
            return request.status;
        }

        offset += chunk_count;
    }

    return 0;
}

/****************************** Transport接收 ******************************/

/**
 * @brief Transport USER_DATA本机交付回调。
 *
 * Transport仅在当前回调期间借用items中的Packet引用，
 * TUN批量写入必须在回调返回前同步完成。
 */
static int _linkg_tun_transport_receive(const linkg_transport_delivery_t *items, uint32_t count, void *user_data)
{
    int lock_ret;
    int ret;

    (void)user_data;

    if (items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    lock_ret = pthread_mutex_lock(&g_tun.lock);
    if (lock_ret != 0)
    {
        return -lock_ret;
    }

    if (!g_tun.initialized || !g_tun.started || g_tun.fd < 0)
    {
        pthread_mutex_unlock(&g_tun.lock);
        return -ESHUTDOWN;
    }

    ret = _linkg_tun_write_batch(items, count);
    if (ret != 0)
    {
        if (!g_tun.write_failed)
        {
            LINKG_TUN_WARN("write batch failed, count=%u, error=%d", count, ret);
            g_tun.write_failed = true;
        }
    }
    else if (g_tun.write_failed)
    {
        LINKG_TUN_INFO("write path recovered");
        g_tun.write_failed = false;
    }

    pthread_mutex_unlock(&g_tun.lock);

    return ret;
}

/****************************** 读取线程 ******************************/

/**
 * @brief TUN读取线程。
 */
static void _linkg_tun_read_thread(linkg_thread_t *thread, void *user_data)
{
    struct pollfd descriptors[LINKG_TUN_POLL_FD_COUNT];
    uint32_t      allocated_count;
    uint32_t      class_index;
    uint32_t      read_count;
    bool          handled;
    int           wakeup_fd;
    int           ret;

    (void)user_data;

    if (thread == NULL)
    {
        return;
    }

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        LINKG_TUN_ERROR("get read thread wakeup fd failed, error=%d", wakeup_fd);
        return;
    }

    memset(descriptors, 0, sizeof(descriptors));

    descriptors[0].fd     = wakeup_fd;
    descriptors[0].events = POLLIN;
    descriptors[1].fd     = g_tun.fd;
    descriptors[1].events = POLLIN;

    LINKG_TUN_DEBUG("read thread entered");

    while (linkg_thread_is_running(thread))
    {
        descriptors[0].revents = 0;
        descriptors[1].revents = 0;

        do
        {
            ret = poll(descriptors, LINKG_TUN_POLL_FD_COUNT, -1);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (ret < 0)
        {
            if (linkg_thread_is_running(thread))
            {
                LINKG_TUN_ERROR("poll descriptors failed, error=%d", -errno);
            }

            break;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                if (linkg_thread_is_running(thread))
                {
                    LINKG_TUN_ERROR("clear read thread wakeup failed, error=%d", ret);
                }

                break;
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_TUN_ERROR("read thread wakeup fd failed, revents=0x%x", descriptors[0].revents);
            break;
        }

        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_TUN_ERROR("TUN fd failed, revents=0x%x", descriptors[1].revents);
            break;
        }

        if ((descriptors[1].revents & POLLIN) == 0)
        {
            continue;
        }

        do
        {
            handled = false;

            // Transport Class枚举顺序即读取优先级：REALTIME -> VIDEO -> DATA。
            for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
            {
                allocated_count = linkg_packet_pool_alloc_batch(g_tun.packet_pool, g_tun.read_packets, LINKG_TUN_BATCH_SIZE);
                if (allocated_count == 0U)
                {
                    (void)linkg_time_sleep_ms(LINKG_TUN_POOL_RETRY_MS);
                    break;
                }

                read_count = 0U;
                ret = _linkg_tun_read_batch((linkg_transport_class_t)class_index, allocated_count, &read_count);

                if (read_count > 0U)
                {
                    handled = true;
                    _linkg_tun_process_read_batch((linkg_transport_class_t)class_index, read_count);
                }

                linkg_packet_pool_release_batch(g_tun.packet_pool, g_tun.read_packets, allocated_count);

                if (ret == -EAGAIN || ret == -EWOULDBLOCK)
                {
                    continue;
                }

                if (ret != 0)
                {
                    LINKG_TUN_ERROR("read class batch failed, class=%u, capacity=%u, received=%u, error=%d",
                                    class_index,
                                    allocated_count,
                                    read_count,
                                    ret);
                    goto exit;
                }
            }
        }
        while (handled && linkg_thread_is_running(thread));
    }

exit:
    LINKG_TUN_DEBUG("read thread exited");
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化TUN数据面。
 *
 * @note Scheduler必须已经初始化用于TUN发送调度；
 *       Transport必须已经初始化并允许注册USER_DATA本机交付回调。
 *       packet_pool由上层持有，整个TUN生命周期内必须保持有效。
 */
int linkg_tun_init(linkg_packet_pool_t *packet_pool)
{
    linkg_thread_config_t thread_config;
    uint32_t              packet_capacity;
    int                   ret;

    if (packet_pool == NULL || !packet_pool->initialized)
    {
        return -EINVAL;
    }

    if (g_tun.initialized)
    {
        return -EALREADY;
    }

    if (packet_pool->headroom < LINKG_TRANSPORT_WIRE_HEADER_MAX_SIZE)
    {
        return -ENOBUFS;
    }

    packet_capacity = packet_pool->slot_size - packet_pool->headroom;

    if (packet_capacity < LINKG_RESOURCE_TUN_MTU ||
        packet_capacity > UINT16_MAX)
    {
        return -EMSGSIZE;
    }

    memset(&g_tun, 0, sizeof(g_tun));

    g_tun.fd          = -1;
    g_tun.packet_pool = packet_pool;

    ret = pthread_mutex_init(&g_tun.lock, NULL);
    if (ret != 0)
    {
        memset(&g_tun, 0, sizeof(g_tun));
        return -ret;
    }

    memset(&thread_config, 0, sizeof(thread_config));

    thread_config.cpu_core         = LINKG_TUN_THREAD_CPU_CORE;
    thread_config.affinity_enabled = true;
#if LINKG_TUN_THREAD_SCHED_PRIORITY > 0
    thread_config.sched_policy       = SCHED_RR;
    thread_config.sched_priority     = LINKG_TUN_THREAD_SCHED_PRIORITY;
    thread_config.scheduling_enabled = true;
#endif

    ret = linkg_thread_init_with_config(&g_tun.read_thread, LINKG_TUN_THREAD_NAME, _linkg_tun_read_thread, NULL, &thread_config);
    if (ret != 0)
    {
        pthread_mutex_destroy(&g_tun.lock);
        memset(&g_tun, 0, sizeof(g_tun));
        return ret;
    }

    g_tun.initialized = true;

    ret = linkg_transport_register_handler(LINKG_TRANSPORT_TYPE_USER_DATA, _linkg_tun_transport_receive, NULL);
    if (ret != 0)
    {
        g_tun.initialized = false;

        linkg_thread_deinit(&g_tun.read_thread);
        pthread_mutex_destroy(&g_tun.lock);

        memset(&g_tun, 0, sizeof(g_tun));

        return ret;
    }

    LINKG_TUN_INFO("module initialized, batch=%u, rt_min=%u, rt_timeout=%uus, normal_min=%u, normal_timeout=%uus, cpu=%d",
                   LINKG_TUN_BATCH_SIZE,
                   LINKG_TUN_REALTIME_BATCH_MIN_PKTS,
                   LINKG_TUN_REALTIME_BATCH_TIMEOUT_US,
                   LINKG_TUN_BATCH_MIN_PKTS,
                   LINKG_TUN_BATCH_TIMEOUT_US,
                   LINKG_TUN_THREAD_CPU_CORE);

    return 0;
}

/**
 * @brief 创建、配置并启动LinkG TUN数据面。
 *
 * 启动阶段一次读取并缓存本机TUN地址、虚拟网络、Node ID和业务规则，
 * 热路径不再访问Config模块。
 */
int linkg_tun_start(void)
{
    linkg_network_config_t      network_config;
    linkg_network_ipv4_config_t tun_ipv4;
    linkg_network_ipv4_config_t virtual_network;
    int                         fd;
    int                         stop_ret;
    int                         ret;

    if (!g_tun.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_tun.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_tun.started)
    {
        pthread_mutex_unlock(&g_tun.lock);
        return -EALREADY;
    }

    pthread_mutex_unlock(&g_tun.lock);

    memset(&network_config, 0, sizeof(network_config));
    memset(&tun_ipv4, 0, sizeof(tun_ipv4));
    memset(&virtual_network, 0, sizeof(virtual_network));

    ret = linkg_config_get_network(&network_config);
    if (ret != 0)
    {
        return ret;
    }

    if (network_config.node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        network_config.node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_tun(&network_config, &tun_ipv4);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_config_get_virtual_network(&network_config, &virtual_network);
    if (ret != 0)
    {
        return ret;
    }

    fd = _linkg_tun_open();
    if (fd < 0)
    {
        return fd;
    }

    ret = _linkg_tun_set_kernel_traffic_config(fd, &network_config.traffic);
    if (ret != 0)
    {
        (void)_linkg_tun_close_fd(fd);
        return ret;
    }

    ret = pthread_mutex_lock(&g_tun.lock);
    if (ret != 0)
    {
        (void)_linkg_tun_close_fd(fd);
        return -ret;
    }

    g_tun.fd              = fd;
    g_tun.tun_ipv4        = tun_ipv4;
    g_tun.virtual_network = virtual_network;
    g_tun.traffic         = network_config.traffic;
    g_tun.local_node_id   = network_config.node_id;
    g_tun.write_failed    = false;

    pthread_mutex_unlock(&g_tun.lock);

    ret = _linkg_tun_configure_interface();
    if (ret != 0)
    {
        goto fail_close;
    }

    ret = linkg_thread_start(&g_tun.read_thread);
    if (ret != 0)
    {
        goto fail_interface;
    }

    ret = pthread_mutex_lock(&g_tun.lock);
    if (ret != 0)
    {
        ret = -ret;
        stop_ret = linkg_thread_stop(&g_tun.read_thread);
        if (stop_ret != 0)
        {
            LINKG_TUN_ERROR("rollback read thread stop failed, error=%d", stop_ret);
            return stop_ret;
        }

        goto fail_interface;
    }

    g_tun.started = true;

    pthread_mutex_unlock(&g_tun.lock);

    LINKG_TUN_INFO("module started, interface=%s, node_id=%u, mtu=%u", LINKG_RESOURCE_INTERFACE_TUN, g_tun.local_node_id, LINKG_RESOURCE_TUN_MTU);

    return 0;

fail_interface:
    (void)linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_TUN, false);

fail_close:
    pthread_mutex_lock(&g_tun.lock);

    g_tun.fd            = -1;
    g_tun.local_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    g_tun.write_failed  = false;

    memset(&g_tun.tun_ipv4, 0, sizeof(g_tun.tun_ipv4));
    memset(&g_tun.virtual_network, 0, sizeof(g_tun.virtual_network));
    memset(&g_tun.traffic, 0, sizeof(g_tun.traffic));

    pthread_mutex_unlock(&g_tun.lock);

    (void)_linkg_tun_close_fd(fd);

    return ret;
}

/**
 * @brief 停止TUN数据面并关闭LinkG TUN接口。
 */
int linkg_tun_stop(void)
{
    bool thread_started;
    int  fd;
    int  first_error;
    int  ret;

    if (!g_tun.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_tun.lock);
    if (ret != 0)
    {
        return -ret;
    }

    thread_started = linkg_thread_is_started(&g_tun.read_thread);
    if (!g_tun.started && !thread_started)
    {
        pthread_mutex_unlock(&g_tun.lock);
        return 0;
    }

    g_tun.started = false;

    pthread_mutex_unlock(&g_tun.lock);

    first_error = 0;

    if (thread_started)
    {
        ret = linkg_thread_stop(&g_tun.read_thread);
        if (ret != 0)
        {
            LINKG_TUN_ERROR("stop read thread failed, error=%d", ret);
            return ret;
        }
    }

    ret = pthread_mutex_lock(&g_tun.lock);
    if (ret != 0)
    {
        return -ret;
    }

    fd = g_tun.fd;

    g_tun.fd            = -1;
    g_tun.local_node_id = LINKG_RESOURCE_NODE_ID_INVALID;
    g_tun.write_failed  = false;

    memset(&g_tun.tun_ipv4, 0, sizeof(g_tun.tun_ipv4));
    memset(&g_tun.virtual_network, 0, sizeof(g_tun.virtual_network));
    memset(&g_tun.traffic, 0, sizeof(g_tun.traffic));

    pthread_mutex_unlock(&g_tun.lock);

    ret = linkg_network_interface_set_up(LINKG_RESOURCE_INTERFACE_TUN, false);
    if (ret != 0 && ret != -ENODEV && ret != -ENXIO)
    {
        first_error = ret;
    }

    ret = _linkg_tun_close_fd(fd);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    if (first_error != 0)
    {
        return first_error;
    }

    LINKG_TUN_INFO("module stopped");

    return 0;
}

/**
 * @brief 动态更新内核TUN业务分类规则。
 *
 * @note 新规则只影响ioctl成功后新进入TUN的Packet，已经入队的Packet不会重新分类。
 */
int linkg_tun_update_traffic_config(const linkg_network_traffic_config_t *traffic)
{
    int ret;

    if (traffic == NULL)
    {
        return -EINVAL;
    }
    if (!g_tun.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_tun.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_tun.fd < 0 || !g_tun.started)
    {
        pthread_mutex_unlock(&g_tun.lock);
        return -ENETDOWN;
    }

    ret = _linkg_tun_set_kernel_traffic_config(g_tun.fd, traffic);
    if (ret == 0)
    {
        g_tun.traffic = *traffic;
    }

    pthread_mutex_unlock(&g_tun.lock);

    return ret;
}

/**
 * @brief 反初始化TUN数据面。
 *
 * 调用前TUN必须停止，并且不得再有Transport接收线程进入USER_DATA回调。
 */
int linkg_tun_deinit(void)
{
    int ret;

    if (!g_tun.initialized)
    {
        return 0;
    }

    if (g_tun.started || linkg_thread_is_started(&g_tun.read_thread))
    {
        return -EBUSY;
    }

    ret = linkg_transport_unregister_handler(LINKG_TRANSPORT_TYPE_USER_DATA);
    if (ret != 0)
    {
        return ret;
    }

    linkg_thread_deinit(&g_tun.read_thread);

    ret = pthread_mutex_destroy(&g_tun.lock);
    if (ret != 0)
    {
        return -ret;
    }

    memset(&g_tun, 0, sizeof(g_tun));

    g_tun.fd = -1;

    LINKG_TUN_INFO("module deinitialized");

    return 0;
}
