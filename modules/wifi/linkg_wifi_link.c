/**
 * @file linkg_wifi_link.c
 * @brief LinkG Wi-Fi数据链路实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#define _GNU_SOURCE

#include "linkg_wifi_link.h"

#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_network_ops.h"

#include "link_internal.h"
#include "wifi_platform_internal.h"
#include "wifi_rx.h"
#include "wifi_traffic.h"
#include "wifi_tx.h"

/****************************** 模块常量 ******************************/

#define LINKG_WIFI_LINK_IP_TOS_DATA      0x00 // 普通数据流量，映射WMM AC_BE
#define LINKG_WIFI_LINK_IP_TOS_VIDEO     0x80 // 视频流量，映射WMM AC_VI
#define LINKG_WIFI_LINK_IP_TOS_REALTIME  0xC0 // 实时流量，映射WMM AC_VO

/****************************** 内部类型 ******************************/

/**
 * @brief Wi-Fi数据链路运行上下文。
 */
typedef struct
{
    linkg_link_t      base;                                           // 链路基类，必须为首成员
    struct in_addr    local_address;                                  // 当前绑定的本地IPv4地址
    uint16_t          service_ports[LINKG_WIFI_TRAFFIC_COUNT];        // 各业务UDP服务端口
    uint32_t          send_buffer_sizes[LINKG_WIFI_TRAFFIC_COUNT];    // 各业务UDP发送缓冲请求值
    uint32_t          receive_buffer_sizes[LINKG_WIFI_TRAFFIC_COUNT]; // 各业务UDP接收缓冲请求值
    int               socket_fds[LINKG_WIFI_TRAFFIC_COUNT];           // 各业务UDP收发套接字
    linkg_wifi_tx_t  *tx;                                             // Wi-Fi发送模块
    linkg_wifi_rx_t  *rx;                                             // Wi-Fi接收模块
} linkg_wifi_link_t;

_Static_assert(offsetof(linkg_wifi_link_t, base) == 0U, "linkg_link_t must be the first member");

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取业务类别对应的IPv4 TOS。
 */
static int _wifi_link_traffic_tos(linkg_wifi_traffic_class_t traffic_class)
{
    switch (traffic_class)
    {
        case LINKG_WIFI_TRAFFIC_REALTIME:
            return LINKG_WIFI_LINK_IP_TOS_REALTIME;

        case LINKG_WIFI_TRAFFIC_VIDEO:
            return LINKG_WIFI_LINK_IP_TOS_VIDEO;

        case LINKG_WIFI_TRAFFIC_DATA:
            return LINKG_WIFI_LINK_IP_TOS_DATA;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 创建并绑定单个业务UDP套接字。
 */
static int _wifi_link_open_socket(const struct in_addr *local_address, uint16_t local_port, int tos, uint32_t send_buffer_size, uint32_t receive_buffer_size, int *out)
{
    struct sockaddr_in local;
    socklen_t          option_length;
    uint64_t           expected_receive_buffer;
    int                actual_receive_buffer;
    int                actual_send_buffer;
    int                receive_buffer;
    int                send_buffer;
    int                socket_fd;
    int                enable;
    int                ret;

    if (local_address == NULL || out == NULL || local_port == 0U)
    {
        return -EINVAL;
    }

    if (send_buffer_size > (uint32_t)INT_MAX || receive_buffer_size > (uint32_t)INT_MAX)
    {
        return -EINVAL;
    }

    *out = -1;

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
        goto fail_socket;
    }

    if (send_buffer_size > 0U)
    {
        send_buffer = (int)send_buffer_size;

        ret = setsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer));
        if (ret != 0)
        {
            ret = -errno;
            goto fail_socket;
        }
    }

    if (receive_buffer_size > 0U)
    {
        receive_buffer = (int)receive_buffer_size;

        ret = setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &receive_buffer, sizeof(receive_buffer));
        if (ret != 0)
        {
            ret = -errno;
            goto fail_socket;
        }

        actual_receive_buffer = 0;
        option_length         = sizeof(actual_receive_buffer);

        ret = getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &actual_receive_buffer, &option_length);
        if (ret != 0)
        {
            ret = -errno;
            goto fail_socket;
        }

        expected_receive_buffer = (uint64_t)receive_buffer_size * 2U;

        if ((uint64_t)actual_receive_buffer < expected_receive_buffer)
        {
            ret = setsockopt(socket_fd, SOL_SOCKET, SO_RCVBUFFORCE, &receive_buffer, sizeof(receive_buffer));
            if (ret != 0)
            {
                LINKG_LOG_WARN("Wi-Fi UDP receive buffer limited, port=%u, requested=%u, actual=%d, force_error=%d",
                               local_port,
                               receive_buffer_size,
                               actual_receive_buffer,
                               errno);
            }
            else
            {
                actual_receive_buffer = 0;
                option_length         = sizeof(actual_receive_buffer);

                ret = getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &actual_receive_buffer, &option_length);
                if (ret != 0)
                {
                    ret = -errno;
                    goto fail_socket;
                }

                if ((uint64_t)actual_receive_buffer < expected_receive_buffer)
                {
                    LINKG_LOG_WARN("Wi-Fi UDP receive buffer below request, port=%u, requested=%u, actual=%d",
                                   local_port,
                                   receive_buffer_size,
                                   actual_receive_buffer);
                }
            }
        }
    }

    actual_send_buffer = 0;
    option_length      = sizeof(actual_send_buffer);

    ret = getsockopt(socket_fd, SOL_SOCKET, SO_SNDBUF, &actual_send_buffer, &option_length);
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    actual_receive_buffer = 0;
    option_length         = sizeof(actual_receive_buffer);

    ret = getsockopt(socket_fd, SOL_SOCKET, SO_RCVBUF, &actual_receive_buffer, &option_length);
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    LINKG_LOG_INFO("Wi-Fi UDP socket buffer, port=%u, sndbuf=%d, rcvbuf=%d", local_port, actual_send_buffer, actual_receive_buffer);

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_BINDTODEVICE, WIFI_PLATFORM_INTERFACE_NAME, strlen(WIFI_PLATFORM_INTERFACE_NAME));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IP, IP_TOS, &tos, sizeof(tos));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    memset(&local, 0, sizeof(local));

    local.sin_family = AF_INET;
    local.sin_addr   = *local_address;
    local.sin_port   = htons(local_port);

    ret = bind(socket_fd, (const struct sockaddr *)&local, sizeof(local));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    *out = socket_fd;

    return 0;

fail_socket:
    close(socket_fd);

    return ret;
}

/**
 * @brief 关闭并清空单个描述符。
 */
static int _wifi_link_close_fd(int *descriptor)
{
    int value;
    int ret;

    if (descriptor == NULL)
    {
        return -EINVAL;
    }

    value       = *descriptor;
    *descriptor = -1;

    if (value < 0)
    {
        return 0;
    }

    ret = close(value);
    if (ret != 0)
    {
        return -errno;
    }

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi具体链路对象。
 */
static int _wifi_link_init(linkg_link_t *link, const void *config)
{
    const linkg_wifi_link_config_t *wifi_config;
    linkg_wifi_link_t              *wifi_link;
    uint32_t                        class_index;

    if (link == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (link->runtime == NULL || link->packet_pool == NULL)
    {
        return -ENODEV;
    }

    wifi_link   = (linkg_wifi_link_t *)link;
    wifi_config = config;

    memset(&wifi_link->local_address, 0, sizeof(wifi_link->local_address));

    wifi_link->service_ports[LINKG_WIFI_TRAFFIC_REALTIME] = wifi_config->realtime_port;
    wifi_link->service_ports[LINKG_WIFI_TRAFFIC_VIDEO]    = wifi_config->video_port;
    wifi_link->service_ports[LINKG_WIFI_TRAFFIC_DATA]     = wifi_config->data_port;

    wifi_link->send_buffer_sizes[LINKG_WIFI_TRAFFIC_REALTIME] = wifi_config->realtime_send_buffer_size;
    wifi_link->send_buffer_sizes[LINKG_WIFI_TRAFFIC_VIDEO]    = wifi_config->video_send_buffer_size;
    wifi_link->send_buffer_sizes[LINKG_WIFI_TRAFFIC_DATA]     = wifi_config->data_send_buffer_size;

    wifi_link->receive_buffer_sizes[LINKG_WIFI_TRAFFIC_REALTIME] = wifi_config->realtime_receive_buffer_size;
    wifi_link->receive_buffer_sizes[LINKG_WIFI_TRAFFIC_VIDEO]    = wifi_config->video_receive_buffer_size;
    wifi_link->receive_buffer_sizes[LINKG_WIFI_TRAFFIC_DATA]     = wifi_config->data_receive_buffer_size;

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        wifi_link->socket_fds[class_index] = -1;
    }

    wifi_link->tx = linkg_wifi_tx_create(link->runtime->tx_batch_size, wifi_link->socket_fds, wifi_link->service_ports);
    if (wifi_link->tx == NULL)
    {
        return -ENOMEM;
    }

    wifi_link->rx = linkg_wifi_rx_create(link->runtime->rx_batch_size,
                                         link->packet_pool,
                                         wifi_link->socket_fds,
                                         wifi_link->service_ports);
    if (wifi_link->rx == NULL)
    {
        linkg_wifi_tx_destroy(wifi_link->tx);
        wifi_link->tx = NULL;
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief 反初始化Wi-Fi具体链路对象。
 */
static void _wifi_link_deinit(linkg_link_t *link)
{
    linkg_wifi_link_t *wifi_link;
    uint32_t           class_index;

    if (link == NULL)
    {
        return;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    linkg_wifi_rx_destroy(wifi_link->rx);
    wifi_link->rx = NULL;

    linkg_wifi_tx_destroy(wifi_link->tx);
    wifi_link->tx = NULL;

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        wifi_link->service_ports[class_index]        = 0U;
        wifi_link->send_buffer_sizes[class_index]    = 0U;
        wifi_link->receive_buffer_sizes[class_index] = 0U;
        wifi_link->socket_fds[class_index]           = -1;
    }

    memset(&wifi_link->local_address, 0, sizeof(wifi_link->local_address));
}

/**
 * @brief 打开Wi-Fi具体链路运行资源。
 */
static int _wifi_link_open(linkg_link_t *link)
{
    linkg_wifi_link_t          *wifi_link;
    int                         socket_fds[LINKG_WIFI_TRAFFIC_COUNT];
    linkg_wifi_traffic_class_t  traffic_class;
    uint32_t                    class_index;
    uint32_t                    send_buffer_size;
    uint32_t                    receive_buffer_size;
    int                         tos;
    int                         ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    if (linkg_wifi_rx_get_fd(wifi_link->rx) >= 0)
    {
        return -EALREADY;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        socket_fds[class_index] = -1;
    }

    ret = linkg_network_interface_get_ipv4(WIFI_PLATFORM_INTERFACE_NAME, &wifi_link->local_address);
    if (ret != 0)
    {
        return ret;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        traffic_class       = (linkg_wifi_traffic_class_t)class_index;
        tos                 = _wifi_link_traffic_tos(traffic_class);
        send_buffer_size    = wifi_link->send_buffer_sizes[traffic_class];
        receive_buffer_size = wifi_link->receive_buffer_sizes[traffic_class];

        if (tos < 0)
        {
            ret = tos;
            goto fail;
        }

        ret = _wifi_link_open_socket(&wifi_link->local_address,
                                     wifi_link->service_ports[class_index],
                                     tos,
                                     send_buffer_size,
                                     receive_buffer_size,
                                     &socket_fds[class_index]);
        if (ret != 0)
        {
            goto fail;
        }
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        wifi_link->socket_fds[class_index] = socket_fds[class_index];
        socket_fds[class_index]            = -1;
    }

    ret = linkg_wifi_rx_start(wifi_link->rx);
    if (ret != 0)
    {
        goto fail_opened;
    }

    ret = linkg_wifi_tx_start(wifi_link->tx);
    if (ret != 0)
    {
        (void)linkg_wifi_rx_stop(wifi_link->rx);
        goto fail_opened;
    }

    return 0;

fail_opened:
    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        (void)_wifi_link_close_fd(&wifi_link->socket_fds[class_index]);
    }

fail:
    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        (void)_wifi_link_close_fd(&socket_fds[class_index]);
    }

    return ret;
}

/**
 * @brief 关闭Wi-Fi具体链路运行资源。
 */
static int _wifi_link_close(linkg_link_t *link)
{
    linkg_wifi_link_t *wifi_link;
    uint32_t           class_index;
    int                first_error;
    int                ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link   = (linkg_wifi_link_t *)link;
    first_error = 0;

    linkg_wifi_tx_stop(wifi_link->tx);

    ret = linkg_wifi_rx_stop(wifi_link->rx);
    if (ret != 0)
    {
        first_error = ret;
    }

    for (class_index = 0U; class_index < LINKG_WIFI_TRAFFIC_COUNT; class_index++)
    {
        ret = _wifi_link_close_fd(&wifi_link->socket_fds[class_index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取Wi-Fi链路接收等待描述符。
 */
static int _wifi_link_get_rx_fd(linkg_link_t *link)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    return linkg_wifi_rx_get_fd(wifi_link->rx);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 将同一业务类别发送请求提交给Wi-Fi TX模块。
 *
 * Packet和Path引用所有权仍由调用方持有，Wi-Fi TX需要异步保存时自行增加引用。
 */
static int _wifi_link_send_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    return linkg_wifi_tx_submit(wifi_link->tx, path, tx_class, destination, packets, count, results);
}


/**
 * @brief 清理Wi-Fi链路发送队列中引用指定Path的待发送Packet。
 *
 * @note 本函数只负责将Link基类的清理请求下沉到Wi-Fi TX模块。
 *       具体三业务队列的扫描、Packet释放和Path释放由Wi-Fi TX模块完成。
 */
static int _wifi_link_purge_tx_path(linkg_link_t *link, linkg_path_t *path, uint32_t *purged_count)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL || path == NULL || purged_count == NULL)
    {
        return -EINVAL;
    }

    *purged_count = 0U;

    wifi_link = (linkg_wifi_link_t *)link;

    if (wifi_link->tx == NULL)
    {
        return -ENODEV;
    }

    return linkg_wifi_tx_purge_path(wifi_link->tx, path, purged_count);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 将Wi-Fi批量接收请求转交给Wi-Fi RX模块。
 */
static int _wifi_link_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t capacity)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    return linkg_wifi_rx_receive_batch(wifi_link->rx, items, capacity);
}

/****************************** 操作接口 ******************************/

static const linkg_link_ops_t g_wifi_link_ops =
{
    .instance_size      = sizeof(linkg_wifi_link_t),
    .init               = _wifi_link_init,
    .deinit             = _wifi_link_deinit,
    .open               = _wifi_link_open,
    .close              = _wifi_link_close,
    .get_rx_fd          = _wifi_link_get_rx_fd,
    .send_batch         = _wifi_link_send_batch,
    .receive_batch      = _wifi_link_receive_batch,
    .purge_tx_path      = _wifi_link_purge_tx_path,
};

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi数据链路对象。
 */
int linkg_wifi_link_create(const linkg_link_config_t *link_config, const linkg_wifi_link_config_t *wifi_config, linkg_link_t **out)
{
    if (link_config == NULL || wifi_config == NULL || out == NULL)
    {
        return -EINVAL;
    }

    if (link_config->access != LINKG_LINK_ACCESS_WIFI)
    {
        return -EINVAL;
    }

    if (wifi_config->data_port == 0U ||
        wifi_config->realtime_port == 0U ||
        wifi_config->video_port == 0U)
    {
        return -EINVAL;
    }

    if (wifi_config->data_port == wifi_config->realtime_port ||
        wifi_config->data_port == wifi_config->video_port ||
        wifi_config->realtime_port == wifi_config->video_port)
    {
        return -EINVAL;
    }

    return linkg_link_create(link_config, &g_wifi_link_ops, wifi_config, out);
}

/****************************** 统计查询 ******************************/

/**
 * @brief 获取指定对端节点的Wi-Fi链路累计接收和确认丢包统计。
 *
 * @note link必须为通过linkg_wifi_link_create创建的Wi-Fi具体Link。
 *       peer_node_id有效范围为1~254。
 *       接口只返回Wi-Fi RX累计Counter，不计算时间窗口和Loss Rate。
 */
int linkg_wifi_link_get_rx_stats(linkg_link_t *link, uint8_t peer_node_id, linkg_wifi_rx_stats_t *stats)
{
    linkg_wifi_link_t *wifi_link;

    if (link == NULL || stats == NULL)
    {
        return -EINVAL;
    }

    memset(stats, 0, sizeof(*stats));

    if (peer_node_id == 0U || peer_node_id == UINT8_MAX)
    {
        return -EINVAL;
    }

    if (linkg_link_get_access(link) != LINKG_LINK_ACCESS_WIFI)
    {
        return -EINVAL;
    }

    wifi_link = (linkg_wifi_link_t *)link;

    if (wifi_link->rx == NULL)
    {
        return -ENODEV;
    }

    return linkg_wifi_rx_get_peer_stats(wifi_link->rx, peer_node_id, stats);
}
