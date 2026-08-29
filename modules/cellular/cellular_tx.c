/**
 * @file cellular_tx.c
 * @brief LinkG蜂窝链路发送模块实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#define _GNU_SOURCE // 启用GNU扩展接口

#include "cellular_tx.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <sched.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "linkg_network_ops.h"
#include "linkg_packet_pool.h"
#include "linkg_time.h"

#include "cellular_tx_queue.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_TX_IPV6_HEADER_SIZE       40U                           // IPv6固定头部长度
#define LINKG_CELLULAR_TX_UDP_HEADER_SIZE        8U                            // UDP头部长度
#define LINKG_CELLULAR_TX_MTU                    1500U                         // 蜂窝链路MTU
#define LINKG_CELLULAR_TX_UDP_PAYLOAD_MAX        (LINKG_CELLULAR_TX_MTU - LINKG_CELLULAR_TX_IPV6_HEADER_SIZE - LINKG_CELLULAR_TX_UDP_HEADER_SIZE) // 单个UDP报文最大负载
#define LINKG_CELLULAR_TX_NORMAL_SEND_MAX        32U                           // VIDEO/DATA单轮最大发送预算
#define LINKG_CELLULAR_TX_VIDEO_QUEUE_CAPACITY   96U                           // VIDEO等待队列容量
#define LINKG_CELLULAR_TX_DATA_QUEUE_CAPACITY    96U                           // DATA等待队列容量
#define LINKG_CELLULAR_TX_VIDEO_MAX_AGE_US       100000ULL                     // VIDEO等待队列最大驻留时间
#define LINKG_CELLULAR_TX_DATA_MAX_AGE_US        150000ULL                     // DATA等待队列最大驻留时间
#define LINKG_CELLULAR_TX_SENDMMSG_CHUNK_MAX     32U                           // Normal单次sendmmsg最大消息数量
#define LINKG_CELLULAR_TX_CONTROL_SIZE           CMSG_SPACE(sizeof(struct in6_pktinfo)) // 单个IPv6 PKTINFO控制区大小

/****************************** 内部类型 ******************************/

struct linkg_cellular_tx
{
    pthread_mutex_t             normal_lock;       // VIDEO/DATA控制路径串行锁
    pthread_mutex_t             send_lock;         // 发送锁，保护sendmmsg共享发送缓冲区
    linkg_cellular_tx_queue_t  *video_queue;       // VIDEO短等待队列
    linkg_cellular_tx_queue_t  *data_queue;        // DATA短等待队列
    struct mmsghdr             *messages;          // sendmmsg消息数组
    struct iovec               *iovecs;            // sendmmsg数据数组
    struct sockaddr_in6        *destinations;      // sendmmsg目标IPv6地址数组
    unsigned char              *controls;          // IPV6_PKTINFO控制数据数组
    int                        *socket_fds;        // 借用Cellular Link业务Socket数组
    const uint16_t             *service_ports;     // 借用Cellular Link业务端口数组
    const char                 *interface_name;    // 借用蜂窝出口接口名称
    uint32_t                    capacity;          // sendmmsg scratch容量
    _Atomic bool                started;           // 发送模块运行状态
};

/****************************** 参数校验 ******************************/

/**
 * @brief 校验蜂窝下一跳IPv6 Endpoint是否合法。
 */
static int _linkg_cellular_tx_validate_destination(const linkg_cellular_tx_t *tx, const linkg_path_endpoint_t *destination)
{
    const struct sockaddr_in6 *target;

    if (tx == NULL || destination == NULL)
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

    target = (const struct sockaddr_in6 *)&destination->address;

    if (!linkg_network_ipv6_address_is_global(&target->sin6_addr))
    {
        return -EDESTADDRREQ;
    }

    if (target->sin6_port != htons(tx->service_ports[LINKG_LINK_TX_CLASS_DATA]))
    {
        return -EDESTADDRREQ;
    }

    return 0;
}

/**
 * @brief 校验单个待发送Packet是否合法。
 */
static int _linkg_cellular_tx_validate_packet(const linkg_packet_t *packet)
{
    if (packet == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length == 0U)
    {
        return -EINVAL;
    }

    if (packet->data_length > LINKG_CELLULAR_TX_UDP_PAYLOAD_MAX)
    {
        return -EMSGSIZE;
    }

    return 0;
}

/**
 * @brief 判断底层发送错误是否允许进入等待队列后重试。
 */
static bool _linkg_cellular_tx_error_retryable(int error)
{
    return error == -EAGAIN || error == -EWOULDBLOCK || error == -ENOBUFS;
}

/**
 * @brief 获取VIDEO或DATA对应的等待队列。
 */
static linkg_cellular_tx_queue_t *_linkg_cellular_tx_select_queue(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class)
{
    if (tx_class == LINKG_LINK_TX_CLASS_VIDEO)
    {
        return tx->video_queue;
    }

    if (tx_class == LINKG_LINK_TX_CLASS_DATA)
    {
        return tx->data_queue;
    }

    return NULL;
}

/**
 * @brief 获取指定普通业务等待队列的最大驻留时间。
 */
static uint64_t _linkg_cellular_tx_queue_max_age(linkg_link_tx_class_t tx_class)
{
    if (tx_class == LINKG_LINK_TX_CLASS_VIDEO)
    {
        return LINKG_CELLULAR_TX_VIDEO_MAX_AGE_US;
    }

    return LINKG_CELLULAR_TX_DATA_MAX_AGE_US;
}

/**
 * @brief 获取当前蜂窝出口接口索引。
 */
static int _linkg_cellular_tx_interface_index(const linkg_cellular_tx_t *tx, unsigned int *interface_index)
{
    if (tx == NULL || interface_index == NULL || tx->interface_name == NULL || tx->interface_name[0] == '\0')
    {
        return -EINVAL;
    }

    errno = 0;
    *interface_index = if_nametoindex(tx->interface_name);

    if (*interface_index == 0U)
    {
        return errno != 0 ? -errno : -ENODEV;
    }

    return 0;
}

/****************************** 原子发送组 ******************************/

/**
 * @brief 判断Packet是否为Transport原子发送组首包。
 */
static bool _linkg_cellular_tx_group_first(const linkg_packet_t *packet)
{
    return packet != NULL &&
           (packet->flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U;
}

/**
 * @brief 获取不会在Transport原子发送组首包处截断的最大前缀长度。
 */
static uint32_t _linkg_cellular_tx_complete_prefix(linkg_packet_t *const *packets, uint32_t count, uint32_t limit)
{
    uint32_t prefix;

    if (packets == NULL || count == 0U || limit == 0U)
    {
        return 0U;
    }

    prefix = count;

    if (prefix > limit)
    {
        prefix = limit;
    }

    if (prefix > 0U && _linkg_cellular_tx_group_first(packets[prefix - 1U]))
    {
        prefix--;
    }

    return prefix;
}

/**
 * @brief 获取等待队列中不会截断Transport原子发送组的前缀长度。
 */
static uint32_t _linkg_cellular_tx_complete_queue_prefix(const linkg_cellular_tx_queue_item_t *items, uint32_t count)
{
    return items != NULL && count > 0U &&
           _linkg_cellular_tx_group_first(items[count - 1U].packet) ? count - 1U : count;
}

/****************************** Path统计 ******************************/

/**
 * @brief 记录单个Packet的最终发送结果。
 *
 * @note failed、dropped、expired三类统计互斥，只记录其中一种。
 */
static void _linkg_cellular_tx_record_terminal(linkg_path_t *path, const linkg_packet_t *packet, bool dropped, bool expired)
{
    uint64_t bytes;

    if (path == NULL || packet == NULL)
    {
        return;
    }

    bytes = packet->data_length;

    if (expired)
    {
        linkg_path_record_tx_expired(path, bytes, 1U);
        return;
    }

    if (dropped)
    {
        linkg_path_record_tx_dropped(path, bytes, 1U);
        return;
    }

    linkg_path_record_tx_failed(path, bytes, 1U);
}

/**
 * @brief 批量记录当前同步提交Packet的最终发送结果。
 */
static void _linkg_cellular_tx_record_current(linkg_path_t *path, linkg_packet_t *const *packets, uint32_t count, bool dropped)
{
    uint32_t index;

    if (path == NULL || packets == NULL || count == 0U)
    {
        return;
    }

    for (index = 0U; index < count; index++)
    {
        _linkg_cellular_tx_record_terminal(path, packets[index], dropped, false);
    }
}

/**
 * @brief 批量记录等待队列Packet的最终处理结果。
 *
 * @note lower_layer_failed为true时全部记录为底层最终发送失败；
 *       否则过期元素记录expired，其余元素记录本地主动dropped。
 */
static void _linkg_cellular_tx_record_queue(const linkg_cellular_tx_queue_item_t *items, uint32_t count, uint64_t now_us, uint64_t max_age_us, bool lower_layer_failed)
{
    uint64_t elapsed_us;
    uint32_t index;
    bool expired;

    if (items == NULL || count == 0U)
    {
        return;
    }

    for (index = 0U; index < count; index++)
    {
        if (lower_layer_failed)
        {
            _linkg_cellular_tx_record_terminal(items[index].path, items[index].packet, false, false);
            continue;
        }

        expired = false;

        if (max_age_us > 0U && now_us >= items[index].enqueue_us)
        {
            elapsed_us = now_us - items[index].enqueue_us;
            expired = elapsed_us >= max_age_us;
        }

        _linkg_cellular_tx_record_terminal(items[index].path, items[index].packet, !expired, expired);
    }
}

/****************************** 底层发送 ******************************/

/**
 * @brief 为单个IPv6 UDP消息构造IPV6_PKTINFO控制信息。
 */
static int _linkg_cellular_tx_prepare_pktinfo(struct msghdr *header, void *control, size_t control_size, unsigned int interface_index)
{
    struct in6_pktinfo *pktinfo;
    struct cmsghdr     *cmsg;

    if (header == NULL || control == NULL || control_size < LINKG_CELLULAR_TX_CONTROL_SIZE || interface_index == 0U)
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

/**
 * @brief 使用指定蜂窝业务Socket同步批量发送当前提交Packet。
 *
 * @note Packet和Path均由调用方借用，函数返回后不继续持有。
 */
static int _linkg_cellular_tx_send_current(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, uint32_t *sent_count)
{
    const struct sockaddr_in6 *target;
    unsigned char             *control;
    unsigned int               interface_index;
    uint64_t                   sent_bytes;
    uint32_t                   chunk_count;
    uint32_t                   index;
    uint32_t                   offset;
    int                        socket_fd;
    int                        ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || sent_count == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    if (count > tx->capacity)
    {
        return -EOVERFLOW;
    }

    *sent_count = 0U;

    socket_fd = tx->socket_fds[tx_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    ret = _linkg_cellular_tx_interface_index(tx, &interface_index);
    if (ret != 0)
    {
        return ret;
    }

    target = (const struct sockaddr_in6 *)&destination->address;

    pthread_mutex_lock(&tx->send_lock);

    for (index = 0U; index < count; index++)
    {
        tx->destinations[index] = *target;
        tx->destinations[index].sin6_port = htons(tx->service_ports[tx_class]);

        tx->iovecs[index].iov_base = (void *)linkg_packet_const_data(packets[index]);
        tx->iovecs[index].iov_len  = packets[index]->data_length;

        memset(&tx->messages[index], 0, sizeof(tx->messages[index]));

        tx->messages[index].msg_hdr.msg_name    = &tx->destinations[index];
        tx->messages[index].msg_hdr.msg_namelen = sizeof(tx->destinations[index]);
        tx->messages[index].msg_hdr.msg_iov     = &tx->iovecs[index];
        tx->messages[index].msg_hdr.msg_iovlen  = 1U;

        control = tx->controls + (size_t)index * LINKG_CELLULAR_TX_CONTROL_SIZE;

        ret = _linkg_cellular_tx_prepare_pktinfo(&tx->messages[index].msg_hdr,
                                                  control,
                                                  LINKG_CELLULAR_TX_CONTROL_SIZE,
                                                  interface_index);
        if (ret != 0)
        {
            pthread_mutex_unlock(&tx->send_lock);
            return ret;
        }
    }

    offset = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (tx_class != LINKG_LINK_TX_CLASS_REALTIME && chunk_count > LINKG_CELLULAR_TX_SENDMMSG_CHUNK_MAX)
        {
            chunk_count = LINKG_CELLULAR_TX_SENDMMSG_CHUNK_MAX;
        }

        do
        {
            ret = sendmmsg(socket_fd,
                           &tx->messages[offset],
                           chunk_count,
                           MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        while (ret < 0 && errno == EINTR);

        if (ret < 0)
        {
            if (offset == 0U)
            {
                ret = -errno;
                pthread_mutex_unlock(&tx->send_lock);
                return ret;
            }

            break;
        }

        if ((uint32_t)ret > chunk_count)
        {
            pthread_mutex_unlock(&tx->send_lock);
            return -EIO;
        }

        offset += (uint32_t)ret;

        if ((uint32_t)ret < chunk_count)
        {
            break;
        }

        if (tx_class != LINKG_LINK_TX_CLASS_REALTIME && offset < count)
        {
            sched_yield();
        }
    }

    *sent_count = offset;
    sent_bytes = 0U;

    for (index = 0U; index < *sent_count; index++)
    {
        sent_bytes += packets[index]->data_length;
    }

    pthread_mutex_unlock(&tx->send_lock);

    if (*sent_count > 0U)
    {
        linkg_path_record_tx_success(path, sent_bytes, *sent_count);
    }

    return 0;
}

/**
 * @brief 使用指定蜂窝业务Socket同步批量发送等待队列Packet。
 *
 * @note items中的Packet和Path均借用Queue当前持有的引用。
 */
static int _linkg_cellular_tx_send_pending(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, const linkg_cellular_tx_queue_item_t *items, uint32_t count, uint32_t *sent_count)
{
    const struct sockaddr_in6 *target;
    unsigned char             *control;
    unsigned int               interface_index;
    uint32_t                   chunk_count;
    uint32_t                   index;
    uint32_t                   offset;
    int                        socket_fd;
    int                        ret;

    if (tx == NULL || items == NULL || sent_count == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    if (count > tx->capacity)
    {
        return -EOVERFLOW;
    }

    *sent_count = 0U;

    socket_fd = tx->socket_fds[tx_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    ret = _linkg_cellular_tx_interface_index(tx, &interface_index);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&tx->send_lock);

    for (index = 0U; index < count; index++)
    {
        target = (const struct sockaddr_in6 *)&items[index].destination.address;

        tx->destinations[index] = *target;
        tx->destinations[index].sin6_port = htons(tx->service_ports[tx_class]);

        tx->iovecs[index].iov_base = (void *)linkg_packet_const_data(items[index].packet);
        tx->iovecs[index].iov_len  = items[index].packet->data_length;

        memset(&tx->messages[index], 0, sizeof(tx->messages[index]));

        tx->messages[index].msg_hdr.msg_name    = &tx->destinations[index];
        tx->messages[index].msg_hdr.msg_namelen = sizeof(tx->destinations[index]);
        tx->messages[index].msg_hdr.msg_iov     = &tx->iovecs[index];
        tx->messages[index].msg_hdr.msg_iovlen  = 1U;

        control = tx->controls + (size_t)index * LINKG_CELLULAR_TX_CONTROL_SIZE;

        ret = _linkg_cellular_tx_prepare_pktinfo(&tx->messages[index].msg_hdr,
                                                  control,
                                                  LINKG_CELLULAR_TX_CONTROL_SIZE,
                                                  interface_index);
        if (ret != 0)
        {
            pthread_mutex_unlock(&tx->send_lock);
            return ret;
        }
    }

    offset = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > LINKG_CELLULAR_TX_SENDMMSG_CHUNK_MAX)
        {
            chunk_count = LINKG_CELLULAR_TX_SENDMMSG_CHUNK_MAX;
        }

        do
        {
            ret = sendmmsg(socket_fd,
                           &tx->messages[offset],
                           chunk_count,
                           MSG_DONTWAIT | MSG_NOSIGNAL);
        }
        while (ret < 0 && errno == EINTR);

        if (ret < 0)
        {
            if (offset == 0U)
            {
                ret = -errno;
                pthread_mutex_unlock(&tx->send_lock);
                return ret;
            }

            break;
        }

        if ((uint32_t)ret > chunk_count)
        {
            pthread_mutex_unlock(&tx->send_lock);
            return -EIO;
        }

        offset += (uint32_t)ret;

        if ((uint32_t)ret < chunk_count)
        {
            break;
        }

        if (offset < count)
        {
            sched_yield();
        }
    }

    *sent_count = offset;

    pthread_mutex_unlock(&tx->send_lock);

    for (index = 0U; index < *sent_count; index++)
    {
        linkg_path_record_tx_success(items[index].path, items[index].packet->data_length, 1U);
    }

    return 0;
}

/****************************** 等待队列维护 ******************************/

/**
 * @brief 判断等待队列元素是否已经超过最大驻留时间。
 */
static bool _linkg_cellular_tx_queue_item_expired(const linkg_cellular_tx_queue_item_t *item, uint64_t now_us, uint64_t max_age_us)
{
    if (item == NULL || max_age_us == 0U)
    {
        return false;
    }

    if (now_us < item->enqueue_us)
    {
        return false;
    }

    return now_us - item->enqueue_us >= max_age_us;
}

/**
 * @brief 获取当前等待队列中可以安全连续发送的队首前缀长度。
 *
 * @note 返回前缀不会跨越失效Path、过期元素或Transport原子发送组边界。
 */
static uint32_t _linkg_cellular_tx_queue_sendable_prefix(const linkg_cellular_tx_queue_item_t *items, uint32_t count, uint64_t now_us, uint64_t max_age_us)
{
    uint32_t prefix;
    uint32_t index;

    if (items == NULL || count == 0U)
    {
        return 0U;
    }

    prefix = count;

    for (index = 0U; index < count; index++)
    {
        if (_linkg_cellular_tx_queue_item_expired(&items[index], now_us, max_age_us))
        {
            prefix = index;
            break;
        }

        if (!linkg_path_is_active(items[index].path))
        {
            prefix = index;
            break;
        }
    }

    return _linkg_cellular_tx_complete_queue_prefix(items, prefix);
}

/**
 * @brief 清理等待队首连续的过期或失效Path元素。
 *
 * @note 调用方必须持有normal_lock。
 */
static void _linkg_cellular_tx_purge_queue(linkg_cellular_tx_t *tx, linkg_cellular_tx_queue_t *queue, linkg_link_tx_class_t tx_class, uint64_t now_us)
{
    linkg_cellular_tx_queue_item_t items[LINKG_CELLULAR_TX_NORMAL_SEND_MAX];
    uint64_t                       max_age_us;
    uint32_t                       peek_count;
    uint32_t                       drop_count;
    uint32_t                       index;

    if (tx == NULL || queue == NULL)
    {
        return;
    }

    max_age_us = _linkg_cellular_tx_queue_max_age(tx_class);

    while (linkg_cellular_tx_queue_count(queue) > 0U)
    {
        peek_count = linkg_cellular_tx_queue_peek_batch(queue, items, LINKG_CELLULAR_TX_NORMAL_SEND_MAX);
        if (peek_count == 0U)
        {
            return;
        }

        drop_count = 0U;

        for (index = 0U; index < peek_count; index++)
        {
            if (!_linkg_cellular_tx_queue_item_expired(&items[index], now_us, max_age_us) && linkg_path_is_active(items[index].path))
            {
                break;
            }

            drop_count++;
        }

        if (drop_count == 0U)
        {
            return;
        }

        _linkg_cellular_tx_record_queue(items, drop_count, now_us, max_age_us, false);
        (void)linkg_cellular_tx_queue_discard_batch(queue, drop_count);

        if (drop_count < peek_count)
        {
            return;
        }
    }
}

/**
 * @brief 丢弃等待队列中的全部元素并记录发送失败。
 *
 * @note 调用方必须持有normal_lock。
 */
static void _linkg_cellular_tx_flush_queue(linkg_cellular_tx_t *tx, linkg_cellular_tx_queue_t *queue)
{
    linkg_cellular_tx_queue_item_t items[LINKG_CELLULAR_TX_NORMAL_SEND_MAX];
    uint32_t                       peek_count;

    if (tx == NULL || queue == NULL)
    {
        return;
    }

    while (linkg_cellular_tx_queue_count(queue) > 0U)
    {
        peek_count = linkg_cellular_tx_queue_peek_batch(queue, items, LINKG_CELLULAR_TX_NORMAL_SEND_MAX);
        if (peek_count == 0U)
        {
            return;
        }

        _linkg_cellular_tx_record_queue(items, peek_count, 0U, 0U, false);
        (void)linkg_cellular_tx_queue_discard_batch(queue, peek_count);
    }
}

/**
 * @brief 在当前发送预算内优先发送指定等待队列。
 *
 * @note 调用方必须持有normal_lock。remaining表示本轮剩余VIDEO/DATA发送预算。
 */
static void _linkg_cellular_tx_drain_queue(linkg_cellular_tx_t *tx, linkg_cellular_tx_queue_t *queue, linkg_link_tx_class_t tx_class, uint64_t now_us, uint32_t *remaining, bool *blocked)
{
    linkg_cellular_tx_queue_item_t items[LINKG_CELLULAR_TX_NORMAL_SEND_MAX];
    uint64_t                       max_age_us;
    uint32_t                       peek_capacity;
    uint32_t                       peek_count;
    uint32_t                       send_count;
    uint32_t                       sent_count;
    int                            ret;

    if (tx == NULL || queue == NULL || remaining == NULL || blocked == NULL)
    {
        return;
    }

    if (*remaining == 0U || *blocked)
    {
        return;
    }

    max_age_us = _linkg_cellular_tx_queue_max_age(tx_class);

    while (*remaining > 0U && !*blocked)
    {
        _linkg_cellular_tx_purge_queue(tx, queue, tx_class, now_us);

        peek_capacity = *remaining;

        if (peek_capacity > tx->capacity)
        {
            peek_capacity = tx->capacity;
        }

        if (peek_capacity > LINKG_CELLULAR_TX_NORMAL_SEND_MAX)
        {
            peek_capacity = LINKG_CELLULAR_TX_NORMAL_SEND_MAX;
        }

        peek_count = linkg_cellular_tx_queue_peek_batch(queue, items, peek_capacity);
        if (peek_count == 0U)
        {
            return;
        }

        send_count = _linkg_cellular_tx_queue_sendable_prefix(items, peek_count, now_us, max_age_us);
        if (send_count == 0U)
        {
            *blocked = true;
            return;
        }

        sent_count = 0U;
        ret = _linkg_cellular_tx_send_pending(tx, tx_class, items, send_count, &sent_count);
        if (ret != 0)
        {
            if (_linkg_cellular_tx_error_retryable(ret))
            {
                *blocked = true;
                return;
            }

            _linkg_cellular_tx_record_queue(items, send_count, 0U, 0U, true);
            (void)linkg_cellular_tx_queue_discard_batch(queue, send_count);
            *blocked = true;
            return;
        }

        if (sent_count > 0U)
        {
            (void)linkg_cellular_tx_queue_discard_batch(queue, sent_count);
            *remaining -= sent_count;
        }

        if (sent_count < send_count)
        {
            *blocked = true;
        }
    }
}

/****************************** 当前批次处理 ******************************/

/**
 * @brief 将当前未发送的VIDEO/DATA Packet批量加入等待队列。
 *
 * @note 调用方必须持有normal_lock。成功入队后Queue接管异步发送责任。
 */
static uint32_t _linkg_cellular_tx_enqueue_current(linkg_cellular_tx_queue_t *queue, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, const uint32_t *indices, uint32_t count, uint64_t enqueue_us, int *results)
{
    uint32_t pushed_count;
    uint32_t index;
    int ret;

    if (count == 0U)
    {
        return 0U;
    }

    pushed_count = 0U;
    ret = linkg_cellular_tx_queue_push_batch(queue, packets, count, path, destination, enqueue_us, &pushed_count);
    if (ret != 0)
    {
        for (index = 0U; index < count; index++)
        {
            results[indices[index]] = ret;
        }

        _linkg_cellular_tx_record_current(path, packets, count, true);
        return 0U;
    }

    for (index = 0U; index < pushed_count; index++)
    {
        results[indices[index]] = 0;
    }

    if (pushed_count < count)
    {
        for (index = pushed_count; index < count; index++)
        {
            results[indices[index]] = -ENOBUFS;
        }

        _linkg_cellular_tx_record_current(path, &packets[pushed_count], count - pushed_count, true);
    }

    return pushed_count;
}

/**
 * @brief 在当前发送预算内处理本次VIDEO/DATA同步提交。
 *
 * @note 调用方必须持有normal_lock。无法立即发送的Packet优先进入对应等待队列。
 */
static uint32_t _linkg_cellular_tx_send_current_normal(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, linkg_cellular_tx_queue_t *queue, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, const uint32_t *indices, uint32_t count, uint32_t *remaining, uint64_t now_us, int *results, bool *blocked)
{
    uint32_t attempt_count;
    uint32_t queued_count;
    uint32_t sent_count;
    uint32_t accepted_count;
    uint32_t index;
    int ret;

    if (count == 0U)
    {
        return 0U;
    }

    if (*remaining == 0U || *blocked)
    {
        return _linkg_cellular_tx_enqueue_current(queue, path, destination, packets, indices, count, now_us, results);
    }

    attempt_count = count;

    if (attempt_count > *remaining)
    {
        attempt_count = *remaining;
    }

    attempt_count = _linkg_cellular_tx_complete_prefix(packets, count, attempt_count);
    if (attempt_count == 0U)
    {
        return _linkg_cellular_tx_enqueue_current(queue, path, destination, packets, indices, count, now_us, results);
    }

    sent_count = 0U;
    ret = _linkg_cellular_tx_send_current(tx, tx_class, path, destination, packets, attempt_count, &sent_count);

    if (ret != 0)
    {
        if (_linkg_cellular_tx_error_retryable(ret))
        {
            *blocked = true;
            return _linkg_cellular_tx_enqueue_current(queue, path, destination, packets, indices, count, now_us, results);
        }

        for (index = 0U; index < attempt_count; index++)
        {
            results[indices[index]] = ret;
        }

        _linkg_cellular_tx_record_current(path, packets, attempt_count, false);
        accepted_count = 0U;

        if (attempt_count < count)
        {
            queued_count = _linkg_cellular_tx_enqueue_current(queue,
                                                               path,
                                                               destination,
                                                               &packets[attempt_count],
                                                               &indices[attempt_count],
                                                               count - attempt_count,
                                                               now_us,
                                                               results);
            accepted_count += queued_count;
        }

        *blocked = true;
        return accepted_count;
    }

    accepted_count = sent_count;

    for (index = 0U; index < sent_count; index++)
    {
        results[indices[index]] = 0;
    }

    *remaining -= sent_count;

    if (sent_count < attempt_count)
    {
        queued_count = _linkg_cellular_tx_enqueue_current(queue,
                                                           path,
                                                           destination,
                                                           &packets[sent_count],
                                                           &indices[sent_count],
                                                           count - sent_count,
                                                           now_us,
                                                           results);
        accepted_count += queued_count;
        *blocked = true;
        return accepted_count;
    }

    if (attempt_count < count)
    {
        queued_count = _linkg_cellular_tx_enqueue_current(queue,
                                                           path,
                                                           destination,
                                                           &packets[attempt_count],
                                                           &indices[attempt_count],
                                                           count - attempt_count,
                                                           now_us,
                                                           results);
        accepted_count += queued_count;
    }

    return accepted_count;
}

/****************************** REALTIME发送 ******************************/

/**
 * @brief 同步提交REALTIME Packet并直接尝试发送。
 *
 * @note REALTIME不进入等待队列，底层无法立即接受的Packet直接返回失败。
 */
static int _linkg_cellular_tx_submit_realtime(linkg_cellular_tx_t *tx, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_packet_t *valid_packets[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t        valid_indices[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t        valid_count;
    uint32_t        sent_count;
    uint32_t        index;
    int             ret;

    valid_count = 0U;

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;

        ret = _linkg_cellular_tx_validate_packet(packets[index]);
        if (ret != 0)
        {
            results[index] = ret;
            continue;
        }

        valid_packets[valid_count] = packets[index];
        valid_indices[valid_count] = index;
        valid_count++;
    }

    if (valid_count == 0U)
    {
        return 0;
    }

    if (!linkg_path_is_active(path))
    {
        for (index = 0U; index < valid_count; index++)
        {
            results[valid_indices[index]] = -ENODEV;
        }

        _linkg_cellular_tx_record_current(path, valid_packets, valid_count, true);
        return 0;
    }

    sent_count = 0U;
    ret = _linkg_cellular_tx_send_current(tx,
                                           LINKG_LINK_TX_CLASS_REALTIME,
                                           path,
                                           destination,
                                           valid_packets,
                                           valid_count,
                                           &sent_count);
    if (ret != 0)
    {
        for (index = 0U; index < valid_count; index++)
        {
            results[valid_indices[index]] = ret;
        }

        _linkg_cellular_tx_record_current(path, valid_packets, valid_count, false);
        return 0;
    }

    for (index = 0U; index < sent_count; index++)
    {
        results[valid_indices[index]] = 0;
    }

    for (index = sent_count; index < valid_count; index++)
    {
        results[valid_indices[index]] = -EAGAIN;
    }

    if (sent_count < valid_count)
    {
        _linkg_cellular_tx_record_current(path, &valid_packets[sent_count], valid_count - sent_count, false);
    }

    return (int)sent_count;
}

/****************************** VIDEO/DATA发送 ******************************/

/**
 * @brief 提交VIDEO或DATA Packet并使用短等待队列吸收瞬时发送阻塞。
 */
static int _linkg_cellular_tx_submit_normal(linkg_cellular_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_packet_t            *valid_packets[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t                   valid_indices[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    linkg_cellular_tx_queue_t *current_queue;
    uint64_t                   now_us;
    uint32_t                   accepted_count;
    uint32_t                   valid_count;
    uint32_t                   remaining;
    uint32_t                   index;
    bool                       blocked;
    int                        ret;

    valid_count = 0U;

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;

        ret = _linkg_cellular_tx_validate_packet(packets[index]);
        if (ret != 0)
        {
            results[index] = ret;
            continue;
        }

        valid_packets[valid_count] = packets[index];
        valid_indices[valid_count] = index;
        valid_count++;
    }

    if (valid_count == 0U)
    {
        return 0;
    }

    current_queue = _linkg_cellular_tx_select_queue(tx, tx_class);
    if (current_queue == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&tx->normal_lock);

    if (!atomic_load(&tx->started))
    {
        pthread_mutex_unlock(&tx->normal_lock);
        return -ENODEV;
    }

    if (!linkg_path_is_active(path))
    {
        for (index = 0U; index < valid_count; index++)
        {
            results[valid_indices[index]] = -ENODEV;
        }

        _linkg_cellular_tx_record_current(path, valid_packets, valid_count, true);
        pthread_mutex_unlock(&tx->normal_lock);
        return 0;
    }

    now_us = linkg_time_monotonic_us();

    _linkg_cellular_tx_purge_queue(tx, tx->video_queue, LINKG_LINK_TX_CLASS_VIDEO, now_us);
    _linkg_cellular_tx_purge_queue(tx, tx->data_queue, LINKG_LINK_TX_CLASS_DATA, now_us);

    remaining      = LINKG_CELLULAR_TX_NORMAL_SEND_MAX;
    blocked        = false;
    accepted_count = 0U;

    _linkg_cellular_tx_drain_queue(tx,
                                    tx->video_queue,
                                    LINKG_LINK_TX_CLASS_VIDEO,
                                    now_us,
                                    &remaining,
                                    &blocked);

    if (tx_class == LINKG_LINK_TX_CLASS_VIDEO)
    {
        accepted_count += _linkg_cellular_tx_send_current_normal(tx,
                                                                  tx_class,
                                                                  current_queue,
                                                                  path,
                                                                  destination,
                                                                  valid_packets,
                                                                  valid_indices,
                                                                  valid_count,
                                                                  &remaining,
                                                                  now_us,
                                                                  results,
                                                                  &blocked);

        if (!blocked && remaining > 0U)
        {
            _linkg_cellular_tx_drain_queue(tx,
                                            tx->data_queue,
                                            LINKG_LINK_TX_CLASS_DATA,
                                            now_us,
                                            &remaining,
                                            &blocked);
        }
    }
    else
    {
        if (!blocked && remaining > 0U)
        {
            _linkg_cellular_tx_drain_queue(tx,
                                            tx->data_queue,
                                            LINKG_LINK_TX_CLASS_DATA,
                                            now_us,
                                            &remaining,
                                            &blocked);
        }

        accepted_count += _linkg_cellular_tx_send_current_normal(tx,
                                                                  tx_class,
                                                                  current_queue,
                                                                  path,
                                                                  destination,
                                                                  valid_packets,
                                                                  valid_indices,
                                                                  valid_count,
                                                                  &remaining,
                                                                  now_us,
                                                                  results,
                                                                  &blocked);
    }

    pthread_mutex_unlock(&tx->normal_lock);

    return (int)accepted_count;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝链路发送模块。
 *
 * @note socket_fds、service_ports和interface_name均为借用引用，生命周期由Cellular Link保证。
 */
linkg_cellular_tx_t *linkg_cellular_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports, const char *interface_name)
{
    linkg_cellular_tx_t *tx;
    int ret;

    if (capacity == 0U || socket_fds == NULL || service_ports == NULL || interface_name == NULL || interface_name[0] == '\0')
    {
        return NULL;
    }

    tx = calloc(1, sizeof(*tx));
    if (tx == NULL)
    {
        return NULL;
    }

    tx->capacity       = capacity;
    tx->socket_fds     = socket_fds;
    tx->service_ports  = service_ports;
    tx->interface_name = interface_name;

    atomic_store(&tx->started, false);

    ret = pthread_mutex_init(&tx->normal_lock, NULL);
    if (ret != 0)
    {
        goto fail_tx;
    }

    ret = pthread_mutex_init(&tx->send_lock, NULL);
    if (ret != 0)
    {
        goto fail_normal_lock;
    }

    tx->video_queue = linkg_cellular_tx_queue_create(LINKG_CELLULAR_TX_VIDEO_QUEUE_CAPACITY);
    if (tx->video_queue == NULL)
    {
        goto fail_send_lock;
    }

    tx->data_queue = linkg_cellular_tx_queue_create(LINKG_CELLULAR_TX_DATA_QUEUE_CAPACITY);
    if (tx->data_queue == NULL)
    {
        goto fail_video_queue;
    }

    tx->messages = calloc(capacity, sizeof(*tx->messages));
    if (tx->messages == NULL)
    {
        goto fail_data_queue;
    }

    tx->iovecs = calloc(capacity, sizeof(*tx->iovecs));
    if (tx->iovecs == NULL)
    {
        goto fail_messages;
    }

    tx->destinations = calloc(capacity, sizeof(*tx->destinations));
    if (tx->destinations == NULL)
    {
        goto fail_iovecs;
    }

    tx->controls = calloc(capacity, LINKG_CELLULAR_TX_CONTROL_SIZE);
    if (tx->controls == NULL)
    {
        goto fail_destinations;
    }

    return tx;

fail_destinations:
    free(tx->destinations);

fail_iovecs:
    free(tx->iovecs);

fail_messages:
    free(tx->messages);

fail_data_queue:
    linkg_cellular_tx_queue_destroy(tx->data_queue);

fail_video_queue:
    linkg_cellular_tx_queue_destroy(tx->video_queue);

fail_send_lock:
    pthread_mutex_destroy(&tx->send_lock);

fail_normal_lock:
    pthread_mutex_destroy(&tx->normal_lock);

fail_tx:
    free(tx);
    return NULL;
}

/**
 * @brief 销毁蜂窝链路发送模块。
 *
 * @note 调用前不得再有其他线程访问tx。
 */
void linkg_cellular_tx_destroy(linkg_cellular_tx_t *tx)
{
    if (tx == NULL)
    {
        return;
    }

    linkg_cellular_tx_stop(tx);

    free(tx->controls);
    free(tx->destinations);
    free(tx->iovecs);
    free(tx->messages);

    linkg_cellular_tx_queue_destroy(tx->data_queue);
    linkg_cellular_tx_queue_destroy(tx->video_queue);

    pthread_mutex_destroy(&tx->send_lock);
    pthread_mutex_destroy(&tx->normal_lock);

    free(tx);
}

/**
 * @brief 启动蜂窝链路发送模块。
 */
int linkg_cellular_tx_start(linkg_cellular_tx_t *tx)
{
    uint32_t index;

    if (tx == NULL)
    {
        return -EINVAL;
    }

    if (atomic_load(&tx->started))
    {
        return -EALREADY;
    }

    for (index = 0U; index < LINKG_LINK_TX_CLASS_COUNT; index++)
    {
        if (tx->socket_fds[index] < 0)
        {
            return -ENODEV;
        }
    }

    atomic_store(&tx->started, true);

    return 0;
}

/**
 * @brief 停止蜂窝链路发送模块并清空VIDEO/DATA等待队列。
 */
void linkg_cellular_tx_stop(linkg_cellular_tx_t *tx)
{
    if (tx == NULL)
    {
        return;
    }

    if (!atomic_exchange(&tx->started, false))
    {
        return;
    }

    pthread_mutex_lock(&tx->normal_lock);

    _linkg_cellular_tx_flush_queue(tx, tx->video_queue);
    _linkg_cellular_tx_flush_queue(tx, tx->data_queue);

    pthread_mutex_unlock(&tx->normal_lock);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 提交同一业务类别的蜂窝链路发送请求。
 *
 * @return <0表示整个batch未进入正常提交流程，>=0表示Cellular TX接受发送责任的Packet数量。
 *
 * @note results[index]为0表示Cellular TX已经接受该Packet发送责任，可能已经真实发送，
 *       也可能已经进入VIDEO/DATA等待队列；负数表示本次拒绝该Packet。
 */
int linkg_cellular_tx_submit(linkg_cellular_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    int ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > tx->capacity || count > LINKG_LINK_TX_BATCH_SIZE_DEFAULT)
    {
        return -EOVERFLOW;
    }

    if (!atomic_load(&tx->started))
    {
        return -ENODEV;
    }

    if ((int)tx_class < 0 || tx_class >= LINKG_LINK_TX_CLASS_COUNT)
    {
        return -EINVAL;
    }

    if (tx->socket_fds[tx_class] < 0)
    {
        return -ENODEV;
    }

    ret = _linkg_cellular_tx_validate_destination(tx, destination);
    if (ret != 0)
    {
        return ret;
    }

    if (tx_class == LINKG_LINK_TX_CLASS_REALTIME)
    {
        return _linkg_cellular_tx_submit_realtime(tx, path, destination, packets, count, results);
    }

    return _linkg_cellular_tx_submit_normal(tx, path, tx_class, destination, packets, count, results);
}
