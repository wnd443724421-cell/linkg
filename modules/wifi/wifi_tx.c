/**
 * @file wifi_tx.c
 * @brief LinkG Wi-Fi发送模块实现
 * @author Dawn
 * @version 1.4.0
 * @date 2026-09-09
 */

#define _GNU_SOURCE

#include "wifi_tx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <pthread.h>
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
#include "wifi_flowctrl.h"
#include "wifi_internal.h"
#include "wifi_traffic.h"
#include "wifi_tx_queue.h"

/****************************** 发送参数 ******************************/

#define LINKG_WIFI_TX_IPV4_HEADER_SIZE     20U                                                                                  // IPv4最小头部长度
#define LINKG_WIFI_TX_UDP_HEADER_SIZE      8U                                                                                   // UDP头部长度
#define LINKG_WIFI_TX_MTU                  1500U                                                                                // Wi-Fi接口MTU
#define LINKG_WIFI_TX_UDP_PAYLOAD_MAX      (LINKG_WIFI_TX_MTU - LINKG_WIFI_TX_IPV4_HEADER_SIZE - LINKG_WIFI_TX_UDP_HEADER_SIZE) // 单个UDP报文最大负载

#define LINKG_WIFI_TX_BATCH_SIZE_MAX       32U                                                                                  // 单次发送调度最大Packet数量
#define LINKG_WIFI_TX_QUEUE_BATCH_COUNT    4U                                                                                   // 单业务等待队列最多缓存批次数

/****************************** 内部类型 ******************************/

/**
 * @brief 单个Wi-Fi业务类别的预分配发送Scratch。
 */
typedef struct
{
    linkg_wifi_tx_queue_item_t items[LINKG_WIFI_TX_BATCH_SIZE_MAX];        // 当前发送Queue元素快照
    struct mmsghdr             messages[LINKG_WIFI_TX_BATCH_SIZE_MAX];     // sendmmsg消息数组
    struct iovec               iovecs[LINKG_WIFI_TX_BATCH_SIZE_MAX];       // iovec数组
    struct sockaddr_in         destinations[LINKG_WIFI_TX_BATCH_SIZE_MAX]; // UDP目标地址数组
} linkg_wifi_tx_scratch_t;

/**
 * @brief 单次Wi-Fi发送调度计划。
 */
typedef struct
{
    uint32_t class_count[LINKG_WIFI_TRAFFIC_COUNT]; // 各业务类别本轮计划提交数量
    uint32_t total_count;                           // 本轮计划提交总数量
} linkg_wifi_tx_plan_t;

/**
 * @brief Wi-Fi发送模块运行上下文。
 */
struct linkg_wifi_tx
{
    pthread_mutex_t          lock;                               // 三业务队列、流控检查和发送调度串行锁
    linkg_wifi_tx_scratch_t  scratch[LINKG_WIFI_TRAFFIC_COUNT];  // 三业务预分配Scratch
    linkg_wifi_tx_queue_t   *queues[LINKG_WIFI_TRAFFIC_COUNT];   // 三业务等待FIFO
    linkg_wifi_flowctrl_t   *flowctrl;                           // Wi-Fi设备发送流控Gate

    int                     *socket_fds;                         // 借用Wi-Fi Link业务Socket数组
    const uint16_t          *service_ports;                      // 借用Wi-Fi Link业务端口数组

    uint32_t                 capacity;                           // 单次发送调度最大提交Packet数量
    uint32_t                 queue_capacity;                     // 单个业务等待队列最大Packet数量

    _Atomic bool             started;                            // 发送模块运行状态
};

/****************************** 业务映射 ******************************/

/**
 * @brief 将Link发送类别映射为Wi-Fi内部业务类别。
 */
static int _linkg_wifi_tx_class_to_traffic(linkg_link_tx_class_t tx_class, linkg_wifi_traffic_class_t *traffic_class)
{
    if (traffic_class == NULL)
    {
        return -EINVAL;
    }

    switch (tx_class)
    {
        case LINKG_LINK_TX_CLASS_REALTIME:
            *traffic_class = LINKG_WIFI_TRAFFIC_REALTIME;
            return 0;

        case LINKG_LINK_TX_CLASS_VIDEO:
            *traffic_class = LINKG_WIFI_TRAFFIC_VIDEO;
            return 0;

        case LINKG_LINK_TX_CLASS_DATA:
            *traffic_class = LINKG_WIFI_TRAFFIC_DATA;
            return 0;

        default:
            return -EINVAL;
    }
}

/**
 * @brief 判断Packet业务标志是否与Wi-Fi业务类别一致。
 */
static bool _linkg_wifi_tx_packet_matches_traffic(const linkg_packet_t *packet, linkg_wifi_traffic_class_t traffic_class)
{
    if (packet == NULL)
    {
        return false;
    }

    switch (traffic_class)
    {
        case LINKG_WIFI_TRAFFIC_REALTIME:
            return linkg_packet_is_realtime(packet);

        case LINKG_WIFI_TRAFFIC_VIDEO:
            return linkg_packet_is_video(packet);

        case LINKG_WIFI_TRAFFIC_DATA:
            return linkg_packet_is_data(packet);

        default:
            return false;
    }
}

/****************************** 参数校验 ******************************/

/**
 * @brief 校验Wi-Fi下一跳Endpoint是否合法。
 */
static int _linkg_wifi_tx_validate_destination(const linkg_wifi_tx_t *tx, const linkg_path_endpoint_t *destination)
{
    const struct sockaddr_in *target;

    if (tx == NULL || destination == NULL)
    {
        return -EINVAL;
    }

    if (destination->length != sizeof(struct sockaddr_in))
    {
        return -EDESTADDRREQ;
    }

    if (destination->address.ss_family != AF_INET)
    {
        return -EAFNOSUPPORT;
    }

    target = (const struct sockaddr_in *)&destination->address;

    if (!linkg_network_ipv4_address_valid(&target->sin_addr))
    {
        return -EDESTADDRREQ;
    }

    if (target->sin_port != htons(tx->service_ports[LINKG_WIFI_TRAFFIC_DATA]))
    {
        return -EDESTADDRREQ;
    }

    return 0;
}

/**
 * @brief 校验单个待发送Packet是否合法。
 */
static int _linkg_wifi_tx_validate_packet(const linkg_packet_t *packet)
{
    if (packet == NULL || packet->pool == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_length == 0U)
    {
        return -EINVAL;
    }

    if (packet->data_length > LINKG_WIFI_TX_UDP_PAYLOAD_MAX)
    {
        return -EMSGSIZE;
    }

    if (packet->data_offset > packet->pool->slot_size ||
        packet->data_length > packet->pool->slot_size - packet->data_offset)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验同一业务类别的完整发送批次。
 */
static int _linkg_wifi_tx_validate_batch(linkg_packet_t *const *packets, uint32_t count, linkg_wifi_traffic_class_t traffic_class)
{
    uint32_t index;
    int      ret;

    for (index = 0U; index < count; index++)
    {
        ret = _linkg_wifi_tx_validate_packet(packets[index]);
        if (ret != 0)
        {
            return ret;
        }

        if (!_linkg_wifi_tx_packet_matches_traffic(packets[index], traffic_class))
        {
            return -EINVAL;
        }
    }

    return 0;
}

/****************************** Path统计 ******************************/

/**
 * @brief 记录单个Packet为主动丢弃。
 */
static void _linkg_wifi_tx_record_dropped(linkg_path_t *path, const linkg_packet_t *packet)
{
    static _Atomic uint64_t dropped_count;
    uint64_t                count;

    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_dropped(path, packet->data_length, 1U);

    count = atomic_fetch_add(&dropped_count, 1U) + 1U;

    if ((count % 1000U) == 0U)
    {
        WIFI_WARN("TX主动丢包累计=%llu", (unsigned long long)count);
    }
}

/**
 * @brief 记录单个Packet发送失败。
 */
static void _linkg_wifi_tx_record_failed(linkg_path_t *path, const linkg_packet_t *packet)
{
    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_failed(path, packet->data_length, 1U);
}

/**
 * @brief 记录单个等待Packet因Path失效而过期。
 */
static void _linkg_wifi_tx_record_expired(linkg_path_t *path, const linkg_packet_t *packet)
{
    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_expired(path, packet->data_length, 1U);
}

/****************************** Queue辅助 ******************************/

/**
 * @brief 判断当前Queue队首是否属于一个需要整体丢弃的TX Group。
 */
static bool _linkg_wifi_tx_queue_head_grouped(const linkg_wifi_tx_queue_item_t *items, uint32_t count)
{
    uint32_t flags;
    uint32_t index;

    if (items == NULL || count == 0U || items[0].packet == NULL)
    {
        return false;
    }

    flags = items[0].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;
    if ((flags & (LINKG_PACKET_FLAG_TX_GROUP_FIRST | LINKG_PACKET_FLAG_TX_GROUP_LAST)) != 0U)
    {
        return true;
    }

    /**
     * 当前队首可能是一次partial send后遗留的TX Group尾部。
     * 如果在下一个Group FIRST出现之前先看到LAST，则把当前前缀视为同一Group剩余部分。
     */
    for (index = 1U; index < count; index++)
    {
        if (items[index].packet == NULL)
        {
            break;
        }

        flags = items[index].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;

        if ((flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U)
        {
            break;
        }

        if ((flags & LINKG_PACKET_FLAG_TX_GROUP_LAST) != 0U)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 从指定业务队列丢弃最旧完整TX Group或单个普通Packet。
 *
 * @note 调用方必须持有tx->lock。
 */
static uint32_t _linkg_wifi_tx_drop_oldest_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class)
{
    linkg_wifi_tx_scratch_t *scratch;
    linkg_wifi_tx_queue_t   *queue;
    uint32_t                 dropped_total;
    uint32_t                 peek_count;
    uint32_t                 drop_count;
    uint32_t                 queue_count;
    uint32_t                 flags;
    uint32_t                 index;
    bool                     grouped;
    bool                     found_last;
    int                      ret;

    queue = tx->queues[traffic_class];
    if (queue == NULL)
    {
        return 0U;
    }

    queue_count = linkg_wifi_tx_queue_count(queue);
    if (queue_count == 0U)
    {
        return 0U;
    }

    scratch = &tx->scratch[traffic_class];

    peek_count = queue_count;
    if (peek_count > LINKG_WIFI_TX_BATCH_SIZE_MAX)
    {
        peek_count = LINKG_WIFI_TX_BATCH_SIZE_MAX;
    }

    peek_count = linkg_wifi_tx_queue_peek_batch(queue, scratch->items, peek_count);
    if (peek_count == 0U)
    {
        return 0U;
    }

    grouped       = _linkg_wifi_tx_queue_head_grouped(scratch->items, peek_count);
    dropped_total = 0U;

    do
    {
        peek_count = linkg_wifi_tx_queue_count(queue);
        if (peek_count == 0U)
        {
            break;
        }

        if (peek_count > LINKG_WIFI_TX_BATCH_SIZE_MAX)
        {
            peek_count = LINKG_WIFI_TX_BATCH_SIZE_MAX;
        }

        peek_count = linkg_wifi_tx_queue_peek_batch(queue, scratch->items, peek_count);
        if (peek_count == 0U)
        {
            break;
        }

        drop_count = 1U;
        found_last = false;

        if (grouped)
        {
            drop_count = peek_count;

            for (index = 0U; index < peek_count; index++)
            {
                if (scratch->items[index].packet == NULL)
                {
                    drop_count = index;
                    break;
                }

                flags = scratch->items[index].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;
                if ((flags & LINKG_PACKET_FLAG_TX_GROUP_LAST) != 0U)
                {
                    drop_count = index + 1U;
                    found_last = true;
                    break;
                }
            }
        }

        if (drop_count == 0U)
        {
            break;
        }

        for (index = 0U; index < drop_count; index++)
        {
            _linkg_wifi_tx_record_dropped(scratch->items[index].path, scratch->items[index].packet);
        }

        ret = linkg_wifi_tx_queue_discard_batch(queue, drop_count);
        if (ret != 0)
        {
            break;
        }

        dropped_total += drop_count;

        if (!grouped || found_last)
        {
            break;
        }
    }
    while (linkg_wifi_tx_queue_count(queue) > 0U);

    return dropped_total;
}

/**
 * @brief 为当前新批次腾出足够Queue容量。
 *
 * @note 调用方必须持有tx->lock。
 */
static int _linkg_wifi_tx_make_room_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, uint32_t required_count)
{
    linkg_wifi_tx_queue_t *queue;
    uint32_t               dropped;

    queue = tx->queues[traffic_class];
    if (queue == NULL)
    {
        return -ENODEV;
    }

    if (required_count > linkg_wifi_tx_queue_capacity(queue))
    {
        return -ENOSPC;
    }

    while (linkg_wifi_tx_queue_available(queue) < required_count)
    {
        dropped = _linkg_wifi_tx_drop_oldest_locked(tx, traffic_class);
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
 * @note 调用方必须持有tx->lock。
 */
static int _linkg_wifi_tx_enqueue_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count)
{
    uint32_t pushed_count;
    int      ret;

    ret = _linkg_wifi_tx_make_room_locked(tx, traffic_class, count);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_wifi_tx_queue_push_batch(tx->queues[traffic_class],
                                         packets,
                                         count,
                                         path,
                                         destination,
                                         linkg_time_monotonic_us(),
                                         &pushed_count);
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
 *
 * @note 调用方必须持有tx->lock。
 */
static void _linkg_wifi_tx_build_plan_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t trigger_class, linkg_wifi_tx_plan_t *plan)
{
    uint32_t remaining;
    uint32_t available;
    uint32_t take_count;
    uint32_t index;

    memset(plan, 0, sizeof(*plan));

    remaining = tx->capacity;

    for (index = LINKG_WIFI_TRAFFIC_REALTIME;
         index <= (uint32_t)trigger_class && index < LINKG_WIFI_TRAFFIC_COUNT && remaining > 0U;
         index++)
    {
        available = linkg_wifi_tx_queue_count(tx->queues[index]);
        take_count = available;

        if (take_count > remaining)
        {
            take_count = remaining;
        }

        plan->class_count[index] = take_count;
        plan->total_count       += take_count;
        remaining               -= take_count;
    }
}

/****************************** Socket发送 ******************************/

/**
 * @brief 判断sendmmsg错误是否属于短时背压。
 */
static bool _linkg_wifi_tx_error_transient(int error)
{
    return error == -EAGAIN || error == -ENOBUFS;
}

/**
 * @brief 使用预分配Scratch对当前Queue元素执行一次非阻塞sendmmsg。
 *
 * @return 大于等于0表示成功提交的连续前缀数量；小于0表示sendmmsg错误。
 *
 * @note 不执行流控、不操作Queue、不负责重试或丢包，调用方必须持有tx->lock。
 */
static int _linkg_wifi_tx_send_once_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, linkg_wifi_tx_queue_item_t *items, uint32_t count)
{
    linkg_wifi_tx_scratch_t  *scratch;
    const struct sockaddr_in *target;
    uint32_t                  index;
    int                       socket_fd;
    int                       ret;

    if (tx == NULL || items == NULL || count == 0U || count > tx->capacity || count > LINKG_WIFI_TX_BATCH_SIZE_MAX)
    {
        return -EINVAL;
    }

    socket_fd = tx->socket_fds[traffic_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    scratch = &tx->scratch[traffic_class];

    memset(scratch->messages, 0, count * sizeof(scratch->messages[0]));

    for (index = 0U; index < count; index++)
    {
        if (items[index].packet == NULL || items[index].path == NULL)
        {
            return -EINVAL;
        }

        target = (const struct sockaddr_in *)&items[index].destination.address;

        scratch->destinations[index] = *target;
        scratch->destinations[index].sin_port = htons(tx->service_ports[traffic_class]);

        scratch->iovecs[index].iov_base = (void *)linkg_packet_const_data(items[index].packet);
        scratch->iovecs[index].iov_len  = items[index].packet->data_length;

        scratch->messages[index].msg_hdr.msg_name       = &scratch->destinations[index];
        scratch->messages[index].msg_hdr.msg_namelen    = sizeof(scratch->destinations[index]);
        scratch->messages[index].msg_hdr.msg_iov        = &scratch->iovecs[index];
        scratch->messages[index].msg_hdr.msg_iovlen     = 1U;
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
 * @brief 从指定业务队列执行当前计划数量的发送。
 *
 * @param stopped 返回true表示本轮出现partial或发送压力，后续低优先级业务不得继续提交。
 *
 * @note 调用方必须持有tx->lock。
 */
static int _linkg_wifi_tx_dispatch_class_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, uint32_t planned_count, bool *stopped)
{
    linkg_wifi_tx_scratch_t *scratch;
    linkg_wifi_tx_queue_t   *queue;
    uint32_t                 remaining;
    uint32_t                 peek_count;
    uint32_t                 send_count;
    uint32_t                 valid_prefix;
    uint32_t                 sent_count;
    uint32_t                 submitted_total;
    uint32_t                 index;
    int                      ret;

    if (stopped == NULL)
    {
        return -EINVAL;
    }

    *stopped = false;

    if (planned_count == 0U)
    {
        return 0;
    }

    scratch = &tx->scratch[traffic_class];
    queue   = tx->queues[traffic_class];

    remaining       = planned_count;
    submitted_total = 0U;

    while (remaining > 0U && linkg_wifi_tx_queue_count(queue) > 0U)
    {
        peek_count = remaining;
        if (peek_count > LINKG_WIFI_TX_BATCH_SIZE_MAX)
        {
            peek_count = LINKG_WIFI_TX_BATCH_SIZE_MAX;
        }

        peek_count = linkg_wifi_tx_queue_peek_batch(queue, scratch->items, peek_count);
        if (peek_count == 0U)
        {
            break;
        }

        valid_prefix = 0U;

        for (index = 0U; index < peek_count; index++)
        {
            if (scratch->items[index].packet == NULL || scratch->items[index].path == NULL)
            {
                break;
            }

            if (!linkg_path_is_active(scratch->items[index].path))
            {
                break;
            }

            if (_linkg_wifi_tx_validate_packet(scratch->items[index].packet) != 0)
            {
                break;
            }

            valid_prefix++;
        }

        if (valid_prefix == 0U)
        {
            if (scratch->items[0].packet != NULL && scratch->items[0].path != NULL)
            {
                if (!linkg_path_is_active(scratch->items[0].path))
                {
                    _linkg_wifi_tx_record_expired(scratch->items[0].path, scratch->items[0].packet);
                }
                else
                {
                    _linkg_wifi_tx_record_dropped(scratch->items[0].path, scratch->items[0].packet);
                }
            }

            ret = linkg_wifi_tx_queue_discard_batch(queue, 1U);
            if (ret != 0)
            {
                *stopped = true;
                return ret;
            }

            continue;
        }

        send_count = valid_prefix;

        ret = _linkg_wifi_tx_send_once_locked(tx, traffic_class, scratch->items, send_count);
        if (ret < 0)
        {
            if (_linkg_wifi_tx_error_transient(ret))
            {
                *stopped = true;
                return (int)submitted_total;
            }

            WIFI_WARN("TX sendmmsg失败，traffic=%u packets=%u error=%d",
                      (unsigned int)traffic_class,
                      send_count,
                      ret);

            for (index = 0U; index < send_count; index++)
            {
                _linkg_wifi_tx_record_failed(scratch->items[index].path, scratch->items[index].packet);
            }

            linkg_wifi_tx_queue_discard_batch(queue, send_count);

            *stopped = true;
            return ret;
        }

        sent_count = (uint32_t)ret;

        for (index = 0U; index < sent_count; index++)
        {
            linkg_path_record_tx_success(scratch->items[index].path, scratch->items[index].packet->data_length, 1U);
        }

        if (sent_count > 0U)
        {
            ret = linkg_wifi_tx_queue_discard_batch(queue, sent_count);
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
            WIFI_WARN("TX sendmmsg部分发送，traffic=%u submit=%u sent=%u pending=%u",
                      (unsigned int)traffic_class,
                      send_count,
                      sent_count,
                      send_count - sent_count);

            *stopped = true;
            break;
        }
    }

    return (int)submitted_total;
}

/**
 * @brief 按触发业务类别执行一次Wi-Fi QoS发送调度。
 *
 * @note 每次只读取一次Flowctrl；本轮所有业务合计最多提交tx->capacity个Packet。
 *       REALTIME触发不向VIDEO/DATA让出剩余budget，VIDEO触发不向DATA让出剩余budget。
 *       调用方必须持有tx->lock。
 */
static int _linkg_wifi_tx_dispatch_once_locked(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t trigger_class)
{
    linkg_wifi_flowctrl_sample_t sample;
    linkg_wifi_tx_plan_t         plan;
    uint32_t                     submitted_total;
    uint32_t                     index;
    bool                         submit_allowed;
    bool                         stopped;
    int                          ret;

    submit_allowed = false;

    ret = linkg_wifi_flowctrl_check(tx->flowctrl, &submit_allowed, &sample);
    if (ret != 0)
    {
        WIFI_WARN("TX流控状态读取失败，error=%d", ret);
        return 0;
    }

    if (!submit_allowed)
    {
        return 0;
    }

    _linkg_wifi_tx_build_plan_locked(tx, trigger_class, &plan);

    submitted_total = 0U;

    for (index = LINKG_WIFI_TRAFFIC_REALTIME;
         index <= (uint32_t)trigger_class && index < LINKG_WIFI_TRAFFIC_COUNT;
         index++)
    {
        if (plan.class_count[index] == 0U)
        {
            continue;
        }

        stopped = false;

        ret = _linkg_wifi_tx_dispatch_class_locked(tx,
                                                   (linkg_wifi_traffic_class_t)index,
                                                   plan.class_count[index],
                                                   &stopped);
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
 * @brief 创建Wi-Fi发送模块。
 *
 * socket_fds和service_ports仅借用，生命周期由Wi-Fi Link保证。
 * 每个业务类别创建独立固定容量等待队列，发送Scratch随Wi-Fi TX对象一次性分配，
 * Flowctrl由Wi-Fi TX模块独占管理。
 */
linkg_wifi_tx_t *linkg_wifi_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports)
{
    linkg_wifi_tx_t *tx;
    uint32_t         index;
    int              ret;

    if (capacity == 0U ||
        capacity > LINKG_WIFI_TX_BATCH_SIZE_MAX ||
        socket_fds == NULL ||
        service_ports == NULL)
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
    tx->capacity       = capacity;
    tx->queue_capacity = capacity * LINKG_WIFI_TX_QUEUE_BATCH_COUNT;

    atomic_store(&tx->started, false);

    ret = pthread_mutex_init(&tx->lock, NULL);
    if (ret != 0)
    {
        goto fail_tx;
    }

    tx->flowctrl = linkg_wifi_flowctrl_create();
    if (tx->flowctrl == NULL)
    {
        goto fail_lock;
    }

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
    {
        tx->queues[index] = linkg_wifi_tx_queue_create(tx->queue_capacity);
        if (tx->queues[index] == NULL)
        {
            goto fail_queues;
        }
    }

    return tx;

fail_queues:
    while (index > 0U)
    {
        index--;

        linkg_wifi_tx_queue_destroy(tx->queues[index]);
        tx->queues[index] = NULL;
    }

    linkg_wifi_flowctrl_destroy(tx->flowctrl);
    tx->flowctrl = NULL;

fail_lock:
    pthread_mutex_destroy(&tx->lock);

fail_tx:
    free(tx);

    return NULL;
}

/**
 * @brief 销毁Wi-Fi发送模块。
 *
 * @note 调用前不能再有其他线程访问tx。
 */
void linkg_wifi_tx_destroy(linkg_wifi_tx_t *tx)
{
    uint32_t index;

    if (tx == NULL)
    {
        return;
    }

    linkg_wifi_tx_stop(tx);

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
    {
        linkg_wifi_tx_queue_destroy(tx->queues[index]);
        tx->queues[index] = NULL;
    }

    linkg_wifi_flowctrl_destroy(tx->flowctrl);
    tx->flowctrl = NULL;

    pthread_mutex_destroy(&tx->lock);

    free(tx);
}

/**
 * @brief 启动Wi-Fi发送模块。
 */
int linkg_wifi_tx_start(linkg_wifi_tx_t *tx)
{
    uint32_t index;
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

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
    {
        if (tx->socket_fds[index] < 0)
        {
            pthread_mutex_unlock(&tx->lock);
            return -ENODEV;
        }
    }

    ret = linkg_wifi_flowctrl_start(tx->flowctrl);
    if (ret != 0)
    {
        pthread_mutex_unlock(&tx->lock);
        return ret;
    }

    atomic_store(&tx->started, true);

    pthread_mutex_unlock(&tx->lock);

    return 0;
}

/**
 * @brief 停止Wi-Fi发送模块。
 *
 * 停止后不再接受新的发送提交，并清空全部业务等待队列。
 */
void linkg_wifi_tx_stop(linkg_wifi_tx_t *tx)
{
    uint32_t index;
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

    linkg_wifi_flowctrl_stop(tx->flowctrl);

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
    {
        linkg_wifi_tx_queue_clear(tx->queues[index]);
    }

    pthread_mutex_unlock(&tx->lock);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 提交同一业务类别、同一Path和同一下一跳Endpoint的数据包批次。
 *
 * 所有Packet统一先进入对应REALTIME/VIDEO/DATA等待队列；队列容量不足时优先丢弃
 * 最旧完整TX Group或普通Packet，为当前新批次腾出空间。随后读取一次Flowctrl，
 * 如果允许提交，则按照本次触发业务类别执行一次最多capacity个Packet的QoS发送。
 *
 * @note results[index]为0表示Wi-Fi已经接受对应Packet的发送责任，Packet可能已经
 *       提交Socket，也可能仍位于对应业务等待队列；负数表示Wi-Fi拒绝接管。
 */
int linkg_wifi_tx_submit(linkg_wifi_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_wifi_traffic_class_t traffic_class;
    uint32_t                   index;
    int                        accepted_count;
    int                        ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > tx->capacity || count > LINKG_WIFI_TX_BATCH_SIZE_MAX)
    {
        return -EOVERFLOW;
    }

    if (!atomic_load(&tx->started))
    {
        return -ENODEV;
    }

    ret = _linkg_wifi_tx_class_to_traffic(tx_class, &traffic_class);
    if (ret != 0)
    {
        return ret;
    }

    if (tx->socket_fds[traffic_class] < 0)
    {
        return -ENODEV;
    }

    ret = _linkg_wifi_tx_validate_destination(tx, destination);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_wifi_tx_validate_batch(packets, count, traffic_class);
    if (ret != 0)
    {
        return ret;
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
            _linkg_wifi_tx_record_dropped(path, packets[index]);
        }

        pthread_mutex_unlock(&tx->lock);
        return 0;
    }

    ret = _linkg_wifi_tx_enqueue_locked(tx,
                                        traffic_class,
                                        path,
                                        destination,
                                        packets,
                                        count);
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

    ret = _linkg_wifi_tx_dispatch_once_locked(tx, traffic_class);
    if (ret < 0)
    {
        WIFI_WARN("TX等待队列调度失败，trigger=%u error=%d",
                  (unsigned int)traffic_class,
                  ret);
    }

    pthread_mutex_unlock(&tx->lock);

    return accepted_count;
}
