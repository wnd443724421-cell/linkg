/**
 * @file linkg_cellular_link.c
 * @brief LinkG蜂窝IPv6数据链路实现
 * @author Dawn
 * @version 1.4.0
 * @date 2026-09-10
 */

#define _GNU_SOURCE

#include "linkg_cellular_link.h"

#include <errno.h>
#include <limits.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "linkg_network_ops.h"
#include "linkg_system_resources.h"

#include "link_internal.h"
#include "cellular_link_heartbeat.h"
#include "cellular_rx.h"
#include "cellular_tx.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_LINK_TCLASS_DATA        0x00 // 普通数据IPv6 Traffic Class
#define LINKG_CELLULAR_LINK_TCLASS_VIDEO       0x80 // 视频业务IPv6 Traffic Class
#define LINKG_CELLULAR_LINK_TCLASS_REALTIME    0xC0 // 实时业务IPv6 Traffic Class
#define LINKG_CELLULAR_LINK_PRIORITY_DATA      2    // 普通数据Linux Socket优先级
#define LINKG_CELLULAR_LINK_PRIORITY_VIDEO     4    // 视频业务Linux Socket优先级
#define LINKG_CELLULAR_LINK_PRIORITY_REALTIME  6    // 实时业务Linux Socket优先级

/****************************** 内部类型 ******************************/

/**
 * @brief 蜂窝IPv6数据链路运行上下文。
 */
typedef struct
{
    linkg_link_t                     base;                                     // 链路基类，必须为首成员
    uint16_t                         service_ports[LINKG_LINK_TX_CLASS_COUNT]; // 各业务IPv6 UDP服务端口
    int                              socket_fds[LINKG_LINK_TX_CLASS_COUNT];    // 各业务IPv6 UDP收发套接字
    linkg_cellular_tx_t             *tx;                                      // 蜂窝发送模块
    linkg_cellular_rx_t             *rx;                                      // 蜂窝接收模块
    linkg_cellular_link_heartbeat_t *heartbeat;                               // 蜂窝业务链路心跳模块
} linkg_cellular_link_t;

_Static_assert(offsetof(linkg_cellular_link_t, base) == 0U, "linkg_link_t must be the first member");

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取业务类别对应的IPv6 Traffic Class。
 */
static int _cellular_link_tclass(linkg_link_tx_class_t tx_class)
{
    switch (tx_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            return LINKG_CELLULAR_LINK_TCLASS_REALTIME;

        case LINKG_LINK_TX_CLASS_VIDEO:
            return LINKG_CELLULAR_LINK_TCLASS_VIDEO;

        case LINKG_LINK_TX_CLASS_DATA:
            return LINKG_CELLULAR_LINK_TCLASS_DATA;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 获取业务类别对应的Linux Socket优先级。
 */
static int _cellular_link_priority(linkg_link_tx_class_t tx_class)
{
    switch (tx_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            return LINKG_CELLULAR_LINK_PRIORITY_REALTIME;

        case LINKG_LINK_TX_CLASS_VIDEO:
            return LINKG_CELLULAR_LINK_PRIORITY_VIDEO;

        case LINKG_LINK_TX_CLASS_DATA:
            return LINKG_CELLULAR_LINK_PRIORITY_DATA;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 关闭并清空单个描述符。
 */
static int _cellular_link_close_fd(int *descriptor)
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

/**
 * @brief 创建并绑定单个业务IPv6 UDP套接字。
 */
static int _cellular_link_open_socket(uint16_t local_port, int tclass, int priority, int *out)
{
    struct sockaddr_in6 local;
    int                 socket_fd;
    int                 enable;
    int                 ret;

    if (local_port == 0U || out == NULL)
    {
        return -EINVAL;
    }

    if (tclass < 0 || tclass > UCHAR_MAX || priority < 0)
    {
        return -EINVAL;
    }

    *out = -1;

    socket_fd = socket(AF_INET6, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, IPPROTO_UDP);
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

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_V6ONLY, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_RECVPKTINFO, &enable, sizeof(enable));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, SOL_SOCKET, SO_PRIORITY, &priority, sizeof(priority));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    ret = setsockopt(socket_fd, IPPROTO_IPV6, IPV6_TCLASS, &tclass, sizeof(tclass));
    if (ret != 0)
    {
        ret = -errno;
        goto fail_socket;
    }

    memset(&local, 0, sizeof(local));

    local.sin6_family = AF_INET6;
    local.sin6_addr   = in6addr_any;
    local.sin6_port   = htons(local_port);

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

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝具体链路对象。
 */
static int _cellular_link_init(linkg_link_t *link, const void *config)
{
    linkg_cellular_link_heartbeat_config_t heartbeat_config;
    const linkg_cellular_link_config_t    *cellular_config;
    linkg_cellular_link_t                 *cellular_link;
    uint32_t                               class_index;
    int                                    ret;

    if (link == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (link->runtime == NULL || link->packet_pool == NULL)
    {
        return -ENODEV;
    }

    cellular_link   = (linkg_cellular_link_t *)link;
    cellular_config = config;

    cellular_link->service_ports[LINKG_LINK_TX_CLASS_REALTIME] = cellular_config->realtime_port;
    cellular_link->service_ports[LINKG_LINK_TX_CLASS_VIDEO]    = cellular_config->video_port;
    cellular_link->service_ports[LINKG_LINK_TX_CLASS_DATA]     = cellular_config->data_port;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        cellular_link->socket_fds[class_index] = -1;
    }

    memset(&heartbeat_config, 0, sizeof(heartbeat_config));

    heartbeat_config.socket_fds     = cellular_link->socket_fds;
    heartbeat_config.service_ports  = cellular_link->service_ports;
    heartbeat_config.interface_name = LINKG_RESOURCE_INTERFACE_CELLULAR;

    ret = linkg_cellular_link_heartbeat_create(&heartbeat_config, &cellular_link->heartbeat);
    if (ret != 0)
    {
        return ret;
    }

    cellular_link->tx = linkg_cellular_tx_create(link->runtime->tx_batch_size,
                                                  cellular_link->socket_fds,
                                                  cellular_link->service_ports,
                                                  LINKG_RESOURCE_INTERFACE_CELLULAR);
    if (cellular_link->tx == NULL)
    {
        ret = -ENOMEM;
        goto fail_heartbeat;
    }

    cellular_link->rx = linkg_cellular_rx_create(link->runtime->rx_batch_size,
                                                  link->packet_pool,
                                                  cellular_link->socket_fds,
                                                  cellular_link->service_ports,
                                                  LINKG_RESOURCE_INTERFACE_CELLULAR);
    if (cellular_link->rx == NULL)
    {
        ret = -ENOMEM;
        goto fail_tx;
    }

    return 0;

fail_tx:
    linkg_cellular_tx_destroy(cellular_link->tx);
    cellular_link->tx = NULL;

fail_heartbeat:
    linkg_cellular_link_heartbeat_destroy(cellular_link->heartbeat);
    cellular_link->heartbeat = NULL;

    return ret;
}

/**
 * @brief 反初始化蜂窝具体链路对象。
 */
static void _cellular_link_deinit(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;
    uint32_t               class_index;

    if (link == NULL)
    {
        return;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    linkg_cellular_link_heartbeat_destroy(cellular_link->heartbeat);
    cellular_link->heartbeat = NULL;

    linkg_cellular_rx_destroy(cellular_link->rx);
    cellular_link->rx = NULL;

    linkg_cellular_tx_destroy(cellular_link->tx);
    cellular_link->tx = NULL;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        cellular_link->socket_fds[class_index]    = -1;
        cellular_link->service_ports[class_index] = 0U;
    }
}

/**
 * @brief 打开蜂窝具体链路运行资源。
 */
static int _cellular_link_open(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;
    int                    socket_fds[LINKG_LINK_TX_CLASS_COUNT];
    linkg_link_tx_class_t  tx_class;
    uint32_t               class_index;
    int                    priority;
    int                    tclass;
    int                    ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    if (linkg_cellular_rx_get_fd(cellular_link->rx) >= 0)
    {
        return -EALREADY;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        socket_fds[class_index] = -1;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        tx_class = (linkg_link_tx_class_t)class_index;
        tclass   = _cellular_link_tclass(tx_class);
        if (tclass < 0)
        {
            ret = tclass;
            goto fail;
        }

        priority = _cellular_link_priority(tx_class);
        if (priority < 0)
        {
            ret = priority;
            goto fail;
        }

        ret = _cellular_link_open_socket(cellular_link->service_ports[class_index], tclass, priority, &socket_fds[class_index]);
        if (ret != 0)
        {
            goto fail;
        }
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        cellular_link->socket_fds[class_index] = socket_fds[class_index];
        socket_fds[class_index]                = -1;
    }

    ret = linkg_cellular_rx_start(cellular_link->rx);
    if (ret != 0)
    {
        goto fail_opened;
    }

    ret = linkg_cellular_tx_start(cellular_link->tx);
    if (ret != 0)
    {
        (void)linkg_cellular_rx_stop(cellular_link->rx);
        goto fail_opened;
    }

    ret = linkg_cellular_link_heartbeat_start(cellular_link->heartbeat, link->id);
    if (ret != 0)
    {
        linkg_cellular_tx_stop(cellular_link->tx);
        (void)linkg_cellular_rx_stop(cellular_link->rx);
        goto fail_opened;
    }

    return 0;

fail_opened:
    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        (void)_cellular_link_close_fd(&cellular_link->socket_fds[class_index]);
    }

fail:
    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        (void)_cellular_link_close_fd(&socket_fds[class_index]);
    }

    return ret;
}

/**
 * @brief 关闭蜂窝具体链路运行资源。
 */
static int _cellular_link_close(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;
    uint32_t               class_index;
    int                    first_error;
    int                    ret;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;
    first_error   = 0;

    /**
     * 心跳线程直接复用三个业务Socket，必须先完成join，
     * 确认不再访问Socket以后才能继续停止TX/RX并关闭描述符。
     */
    ret = linkg_cellular_link_heartbeat_stop(cellular_link->heartbeat);
    if (ret != 0)
    {
        return ret;
    }

    linkg_cellular_tx_stop(cellular_link->tx);

    ret = linkg_cellular_rx_stop(cellular_link->rx);
    if (ret != 0)
    {
        first_error = ret;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        ret = _cellular_link_close_fd(&cellular_link->socket_fds[class_index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取蜂窝链路接收等待描述符。
 */
static int _cellular_link_get_rx_fd(linkg_link_t *link)
{
    linkg_cellular_link_t *cellular_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    return linkg_cellular_rx_get_fd(cellular_link->rx);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 将同一业务类别发送请求提交给蜂窝TX模块。
 *
 * Packet和Path引用所有权仍由调用方持有，蜂窝TX需要异步保存时自行增加引用。
 */
static int _cellular_link_send_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_cellular_link_t *cellular_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    return linkg_cellular_tx_submit(cellular_link->tx, path, tx_class, destination, packets, count, results);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 将蜂窝批量接收请求转交给蜂窝RX模块。
 */
static int _cellular_link_receive_batch(linkg_link_t *link, linkg_link_rx_item_t *items, uint32_t capacity)
{
    linkg_cellular_link_t *cellular_link;

    if (link == NULL)
    {
        return -EINVAL;
    }

    cellular_link = (linkg_cellular_link_t *)link;

    return linkg_cellular_rx_receive_batch(cellular_link->rx, items, capacity);
}

/****************************** 操作接口 ******************************/

static const linkg_link_ops_t g_cellular_link_ops =
{
    .instance_size = sizeof(linkg_cellular_link_t),
    .init          = _cellular_link_init,
    .deinit        = _cellular_link_deinit,
    .open          = _cellular_link_open,
    .close         = _cellular_link_close,
    .get_rx_fd     = _cellular_link_get_rx_fd,
    .send_batch    = _cellular_link_send_batch,
    .receive_batch = _cellular_link_receive_batch,
};

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝IPv6数据链路对象。
 */
int linkg_cellular_link_create(const linkg_link_config_t *link_config, const linkg_cellular_link_config_t *cellular_config, linkg_link_t **out)
{
    if (link_config == NULL || cellular_config == NULL || out == NULL)
    {
        return -EINVAL;
    }

    if (link_config->access != LINKG_LINK_ACCESS_CELLULAR)
    {
        return -EINVAL;
    }

    if (cellular_config->data_port == 0U ||
        cellular_config->realtime_port == 0U ||
        cellular_config->video_port == 0U)
    {
        return -EINVAL;
    }

    if (cellular_config->data_port == cellular_config->realtime_port ||
        cellular_config->data_port == cellular_config->video_port ||
        cellular_config->realtime_port == cellular_config->video_port)
    {
        return -EINVAL;
    }

    return linkg_link_create(link_config, &g_cellular_link_ops, cellular_config, out);
}
