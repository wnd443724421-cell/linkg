/**
 * @file cellular_tx.c
 * @brief LinkG蜂窝链路发送模块实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#define _GNU_SOURCE

#include "cellular_tx.h"

#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "linkg_time.h"

#include "cellular_tx_queue.h"

/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_TX_BATCH_SIZE_MAX      32U                                        // 单次发送调度最大Packet数量
#define LINKG_CELLULAR_TX_QUEUE_BATCH_COUNT   5U                                         // 单业务等待队列最多缓存批次数
#define LINKG_CELLULAR_TX_CONTROL_SIZE        CMSG_SPACE(sizeof(struct in6_pktinfo))     // 单个IPv6 PKTINFO控制区大小

/****************************** 内部类型 ******************************/

/**
 * @brief 单个蜂窝业务类别的预分配发送Scratch。
 */
typedef struct
{
    linkg_cellular_tx_queue_item_t items[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                           // 当前发送Queue元素快照
    struct mmsghdr                 messages[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                        // sendmmsg消息数组
    struct iovec                   iovecs[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                          // iovec数组
    struct sockaddr_in6            destinations[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                    // UDP目标IPv6地址数组
    unsigned char                  controls[LINKG_CELLULAR_TX_BATCH_SIZE_MAX][LINKG_CELLULAR_TX_CONTROL_SIZE]; // IPv6 PKTINFO控制区
} linkg_cellular_tx_scratch_t;

/**
 * @brief 单次蜂窝发送调度计划。
 */
typedef struct
{
    uint32_t class_count[LINKG_LINK_TX_CLASS_COUNT]; // 各业务类别本轮计划提交数量
} linkg_cellular_tx_plan_t;

/**
 * @brief 蜂窝发送模块运行上下文。
 */
struct linkg_cellular_tx
{
    pthread_mutex_t             lock;                              // 三业务队列和发送调度串行锁
    linkg_cellular_tx_scratch_t scratch[LINKG_LINK_TX_CLASS_COUNT]; // 三业务预分配Scratch
    linkg_cellular_tx_queue_t  *queues[LINKG_LINK_TX_CLASS_COUNT]; // 三业务等待FIFO

    int                        *socket_fds;                        // 借用Cellular Link业务Socket数组
    const uint16_t             *service_ports;                     // 借用Cellular Link业务端口数组
    const char                 *interface_name;                    // 借用蜂窝出口接口名称

    uint32_t                    capacity;                          // 单次发送调度最大提交Packet数量
    uint32_t                    queue_capacity;                    // 单业务等待队列最大Packet数量

    _Atomic bool                started;                           // 发送模块运行状态
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验蜂窝发送业务类别。
 */
static bool _linkg_cellular_tx_class_valid(linkg_link_tx_class_t tx_class)
{
    return (int)tx_class >= 0 && tx_class < LINKG_LINK_TX_CLASS_COUNT;
}

/**
 * @brief 获取当前蜂窝出口接口索引。
 */
static int _linkg_cellular_tx_interface_index(const linkg_cellular_tx_t *tx, unsigned int *interface_index)
{
    if (tx == NULL || interface_index == NULL)
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

/****************************** Path统计 ******************************/

/**
 * @brief 记录单个Packet为主动丢弃。
 */
static void _linkg_cellular_tx_record_dropped(linkg_path_t *path, const linkg_packet_t *packet)
{
    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_dropped(path, packet->data_length, 1U);
}

/**
 * @brief 记录单个Packet发送失败。
 */
static void _linkg_cellular_tx_record_failed(linkg_path_t *path, const linkg_packet_t *packet)
{
    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_failed(path, packet->data_length, 1U);
}

/****************************** Queue辅助 ******************************/

/**
 * @brief 从指定业务队列丢弃最旧Packet，完整两片TX Group尽量一起丢弃。
 *
 * Transport保证单个TX Group最多包含两个连续Packet。
 * 调用方必须持有tx->lock。
 */
static uint32_t _linkg_cellular_tx_drop_oldest_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class)
{
    linkg_cellular_tx_queue_item_t items[2];
    linkg_cellular_tx_queue_t     *queue;
    uint32_t                       first_flags;
    uint32_t                       second_flags;
    uint32_t                       peek_count;
    uint32_t                       drop_count;
    uint32_t                       index;
    int                            ret;

    queue = tx->queues[tx_class];
    if (queue == NULL)
    {
        return 0U;
    }

    peek_count = linkg_cellular_tx_queue_peek_batch(queue, items, 2U);
    if (peek_count == 0U)
    {
        return 0U;
    }

    drop_count  = 1U;
    first_flags = items[0].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;

    // 完整两片TX Group仍在Queue中时一起丢弃。
    if ((first_flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U && peek_count == 2U)
    {
        second_flags = items[1].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;

        if ((second_flags & LINKG_PACKET_FLAG_TX_GROUP_LAST) != 0U)
        {
            drop_count = 2U;
        }
    }

    // Queue丢弃会释放Packet和Path引用，因此必须提前记录丢包统计。
    for (index = 0U; index < drop_count; index++)
    {
        _linkg_cellular_tx_record_dropped(items[index].path, items[index].packet);
    }

    ret = linkg_cellular_tx_queue_discard_batch(queue, drop_count);
    if (ret != 0)
    {
        return 0U;
    }

    return drop_count;
}

/**
 * @brief 为当前新批次腾出足够Queue容量。
 *
 * 调用方必须持有tx->lock。
 */
static int _linkg_cellular_tx_make_room_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, uint32_t required_count)
{
    linkg_cellular_tx_queue_t *queue;
    uint32_t                   dropped;

    queue = tx->queues[tx_class];
    if (queue == NULL)
    {
        return -ENODEV;
    }

    if (required_count > linkg_cellular_tx_queue_capacity(queue))
    {
        return -ENOSPC;
    }

    while (linkg_cellular_tx_queue_available(queue) < required_count)
    {
        dropped = _linkg_cellular_tx_drop_oldest_locked(tx, tx_class);
        if (dropped == 0U)
        {
            return -ENOSPC;
        }
    }

    return 0;
}

/**
 * @brief 将当前完整批次加入对应业务等待队列。
 *
 * 调用方必须持有tx->lock。
 */
static int _linkg_cellular_tx_enqueue_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count)
{
    uint32_t pushed_count;
    int      ret;

    ret = _linkg_cellular_tx_make_room_locked(tx, tx_class, count);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_cellular_tx_queue_push_batch(tx->queues[tx_class], packets, count, path, destination, linkg_time_monotonic_us(), &pushed_count);
    if (ret != 0)
    {
        return ret;
    }

    if (pushed_count != count)
    {
        return -ENOSPC;
    }

    return 0;
}

/****************************** 调度计划 ******************************/

/**
 * @brief 根据本次触发业务类别构建一次最多capacity个Packet的发送计划。
 *
 * REALTIME触发只服务REALTIME；VIDEO触发服务REALTIME和VIDEO；
 * DATA触发按REALTIME、VIDEO、DATA顺序消费剩余budget。
 */
static void _linkg_cellular_tx_build_plan_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t trigger_class, linkg_cellular_tx_plan_t *plan)
{
    static const linkg_link_tx_class_t priority_order[LINKG_LINK_TX_CLASS_COUNT] =
    {
        LINKG_LINK_TX_CLASS_REALTIME,
        LINKG_LINK_TX_CLASS_VIDEO,
        LINKG_LINK_TX_CLASS_DATA
    };

    uint32_t class_limit;
    uint32_t remaining;
    uint32_t available;
    uint32_t take_count;
    uint32_t order_index;
    linkg_link_tx_class_t tx_class;

    memset(plan, 0, sizeof(*plan));

    switch (trigger_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            class_limit = 1U;
            break;

        case LINKG_LINK_TX_CLASS_VIDEO:
            class_limit = 2U;
            break;

        case LINKG_LINK_TX_CLASS_DATA:
            class_limit = 3U;
            break;

        default:
            return;
    }

    remaining = tx->capacity;

    for (order_index = 0U; order_index < class_limit && remaining > 0U; order_index++)
    {
        tx_class   = priority_order[order_index];
        available  = linkg_cellular_tx_queue_count(tx->queues[tx_class]);
        take_count = available;

        if (take_count > remaining)
        {
            take_count = remaining;
        }

        plan->class_count[tx_class] = take_count;
        remaining                  -= take_count;
    }
}

/****************************** Socket发送 ******************************/

/**
 * @brief 判断sendmmsg错误是否属于短时背压。
 */
static bool _linkg_cellular_tx_error_transient(int error)
{
    return error == -EAGAIN || error == -ENOBUFS;
}

/**
 * @brief 使用预分配Scratch对当前Queue元素执行一次非阻塞sendmmsg。
 *
 * 不执行Queue管理、不负责重试或丢包，调用方必须持有tx->lock。
 */
static int _linkg_cellular_tx_send_once_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, linkg_cellular_tx_queue_item_t *items, uint32_t count)
{
    linkg_cellular_tx_scratch_t *scratch;
    const struct sockaddr_in6   *target;
    unsigned int                 interface_index;
    uint32_t                     index;
    int                          socket_fd;
    int                          ret;

    if (tx == NULL || items == NULL || count == 0U || count > tx->capacity || count > LINKG_CELLULAR_TX_BATCH_SIZE_MAX)
    {
        return -EINVAL;
    }

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

    scratch = &tx->scratch[tx_class];

    memset(scratch->messages, 0, count * sizeof(scratch->messages[0]));

    for (index = 0U; index < count; index++)
    {
        target = (const struct sockaddr_in6 *)&items[index].destination.address;

        scratch->destinations[index]           = *target;
        scratch->destinations[index].sin6_port = htons(tx->service_ports[tx_class]);

        scratch->iovecs[index].iov_base = (void *)linkg_packet_const_data(items[index].packet);
        scratch->iovecs[index].iov_len  = items[index].packet->data_length;

        scratch->messages[index].msg_hdr.msg_name    = &scratch->destinations[index];
        scratch->messages[index].msg_hdr.msg_namelen = sizeof(scratch->destinations[index]);
        scratch->messages[index].msg_hdr.msg_iov     = &scratch->iovecs[index];
        scratch->messages[index].msg_hdr.msg_iovlen  = 1U;

        ret = _linkg_cellular_tx_prepare_pktinfo(&scratch->messages[index].msg_hdr, scratch->controls[index], sizeof(scratch->controls[index]), interface_index);
        if (ret != 0)
        {
            return ret;
        }
    }

    do
    {
        ret = sendmmsg(socket_fd, scratch->messages, count, MSG_DONTWAIT | MSG_NOSIGNAL);
    }
    while (ret < 0 && errno == EINTR);

    if (ret < 0)
    {
        return -errno;
    }

    if ((uint32_t)ret > count)
    {
        return -EIO;
    }

    return ret;
}

/****************************** QoS发送 ******************************/

/**
 * @brief 从指定业务队列提交当前计划数量的数据包。
 *
 * stopped为true表示本轮出现partial或发送压力，
 * 后续低优先级业务不得继续提交。调用方必须持有tx->lock。
 */
static int _linkg_cellular_tx_dispatch_class_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, uint32_t planned_count, bool *stopped)
{
    linkg_cellular_tx_scratch_t *scratch;
    linkg_cellular_tx_queue_t   *queue;
    uint32_t                     submitted_total;
    uint32_t                     remaining;
    uint32_t                     peek_count;
    uint32_t                     send_count;
    uint32_t                     sent_count;
    uint32_t                     index;
    int                          ret;

    if (stopped == NULL)
    {
        return -EINVAL;
    }

    *stopped = false;

    if (planned_count == 0U)
    {
        return 0;
    }

    scratch = &tx->scratch[tx_class];
    queue   = tx->queues[tx_class];

    submitted_total = 0U;
    remaining       = planned_count;

    while (remaining > 0U)
    {
        peek_count = linkg_cellular_tx_queue_peek_batch(queue, scratch->items, remaining);
        if (peek_count == 0U)
        {
            break;
        }

        send_count = 0U;

        for (index = 0U; index < peek_count; index++)
        {
            if (!linkg_path_is_active(scratch->items[index].path))
            {
                break;
            }

            send_count++;
        }

        // 队首Path失效时主动丢弃，不占用本轮发送budget。
        if (send_count == 0U)
        {
            _linkg_cellular_tx_record_dropped(scratch->items[0].path, scratch->items[0].packet);

            ret = linkg_cellular_tx_queue_discard_batch(queue, 1U);
            if (ret != 0)
            {
                *stopped = true;
                return ret;
            }

            continue;
        }

        ret = _linkg_cellular_tx_send_once_locked(tx, tx_class, scratch->items, send_count);
        if (ret < 0)
        {
            int send_error;

            send_error = ret;

            if (_linkg_cellular_tx_error_transient(send_error))
            {
                *stopped = true;
                return (int)submitted_total;
            }

            LINKG_LOG_WARN("Cellular TX sendmmsg failed, class=%u, packets=%u, error=%d", (unsigned int)tx_class, send_count, send_error);

            for (index = 0U; index < send_count; index++)
            {
                _linkg_cellular_tx_record_failed(scratch->items[index].path, scratch->items[index].packet);
            }

            ret = linkg_cellular_tx_queue_discard_batch(queue, send_count);
            if (ret != 0)
            {
                *stopped = true;
                return ret;
            }

            *stopped = true;
            return send_error;
        }

        sent_count = (uint32_t)ret;

        if (sent_count > send_count)
        {
            *stopped = true;
            return -EIO;
        }

        // Queue释放Packet和Path引用前先完成成功发送统计。
        for (index = 0U; index < sent_count; index++)
        {
            linkg_path_record_tx_success(scratch->items[index].path, scratch->items[index].packet->data_length, 1U);
        }

        if (sent_count > 0U)
        {
            ret = linkg_cellular_tx_queue_discard_batch(queue, sent_count);
            if (ret != 0)
            {
                *stopped = true;
                return ret;
            }

            remaining       -= sent_count;
            submitted_total += sent_count;
        }

        if (sent_count < send_count)
        {
                *stopped = true;
            break;
        }
    }

    return (int)submitted_total;
}

/**
 * @brief 按触发业务类别执行一次蜂窝QoS发送调度。
 *
 * 本轮所有业务合计最多提交tx->capacity个Packet。
 */
static int _linkg_cellular_tx_dispatch_once_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t trigger_class)
{
    static const linkg_link_tx_class_t priority_order[LINKG_LINK_TX_CLASS_COUNT] =
    {
        LINKG_LINK_TX_CLASS_REALTIME,
        LINKG_LINK_TX_CLASS_VIDEO,
        LINKG_LINK_TX_CLASS_DATA
    };

    linkg_cellular_tx_plan_t plan;
    uint32_t                 class_limit;
    uint32_t                 submitted_total;
    uint32_t                 order_index;
    linkg_link_tx_class_t    tx_class;
    bool                     stopped;
    int                      ret;

    switch (trigger_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            class_limit = 1U;
            break;

        case LINKG_LINK_TX_CLASS_VIDEO:
            class_limit = 2U;
            break;

        case LINKG_LINK_TX_CLASS_DATA:
            class_limit = 3U;
            break;

        default:
            return -EINVAL;
    }

    _linkg_cellular_tx_build_plan_locked(tx, trigger_class, &plan);

    submitted_total = 0U;

    for (order_index = 0U; order_index < class_limit; order_index++)
    {
        tx_class = priority_order[order_index];

        if (plan.class_count[tx_class] == 0U)
        {
            continue;
        }

        stopped = false;

        ret = _linkg_cellular_tx_dispatch_class_locked(tx, tx_class, plan.class_count[tx_class], &stopped);
        if (ret < 0)
        {
            return ret;
        }

        submitted_total += (uint32_t)ret;

        if (stopped)
        {
            break;
        }
    }

    return (int)submitted_total;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝链路发送模块。
 *
 * socket_fds、service_ports和interface_name仅借用，生命周期由Cellular Link保证。
 */
linkg_cellular_tx_t *linkg_cellular_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports, const char *interface_name)
{
    linkg_cellular_tx_t *tx;
    uint32_t             class_index;
    int                  ret;

    if (capacity == 0U || capacity > LINKG_CELLULAR_TX_BATCH_SIZE_MAX || socket_fds == NULL || service_ports == NULL || interface_name == NULL || interface_name[0] == '\0')
    {
        return NULL;
    }

    tx = calloc(1, sizeof(*tx));
    if (tx == NULL)
    {
        return NULL;
    }

    tx->socket_fds     = socket_fds;
    tx->service_ports  = service_ports;
    tx->interface_name = interface_name;
    tx->capacity       = capacity;
    tx->queue_capacity = capacity * LINKG_CELLULAR_TX_QUEUE_BATCH_COUNT;

    atomic_store(&tx->started, false);

    ret = pthread_mutex_init(&tx->lock, NULL);
    if (ret != 0)
    {
        goto fail_tx;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        tx->queues[class_index] = linkg_cellular_tx_queue_create(tx->queue_capacity);
        if (tx->queues[class_index] == NULL)
        {
            goto fail_queues;
        }
    }

    return tx;

fail_queues:
    while (class_index > 0U)
    {
        class_index--;

        linkg_cellular_tx_queue_destroy(tx->queues[class_index]);
        tx->queues[class_index] = NULL;
    }

    pthread_mutex_destroy(&tx->lock);

fail_tx:
    free(tx);

    return NULL;
}

/**
 * @brief 销毁蜂窝链路发送模块。
 *
 * 调用前不能再有其他线程访问tx。
 */
void linkg_cellular_tx_destroy(linkg_cellular_tx_t *tx)
{
    uint32_t class_index;

    if (tx == NULL)
    {
        return;
    }

    linkg_cellular_tx_stop(tx);

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        linkg_cellular_tx_queue_destroy(tx->queues[class_index]);
        tx->queues[class_index] = NULL;
    }

    pthread_mutex_destroy(&tx->lock);

    free(tx);
}

/**
 * @brief 启动蜂窝链路发送模块。
 */
int linkg_cellular_tx_start(linkg_cellular_tx_t *tx)
{
    uint32_t class_index;
    int      ret;

    if (tx == NULL)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&tx->lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (atomic_load(&tx->started))
    {
        pthread_mutex_unlock(&tx->lock);
        return -EALREADY;
    }

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        if (tx->socket_fds[class_index] < 0)
        {
            pthread_mutex_unlock(&tx->lock);
            return -ENODEV;
        }
    }

    atomic_store(&tx->started, true);

    pthread_mutex_unlock(&tx->lock);

    return 0;
}

/**
 * @brief 停止蜂窝链路发送模块并清空全部业务等待队列。
 */
void linkg_cellular_tx_stop(linkg_cellular_tx_t *tx)
{
    uint32_t class_index;
    int      ret;

    if (tx == NULL)
    {
        return;
    }

    ret = pthread_mutex_lock(&tx->lock);
    if (ret != 0)
    {
        return;
    }

    if (!atomic_load(&tx->started))
    {
        pthread_mutex_unlock(&tx->lock);
        return;
    }

    atomic_store(&tx->started, false);

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        linkg_cellular_tx_queue_clear(tx->queues[class_index]);
    }

    pthread_mutex_unlock(&tx->lock);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 提交同一业务类别、同一Path和同一下一跳Endpoint的数据包批次。
 *
 * 所有Packet统一先进入对应REALTIME/VIDEO/DATA等待队列；队列容量不足时主动丢弃
 * 最旧完整TX Group或普通Packet，为当前新批次腾出空间。成功入队即表示Cellular TX
 * 接受对应Packet的发送责任，随后执行一次最多capacity个Packet的QoS发送。
 */
int linkg_cellular_tx_submit(linkg_cellular_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    uint32_t index;
    int      accepted_count;
    int      ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_cellular_tx_class_valid(tx_class))
    {
        return -EINVAL;
    }

    if (count > tx->capacity)
    {
        return -EOVERFLOW;
    }

    if (!atomic_load(&tx->started))
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&tx->lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (!atomic_load(&tx->started))
    {
        pthread_mutex_unlock(&tx->lock);
        return -ENODEV;
    }

    if (!linkg_path_is_active(path))
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = -ENODEV;
            _linkg_cellular_tx_record_dropped(path, packets[index]);
        }

        pthread_mutex_unlock(&tx->lock);
        return 0;
    }

    ret = _linkg_cellular_tx_enqueue_locked(tx, tx_class, path, destination, packets, count);
    if (ret != 0)
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = ret;
        }

        pthread_mutex_unlock(&tx->lock);
        return 0;
    }

    for (index = 0U; index < count; index++)
    {
        results[index] = 0;
    }

    accepted_count = (int)count;

    ret = _linkg_cellular_tx_dispatch_once_locked(tx, tx_class);
    if (ret < 0)
    {
        LINKG_LOG_WARN("Cellular TX queue dispatch failed, trigger=%u, error=%d", (unsigned int)tx_class, ret);
    }

    pthread_mutex_unlock(&tx->lock);

    return accepted_count;
}
