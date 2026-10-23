/**
 * @file cellular_tx.c
 * @brief LinkG蜂窝链路发送模块实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-10
 */

#define _GNU_SOURCE

#include "cellular_tx.h"

#include <arpa/inet.h>
#include <errno.h>
#include <net/if.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/eventfd.h>
#include <sys/random.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

#include "linkg_cellular_config.h"
#include "linkg_config.h"
#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "linkg_time.h"
#include "cellular_tx_queue.h"
#include "cellular_wire.h"


/****************************** 模块常量 ******************************/

#define LINKG_CELLULAR_TX_BATCH_SIZE_MAX                32U                                   // 单次发送调度最大Packet数量
#define LINKG_CELLULAR_TX_QUEUE_REALTIME_BATCH_COUNT    1U                                    // REALTIME队列缓存1批，共32包
#define LINKG_CELLULAR_TX_QUEUE_VIDEO_BATCH_COUNT       5U                                    // VIDEO队列缓存5批，共160包
#define LINKG_CELLULAR_TX_QUEUE_DATA_BATCH_COUNT        5U                                    // DATA队列缓存5批，共160包
#define LINKG_CELLULAR_TX_CONTROL_SIZE                  CMSG_SPACE(sizeof(struct in6_pktinfo))// 单个IPv6 PKTINFO控制区大小
#define LINKG_CELLULAR_TX_PEER_SLOTS                    256U                                  // 最多256个IPv6对端Sequence状态
#define LINKG_CELLULAR_TX_QOS_BATCH_MAX                 4U                                    // 小批次重选优先级
#define LINKG_CELLULAR_TX_IPV6_UDP_BYTES                48ULL                                 // IPv6 + UDP头部
#define LINKG_CELLULAR_TX_TOKEN_SCALE                   1000000ULL                            // 微字节令牌精度
#define LINKG_CELLULAR_TX_MIN_BURST                      6400ULL                               // 至少容纳完整双分片
#define LINKG_CELLULAR_TX_CONFIG_PERIOD_US               250000ULL                             // 动态配置检查周期
#define LINKG_CELLULAR_TX_BACKPRESSURE_US                 2000ULL                               // Socket短时背压重试
#define LINKG_CELLULAR_TX_REALTIME_TIMEOUT_US            100000ULL                             // 100ms
#define LINKG_CELLULAR_TX_VIDEO_TIMEOUT_US               250000ULL                             // 250ms
#define LINKG_CELLULAR_TX_DATA_TIMEOUT_US                500000ULL                             // 500ms

/****************************** 内部类型 ******************************/

/**
 * @brief 每个目标IPv6地址的三业务独立发送序号。
 */
typedef struct
{
    struct in6_addr address;                              // 对端下一跳IPv6
    uint32_t        sequence[LINKG_LINK_TX_CLASS_COUNT];  // 成功提交Socket的序号
    bool            used;                                 // 状态槽是否已分配
} linkg_cellular_tx_peer_sequence_t;

/**
 * @brief 单个蜂窝业务类别的预分配发送Scratch。
 */
typedef struct
{
    linkg_cellular_tx_queue_item_t items[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                                     // 当前发送Queue元素快照
    struct mmsghdr                 messages[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                                  // sendmmsg消息数组
    struct iovec                   iovecs[LINKG_CELLULAR_TX_BATCH_SIZE_MAX][2];                                 // Wire头和Transport帧
    linkg_cellular_wire_header_t   headers[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                                   // 当轮Wire头
    uint32_t                       peer_indices[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                              // 当轮序号Peer槽索引
    struct sockaddr_in6            destinations[LINKG_CELLULAR_TX_BATCH_SIZE_MAX];                              // UDP目标IPv6地址数组
    unsigned char                  controls[LINKG_CELLULAR_TX_BATCH_SIZE_MAX][LINKG_CELLULAR_TX_CONTROL_SIZE];  // IPv6 PKTINFO控制区
} linkg_cellular_tx_scratch_t;

/**
 * @brief 蜂窝发送模块运行上下文。
 */
struct linkg_cellular_tx
{
    pthread_mutex_t                   lock;                                          // 三业务队列和发送调度串行锁
    uint32_t                          session_id;                                    // 当前TX对象随机Session ID
    linkg_cellular_tx_peer_sequence_t peer_sequences[LINKG_CELLULAR_TX_PEER_SLOTS];  // Peer独立序号
    linkg_cellular_tx_scratch_t       scratch[LINKG_LINK_TX_CLASS_COUNT];            // 三业务预分配Scratch
    linkg_cellular_tx_queue_t         *queues[LINKG_LINK_TX_CLASS_COUNT];            // 三业务等待FIFO
    int                               *socket_fds;                                   // 借用Cellular Link业务Socket数组
    const uint16_t                    *service_ports;                                // 借用Cellular Link业务端口数组
    const char                        *interface_name;                               // 借用蜂窝出口接口名称
    uint32_t                          capacity;                                      // 单次发送调度最大提交Packet数量
    pthread_t                         worker;                                        // 独立QoS发送线程
    int                               event_fd;                                      // 非阻塞事件通知
    bool                              worker_running;                                // 工作线程生命周期
    uint32_t                          max_load_mbps;                                 // 当前生效限速
    uint64_t                          rate_bytes_per_second;                         // 总发送字节额度
    uint64_t                          tokens_scaled;                                 // 微字节令牌
    uint64_t                          burst_scaled;                                  // 桶最大额度
    uint64_t                          last_refill_us;                                // 最近补充时间
    uint64_t                          next_config_us;                                // 动态配置检查时刻
    _Atomic bool                      started;                                       // 发送模块运行状态
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 生成当前TX对象非零随机Session ID。
 */
static int _linkg_cellular_tx_generate_session(uint32_t *session_id)
{
    uint8_t  *cursor;
    size_t    remaining;
    ssize_t   result;
    uint32_t  value;

    if (session_id == NULL)
    {
        return -EINVAL;
    }

    do
    {
        value     = 0U;
        cursor    = (uint8_t *)&value;
        remaining = sizeof(value);

        while (remaining > 0U)
        {
            do
            {
                result = getrandom(cursor, remaining, 0U);
            }
            while (result < 0 && errno == EINTR);

            if (result <= 0)
            {
                return result == 0 ? -EIO : -errno;
            }

            cursor    += (size_t)result;
            remaining -= (size_t)result;
        }
    }
    while (value == 0U);

    *session_id = value;

    return 0;
}

/**
 * @brief 查找或分配指定目标IPv6的序号状态槽。
 *
 * @note 调用方持有tx->lock，不依赖Transport逻辑Node ID。
 */
static int _linkg_cellular_tx_peer_slot_locked(linkg_cellular_tx_t *tx, const struct in6_addr *address, uint32_t *slot)
{
    uint32_t free_index;
    uint32_t index;

    if (tx == NULL || address == NULL || slot == NULL)
    {
        return -EINVAL;
    }

    free_index = LINKG_CELLULAR_TX_PEER_SLOTS;

    for (index = 0U; index < LINKG_CELLULAR_TX_PEER_SLOTS; index++)
    {
        if (tx->peer_sequences[index].used)
        {
            if (memcmp(&tx->peer_sequences[index].address, address, sizeof(*address)) == 0)
            {
                *slot = index;
                return 0;
            }
        }
        else if (free_index == LINKG_CELLULAR_TX_PEER_SLOTS)
        {
            free_index = index;
        }
    }

    if (free_index == LINKG_CELLULAR_TX_PEER_SLOTS)
    {
        return -ENOSPC;
    }

    tx->peer_sequences[free_index].address = *address;
    tx->peer_sequences[free_index].used    = true;
    *slot                                  = free_index;

    return 0;
}

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
 * @brief 仅队列容量溢出时更新专用计数，不与其它主动丢弃混淆。
 */
static void _linkg_cellular_tx_record_queue_overflow(linkg_path_t *path, const linkg_packet_t *packet)
{
    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_queue_overflow(path, packet->data_length, 1U);
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
 * @note Transport保证单个TX Group最多包含两个连续Packet。
 *       调用方必须持有tx->lock。
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
        _linkg_cellular_tx_record_queue_overflow(items[index].path, items[index].packet);
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
 * @note 调用方必须持有tx->lock。
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
 * @note 调用方必须持有tx->lock。
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
 * @note 不执行Queue管理、不负责重试或丢包，调用方必须持有tx->lock。
 */
static int _linkg_cellular_tx_send_once_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t tx_class, linkg_cellular_tx_queue_item_t *items, uint32_t count)
{
    linkg_cellular_tx_scratch_t *scratch;
    const struct sockaddr_in6   *target;
    unsigned int                 interface_index;
    uint32_t                     index;
    uint32_t                     peer_slot;
    uint32_t                     sequence;
    uint8_t                      reserved_counts[LINKG_CELLULAR_TX_PEER_SLOTS] = {0};
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

        ret = _linkg_cellular_tx_peer_slot_locked(tx, &target->sin6_addr, &peer_slot);
        if (ret != 0)
        {
            return ret;
        }

        sequence = tx->peer_sequences[peer_slot].sequence[tx_class] + reserved_counts[peer_slot];
        reserved_counts[peer_slot]++;

        scratch->peer_indices[index]       = peer_slot;
        scratch->headers[index].session_id = htonl(tx->session_id);
        scratch->headers[index].sequence   = htonl(sequence);

        scratch->destinations[index]           = *target;
        scratch->destinations[index].sin6_port = htons(tx->service_ports[tx_class]);

        scratch->iovecs[index][0].iov_base = &scratch->headers[index];
        scratch->iovecs[index][0].iov_len  = LINKG_CELLULAR_WIRE_HEADER_SIZE;
        scratch->iovecs[index][1].iov_base = (void *)linkg_packet_const_data(items[index].packet);
        scratch->iovecs[index][1].iov_len  = items[index].packet->data_length;

        scratch->messages[index].msg_hdr.msg_name    = &scratch->destinations[index];
        scratch->messages[index].msg_hdr.msg_namelen = sizeof(scratch->destinations[index]);
        scratch->messages[index].msg_hdr.msg_iov     = scratch->iovecs[index];
        scratch->messages[index].msg_hdr.msg_iovlen  = 2U;

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

    // 仅成功提交Socket的前缀消耗序号，未提交尾部下次重试重新编号。
    for (index = 0U; index < (uint32_t)ret; index++)
    {
        tx->peer_sequences[scratch->peer_indices[index]].sequence[tx_class]++;
    }

    return ret;
}

/****************************** QoS整形与独立发送调度 ******************************/

/** @brief 获取业务类别等待期限。 */
static uint64_t _linkg_cellular_tx_timeout_us(linkg_link_tx_class_t cls)
{
    switch (cls)
    {
        case LINKG_LINK_TX_CLASS_REALTIME: return LINKG_CELLULAR_TX_REALTIME_TIMEOUT_US;
        case LINKG_LINK_TX_CLASS_VIDEO:    return LINKG_CELLULAR_TX_VIDEO_TIMEOUT_US;
        case LINKG_LINK_TX_CLASS_DATA:     return LINKG_CELLULAR_TX_DATA_TIMEOUT_US;
        default:                           return 0U;
    }
}

/** @brief 一个LinkG UDP Wire包的IPv6出口预算，不含其他usb0流量。 */
static uint64_t _linkg_cellular_tx_wire_bytes(const linkg_cellular_tx_queue_item_t *item)
{
    return (uint64_t)item->packet->data_length + LINKG_CELLULAR_WIRE_HEADER_SIZE +
           LINKG_CELLULAR_TX_IPV6_UDP_BYTES;
}

/** @brief 设置单实例全部Peer共享的令牌桶速率。锁由调用方持有。 */
static void _linkg_cellular_tx_set_rate_locked(linkg_cellular_tx_t *tx, uint32_t mbps)
{
    uint64_t burst;

    if (mbps < LINKG_CELLULAR_MAX_LOAD_MBPS_MIN || mbps > LINKG_CELLULAR_MAX_LOAD_MBPS_MAX ||
        tx->max_load_mbps == mbps)
    {
        return;
    }

    tx->rate_bytes_per_second = (uint64_t)mbps * 125000ULL;
    burst = tx->rate_bytes_per_second / 200U; // 约5ms突发
    if (burst < LINKG_CELLULAR_TX_MIN_BURST)
    {
        burst = LINKG_CELLULAR_TX_MIN_BURST;
    }

    tx->burst_scaled = burst * LINKG_CELLULAR_TX_TOKEN_SCALE;
    if (tx->max_load_mbps == 0U)
    {
        tx->tokens_scaled = tx->burst_scaled;
    }
    if (tx->tokens_scaled > tx->burst_scaled)
    {
        tx->tokens_scaled = tx->burst_scaled;
    }
    tx->max_load_mbps = mbps;
    LINKG_LOG_INFO("Cellular TX QoS limit=%u Mbps burst=%llu bytes expiry=100/250/500ms",
                   mbps, (unsigned long long)burst);
}

/** @brief 令牌用百万分之一字节计数，保留微秒更新的整数余数。 */
static void _linkg_cellular_tx_refill_locked(linkg_cellular_tx_t *tx, uint64_t now_us)
{
    uint64_t delta;
    uint64_t add;

    if (now_us <= tx->last_refill_us)
    {
        return;
    }

    delta = now_us - tx->last_refill_us;
    tx->last_refill_us = now_us;
    if (delta > 1000000ULL)
    {
        delta = 1000000ULL;
    }

    add = delta * tx->rate_bytes_per_second;
    if (add >= tx->burst_scaled - tx->tokens_scaled)
    {
        tx->tokens_scaled = tx->burst_scaled;
    }
    else
    {
        tx->tokens_scaled += add;
    }
}

/** @brief 令牌不足时距离凑齐队首包（或双片组）还需多少微秒。 */
static uint64_t _linkg_cellular_tx_wait_us(const linkg_cellular_tx_t *tx, uint64_t wire_bytes)
{
    uint64_t need = wire_bytes * LINKG_CELLULAR_TX_TOKEN_SCALE;
    if (need <= tx->tokens_scaled)
    {
        return 0U;
    }

    need -= tx->tokens_scaled;
    return (need + tx->rate_bytes_per_second - 1U) / tx->rate_bytes_per_second;
}

/** @brief 过期先于令牌检查，三个队列均清理，完整双片一并淘汰。 */
static uint32_t _linkg_cellular_tx_expire_locked(linkg_cellular_tx_t *tx, uint64_t now_us)
{
    linkg_cellular_tx_queue_item_t items[2];
    linkg_cellular_tx_queue_t     *queue;
    uint64_t                       timeout_us;
    uint32_t                       peek_count;
    uint32_t                       drop_count;
    uint32_t                       total = 0U;
    uint32_t                       cls;
    uint32_t                       i;

    for (cls = 0U; cls < LINKG_LINK_TX_CLASS_COUNT; cls++)
    {
        queue = tx->queues[cls];
        timeout_us = _linkg_cellular_tx_timeout_us((linkg_link_tx_class_t)cls);

        while ((peek_count = linkg_cellular_tx_queue_peek_batch(queue, items, 2U)) > 0U)
        {
            if (now_us < items[0].enqueue_us || now_us - items[0].enqueue_us < timeout_us)
            {
                break;
            }

            drop_count = 1U;
            if ((items[0].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U &&
                peek_count > 1U && (items[1].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_LAST) != 0U)
            {
                drop_count = 2U;
            }

            for (i = 0U; i < drop_count; i++)
            {
                linkg_path_record_tx_expired(items[i].path, items[i].packet->data_length, 1U);
            }
            (void)linkg_cellular_tx_queue_discard_batch(queue, drop_count);
            total += drop_count;
        }
    }
    return total;
}

/** @brief 清除最高优先级队首失效Path，不占用令牌。 */
static bool _linkg_cellular_tx_drop_inactive_locked(linkg_cellular_tx_t *tx, linkg_link_tx_class_t cls)
{
    linkg_cellular_tx_queue_item_t items[2];
    uint32_t                       count;
    uint32_t                       drop = 1U;
    uint32_t                       i;

    count = linkg_cellular_tx_queue_peek_batch(tx->queues[cls], items, 2U);
    if (count == 0U || linkg_path_is_active(items[0].path))
    {
        return false;
    }

    if ((items[0].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U &&
        count > 1U && (items[1].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_LAST) != 0U)
    {
        drop = 2U;
    }

    for (i = 0U; i < drop; i++)
    {
        _linkg_cellular_tx_record_dropped(items[i].path, items[i].packet);
    }
    (void)linkg_cellular_tx_queue_discard_batch(tx->queues[cls], drop);
    return true;
}

/**
 * @brief 单次小批调度：REALTIME > VIDEO > DATA，无低优先级令牌借用。
 *
 * 返回成功提交或主动删除的Packet数量；wait_us>0要求等待，避免忙循环。
 * Socket已接受的部分前缀扣实际令牌、确认Sequence、保留未提交队尾。
 */
static int _linkg_cellular_tx_dispatch_once_locked(linkg_cellular_tx_t *tx, uint64_t now_us, uint64_t *wait_us)
{
    static const linkg_link_tx_class_t priority[] =
    {
        LINKG_LINK_TX_CLASS_REALTIME, LINKG_LINK_TX_CLASS_VIDEO, LINKG_LINK_TX_CLASS_DATA
    };
    linkg_cellular_tx_queue_item_t items[LINKG_CELLULAR_TX_QOS_BATCH_MAX];
    linkg_cellular_tx_queue_t     *queue;
    uint64_t                       budget;
    uint64_t                       group_bytes;
    uint32_t                       count;
    uint32_t                       n;
    uint32_t                       i;
    uint32_t                       step;
    uint32_t                       order;
    int                            done;
    int                            sent;

    *wait_us = 0U;
    _linkg_cellular_tx_refill_locked(tx, now_us);
    done = (int)_linkg_cellular_tx_expire_locked(tx, now_us);

    for (order = 0U; order < LINKG_LINK_TX_CLASS_COUNT; order++)
    {
        linkg_link_tx_class_t cls = priority[order];
        queue = tx->queues[cls];

        if (_linkg_cellular_tx_drop_inactive_locked(tx, cls))
        {
            return done + 1;
        }

        count = linkg_cellular_tx_queue_peek_batch(queue, items, LINKG_CELLULAR_TX_QOS_BATCH_MAX);
        if (count == 0U)
        {
            continue;
        }

        budget = 0U;
        n      = 0U;
        while (n < count)
        {
            if (!linkg_path_is_active(items[n].path))
            {
                break;
            }

            step = 1U;
            group_bytes = _linkg_cellular_tx_wire_bytes(&items[n]);
            if ((items[n].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_FIRST) != 0U)
            {
                if (n + 1U >= count ||
                    (items[n + 1U].packet->flags & LINKG_PACKET_FLAG_TX_GROUP_LAST) == 0U)
                {
                    break;
                }

                step = 2U;
                group_bytes += _linkg_cellular_tx_wire_bytes(&items[n + 1U]);
            }

            if ((budget + group_bytes) * LINKG_CELLULAR_TX_TOKEN_SCALE > tx->tokens_scaled)
            {
                if (n == 0U)
                {
                    *wait_us = _linkg_cellular_tx_wait_us(tx, group_bytes);
                }
                break;
            }

            budget += group_bytes;
            n += step;
        }

        if (n == 0U)
        {
            if (*wait_us == 0U)
            {
                *wait_us = LINKG_CELLULAR_TX_BACKPRESSURE_US;
            }
            return done;
        }

        sent = _linkg_cellular_tx_send_once_locked(tx, cls, items, n);
        if (sent < 0)
        {
            if (_linkg_cellular_tx_error_transient(sent))
            {
                *wait_us = LINKG_CELLULAR_TX_BACKPRESSURE_US;
                return done;
            }

            LINKG_LOG_WARN("Cellular TX sendmmsg failed, class=%u count=%u error=%d",
                           (unsigned int)cls, n, sent);
            for (i = 0U; i < n; i++)
            {
                _linkg_cellular_tx_record_failed(items[i].path, items[i].packet);
            }
            (void)linkg_cellular_tx_queue_discard_batch(queue, n);
            return done + (int)n;
        }

        if (sent == 0)
        {
            *wait_us = LINKG_CELLULAR_TX_BACKPRESSURE_US;
            return done;
        }

        for (i = 0U; i < (uint32_t)sent; i++)
        {
            tx->tokens_scaled -= _linkg_cellular_tx_wire_bytes(&items[i]) *
                                 LINKG_CELLULAR_TX_TOKEN_SCALE;
            linkg_path_record_tx_success(items[i].path, items[i].packet->data_length, 1U);
        }
        (void)linkg_cellular_tx_queue_discard_batch(queue, (uint32_t)sent);

        if ((uint32_t)sent < n)
        {
            *wait_us = LINKG_CELLULAR_TX_BACKPRESSURE_US;
        }

        return done + sent;
    }
    return done;
}

/** @brief 三类队首中最早的到期时间，不让低优先级旧包占用Packet Pool。 */
static uint64_t _linkg_cellular_tx_expiry_locked(linkg_cellular_tx_t *tx, uint64_t deadline)
{
    linkg_cellular_tx_queue_item_t item;
    uint64_t                       expiry;
    uint32_t                       cls;

    for (cls = 0U; cls < LINKG_LINK_TX_CLASS_COUNT; cls++)
    {
        if (linkg_cellular_tx_queue_peek_batch(tx->queues[cls], &item, 1U) == 0U)
        {
            continue;
        }
        expiry = item.enqueue_us + _linkg_cellular_tx_timeout_us((linkg_link_tx_class_t)cls);
        if (expiry < deadline)
        {
            deadline = expiry;
        }
    }
    return deadline;
}

/** @brief 事件通知：eventfd满表明已有未消费事件，不需要重复写。 */
static void _linkg_cellular_tx_wakeup(linkg_cellular_tx_t *tx)
{
    uint64_t value = 1U;
    ssize_t  ret;

    do
    {
        ret = write(tx->event_fd, &value, sizeof(value));
    }
    while (ret < 0 && errno == EINTR);
}

/**
 * @brief 唯一TX Worker：按令牌可用时间与最近超时期限休眠。
 *
 * 每250ms同步Web现有max_load_mbps，无需重启；stop必须先join再关闭socket。
 */
static void *_linkg_cellular_tx_worker(void *argument)
{
    linkg_cellular_tx_t    *tx = argument;
    linkg_cellular_config_t config;
    struct pollfd           fd;
    uint64_t                now;
    uint64_t                deadline;
    uint64_t                wait_us;
    uint64_t                value;
    int                     work;
    int                     timeout_ms;
    int                     ret;

    fd.fd     = tx->event_fd;
    fd.events = POLLIN;

    for (;;)
    {
        pthread_mutex_lock(&tx->lock);
        if (!atomic_load(&tx->started))
        {
            pthread_mutex_unlock(&tx->lock);
            break;
        }

        now = linkg_time_monotonic_us();
        if (now >= tx->next_config_us)
        {
            if (linkg_config_get_cellular(&config) == 0)
            {
                _linkg_cellular_tx_set_rate_locked(tx, config.max_load_mbps);
            }
            tx->next_config_us = now + LINKG_CELLULAR_TX_CONFIG_PERIOD_US;
        }

        work = _linkg_cellular_tx_dispatch_once_locked(tx, now, &wait_us);
        now = linkg_time_monotonic_us();
        deadline = _linkg_cellular_tx_expiry_locked(tx, tx->next_config_us);

        if (wait_us > 0U && now + wait_us < deadline)
        {
            deadline = now + wait_us;
        }
        else if (wait_us == 0U && work > 0)
        {
            deadline = now;
        }

        timeout_ms = (int)((deadline > now ? deadline - now : 0U) + 999U) / 1000;
        pthread_mutex_unlock(&tx->lock);

        do
        {
            ret = poll(&fd, 1U, timeout_ms);
        }
        while (ret < 0 && errno == EINTR && atomic_load(&tx->started));

        if (ret > 0 && (fd.revents & POLLIN) != 0)
        {
            while (read(tx->event_fd, &value, sizeof(value)) == sizeof(value))
            {
            }
        }
        else if (ret < 0 || (ret > 0 && (fd.revents & (POLLERR | POLLHUP | POLLNVAL)) != 0))
        {
            // 异常FD/轮询错误时避免忙循环，最终由stop完成资源回收。
            (void)poll(NULL, 0U, 5);
        }
    }

    return NULL;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝链路发送模块。
 *
 * socket_fds、service_ports和interface_name仅借用，生命周期由Cellular Link保证。
 */
linkg_cellular_tx_t *linkg_cellular_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports, const char *interface_name)
{
    static const uint32_t queue_batch_counts[LINKG_LINK_TX_CLASS_COUNT] =
    {
        [LINKG_LINK_TX_CLASS_REALTIME] = LINKG_CELLULAR_TX_QUEUE_REALTIME_BATCH_COUNT,
        [LINKG_LINK_TX_CLASS_VIDEO]    = LINKG_CELLULAR_TX_QUEUE_VIDEO_BATCH_COUNT,
        [LINKG_LINK_TX_CLASS_DATA]     = LINKG_CELLULAR_TX_QUEUE_DATA_BATCH_COUNT
    };

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
    tx->event_fd       = -1;

    ret = _linkg_cellular_tx_generate_session(&tx->session_id);
    if (ret != 0)
    {
        free(tx);
        return NULL;
    }

    atomic_store(&tx->started, false);

    ret = pthread_mutex_init(&tx->lock, NULL);
    if (ret != 0)
    {
        goto fail_tx;
    }

    tx->event_fd = eventfd(0U, EFD_NONBLOCK | EFD_CLOEXEC);
    if (tx->event_fd < 0)
    {
        pthread_mutex_destroy(&tx->lock);
        goto fail_tx;
    }

    _linkg_cellular_tx_set_rate_locked(tx, LINKG_CELLULAR_MAX_LOAD_MBPS_DEFAULT);

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        tx->queues[class_index] = linkg_cellular_tx_queue_create(capacity * queue_batch_counts[class_index]);
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

    close(tx->event_fd);
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

    close(tx->event_fd);
    pthread_mutex_destroy(&tx->lock);

    free(tx);
}

/**
 * @brief 启动蜂窝链路发送模块。
 */
int linkg_cellular_tx_start(linkg_cellular_tx_t *tx)
{
    uint32_t cls;
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

    for (cls = 0U; cls < LINKG_LINK_TX_CLASS_COUNT; cls++)
    {
        if (tx->socket_fds[cls] < 0)
        {
            pthread_mutex_unlock(&tx->lock);
            return -ENODEV;
        }
    }

    tx->tokens_scaled  = tx->burst_scaled;
    tx->last_refill_us = linkg_time_monotonic_us();
    tx->next_config_us = 0U;
    atomic_store(&tx->started, true);

    ret = pthread_create(&tx->worker, NULL, _linkg_cellular_tx_worker, tx);
    if (ret != 0)
    {
        atomic_store(&tx->started, false);
        pthread_mutex_unlock(&tx->lock);
        return -ret;
    }

    tx->worker_running = true;
    pthread_mutex_unlock(&tx->lock);
    return 0;
}

/** @brief 关闭TX Worker并清空积压；调用方随后才能关闭Socket。 */
void linkg_cellular_tx_stop(linkg_cellular_tx_t *tx)
{
    uint32_t  cls;
    pthread_t worker;
    bool      join_worker;

    if (tx == NULL)
    {
        return;
    }

    pthread_mutex_lock(&tx->lock);
    atomic_store(&tx->started, false);
    worker = tx->worker;
    join_worker = tx->worker_running;
    tx->worker_running = false;
    pthread_mutex_unlock(&tx->lock);

    if (join_worker)
    {
        _linkg_cellular_tx_wakeup(tx);
        (void)pthread_join(worker, NULL);
    }

    pthread_mutex_lock(&tx->lock);
    for (cls = 0U; cls < LINKG_LINK_TX_CLASS_COUNT; cls++)
    {
        linkg_cellular_tx_queue_clear(tx->queues[cls]);
    }
    pthread_mutex_unlock(&tx->lock);
}

/****************************** 队列清理 ******************************/

/**
 * @brief 清理三业务发送队列中引用指定Path的待发送Packet。
 *
 * @note 本接口用于Peer或Path退役后的控制面强制清理。
 *       不要求TX处于started状态；与正常submit/dispatch共用tx->lock串行化。
 *       Queue层只删除指针等于目标Path的元素并释放其Packet和Path引用。
 */
int linkg_cellular_tx_purge_path(linkg_cellular_tx_t *tx, linkg_path_t *path, uint32_t *purged_count)
{
    uint32_t queue_purged;
    uint32_t total_purged;
    uint32_t class_index;
    int      first_error;
    int      ret;

    if (tx == NULL || path == NULL || purged_count == NULL)
    {
        return -EINVAL;
    }

    *purged_count = 0U;

    ret = pthread_mutex_lock(&tx->lock);
    if (ret != 0)
    {
        return -ret;
    }

    first_error  = 0;
    total_purged = 0U;

    for (class_index = 0U; class_index < LINKG_LINK_TX_CLASS_COUNT; class_index++)
    {
        if (tx->queues[class_index] == NULL)
        {
            if (first_error == 0)
            {
                first_error = -ENODEV;
            }

            continue;
        }

        queue_purged = 0U;

        ret = linkg_cellular_tx_queue_purge_path(tx->queues[class_index], path, &queue_purged);
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        total_purged += queue_purged;
    }

    *purged_count = total_purged;

    pthread_mutex_unlock(&tx->lock);

    return first_error;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 提交同一业务类别、同一Path和同一下一跳Endpoint的数据包批次。
 *
 * 所有Packet统一先进入对应REALTIME/VIDEO/DATA等待队列；队列容量不足时主动丢弃
 * 最旧完整TX Group或普通Packet，为当前新批次腾出空间。成功入队即表示Cellular TX
 * 接受对应Packet的发送责任，立即通知独立QoS Worker。
 *
 * Worker按照REALTIME > VIDEO > DATA严格优先级消费，共享max_load_mbps令牌桶；
 * 各队列分别在100/250/500ms到期时丢弃，过期包不消耗令牌。
 * Web修改限速后最长约250ms同步到Worker，无需重启Cellular。
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

    _linkg_cellular_tx_wakeup(tx);

    pthread_mutex_unlock(&tx->lock);

    return accepted_count;
}
