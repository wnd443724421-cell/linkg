/**
 * @file wifi_tx.c
 * @brief LinkG Wi-Fi发送模块实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-08
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
#include "wifi_traffic.h"

/****************************** 发送参数 ******************************/

#define LINKG_WIFI_TX_IPV4_HEADER_SIZE 20U                                                                                 // IPv4最小头部长度
#define LINKG_WIFI_TX_UDP_HEADER_SIZE  8U                                                                                  // UDP头部长度
#define LINKG_WIFI_TX_MTU              1500U                                                                               // Wi-Fi接口MTU
#define LINKG_WIFI_TX_UDP_PAYLOAD_MAX  (LINKG_WIFI_TX_MTU - LINKG_WIFI_TX_IPV4_HEADER_SIZE - LINKG_WIFI_TX_UDP_HEADER_SIZE) // 单个UDP报文最大负载

/****************************** 内部类型 ******************************/

/**
 * @brief 单个Wi-Fi业务类别的预分配发送Scratch。
 */
typedef struct
{
    pthread_mutex_t    lock;         // 当前业务类别发送Scratch保护锁
    struct mmsghdr    *messages;     // 预分配sendmmsg消息数组
    struct iovec      *iovecs;       // 预分配iovec数组
    struct sockaddr_in *destinations; // 预分配目标地址数组
    uint32_t          *packet_indices; // 预分配有效Packet原始索引数组
} linkg_wifi_tx_scratch_t;

struct linkg_wifi_tx
{
    linkg_wifi_tx_scratch_t scratch[LINKG_WIFI_TRAFFIC_COUNT]; // 各业务类别独立发送Scratch
    int                    *socket_fds;                         // 借用Wi-Fi Link业务Socket数组
    const uint16_t         *service_ports;                      // 借用Wi-Fi Link业务端口数组
    uint32_t                capacity;                           // 单次同步提交最大Packet数量
    _Atomic bool            started;                            // 发送模块运行状态
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
    if (packet == NULL)
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

    return 0;
}

/****************************** Path统计 ******************************/

/**
 * @brief 记录单个Packet为主动丢弃。
 */
static void _linkg_wifi_tx_record_dropped(linkg_path_t *path, const linkg_packet_t *packet)
{
    if (path == NULL || packet == NULL)
    {
        return;
    }

    linkg_path_record_tx_dropped(path, packet->data_length, 1U);
}

/****************************** Scratch管理 ******************************/

/**
 * @brief 初始化单个业务类别的预分配发送Scratch。
 */
static int _linkg_wifi_tx_scratch_init(linkg_wifi_tx_scratch_t *scratch, uint32_t capacity)
{
    int ret;

    if (scratch == NULL || capacity == 0U)
    {
        return -EINVAL;
    }

    memset(scratch, 0, sizeof(*scratch));

    ret = pthread_mutex_init(&scratch->lock, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    scratch->messages = calloc(capacity, sizeof(*scratch->messages));
    if (scratch->messages == NULL)
    {
        ret = -ENOMEM;
        goto fail_lock;
    }

    scratch->iovecs = calloc(capacity, sizeof(*scratch->iovecs));
    if (scratch->iovecs == NULL)
    {
        ret = -ENOMEM;
        goto fail_messages;
    }

    scratch->destinations = calloc(capacity, sizeof(*scratch->destinations));
    if (scratch->destinations == NULL)
    {
        ret = -ENOMEM;
        goto fail_iovecs;
    }

    scratch->packet_indices = calloc(capacity, sizeof(*scratch->packet_indices));
    if (scratch->packet_indices == NULL)
    {
        ret = -ENOMEM;
        goto fail_destinations;
    }

    return 0;

fail_destinations:
    free(scratch->destinations);
    scratch->destinations = NULL;

fail_iovecs:
    free(scratch->iovecs);
    scratch->iovecs = NULL;

fail_messages:
    free(scratch->messages);
    scratch->messages = NULL;

fail_lock:
    pthread_mutex_destroy(&scratch->lock);
    return ret;
}

/**
 * @brief 释放单个业务类别的预分配发送Scratch。
 */
static void _linkg_wifi_tx_scratch_deinit(linkg_wifi_tx_scratch_t *scratch)
{
    if (scratch == NULL)
    {
        return;
    }

    free(scratch->packet_indices);
    free(scratch->destinations);
    free(scratch->iovecs);
    free(scratch->messages);

    scratch->packet_indices = NULL;
    scratch->destinations = NULL;
    scratch->iovecs = NULL;
    scratch->messages = NULL;

    pthread_mutex_destroy(&scratch->lock);
}

/****************************** 底层发送 ******************************/

/**
 * @brief 使用预分配Scratch将当前Packet批次直接非阻塞提交给内核Socket。
 *
 * @note 不执行用户态流控、不进入等待队列、不重试。
 *       sendmmsg仅调用一次，未被内核接受的Packet立即按丢弃处理。
 */
static int _linkg_wifi_tx_send_direct(linkg_wifi_tx_t *tx, linkg_wifi_traffic_class_t traffic_class, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_wifi_tx_scratch_t  *scratch;
    const struct sockaddr_in *target;
    uint64_t                  sent_bytes;
    uint32_t                  original_index;
    uint32_t                  valid_count;
    uint32_t                  sent_count;
    uint32_t                  index;
    int                       socket_fd;
    int                       error;
    int                       ret;

    if (tx == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (count > tx->capacity)
    {
        return -EOVERFLOW;
    }

    socket_fd = tx->socket_fds[traffic_class];
    if (socket_fd < 0)
    {
        return -ENODEV;
    }

    scratch = &tx->scratch[traffic_class];
    target = (const struct sockaddr_in *)&destination->address;

    pthread_mutex_lock(&scratch->lock);

    valid_count = 0U;

    for (index = 0U; index < count; index++)
    {
        results[index] = -EINPROGRESS;

        ret = _linkg_wifi_tx_validate_packet(packets[index]);
        if (ret != 0)
        {
            results[index] = ret;
            continue;
        }

        scratch->packet_indices[valid_count] = index;

        scratch->destinations[valid_count] = *target;
        scratch->destinations[valid_count].sin_port = htons(tx->service_ports[traffic_class]);

        scratch->iovecs[valid_count].iov_base = (void *)linkg_packet_const_data(packets[index]);
        scratch->iovecs[valid_count].iov_len = packets[index]->data_length;

        memset(&scratch->messages[valid_count], 0, sizeof(scratch->messages[valid_count]));
        scratch->messages[valid_count].msg_hdr.msg_name = &scratch->destinations[valid_count];
        scratch->messages[valid_count].msg_hdr.msg_namelen = sizeof(scratch->destinations[valid_count]);
        scratch->messages[valid_count].msg_hdr.msg_iov = &scratch->iovecs[valid_count];
        scratch->messages[valid_count].msg_hdr.msg_iovlen = 1U;

        valid_count++;
    }

    if (valid_count == 0U)
    {
        pthread_mutex_unlock(&scratch->lock);
        return 0;
    }

    ret = sendmmsg(socket_fd, scratch->messages, valid_count, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (ret < 0)
    {
        error = -errno;

        for (index = 0U; index < valid_count; index++)
        {
            original_index = scratch->packet_indices[index];
            results[original_index] = error;
            _linkg_wifi_tx_record_dropped(path, packets[original_index]);
        }

        pthread_mutex_unlock(&scratch->lock);
        return 0;
    }

    if ((uint32_t)ret > valid_count)
    {
        for (index = 0U; index < valid_count; index++)
        {
            original_index = scratch->packet_indices[index];
            results[original_index] = -EIO;
            _linkg_wifi_tx_record_dropped(path, packets[original_index]);
        }

        pthread_mutex_unlock(&scratch->lock);
        return 0;
    }

    sent_count = (uint32_t)ret;
    sent_bytes = 0U;

    for (index = 0U; index < sent_count; index++)
    {
        original_index = scratch->packet_indices[index];
        results[original_index] = 0;
        sent_bytes += packets[original_index]->data_length;
    }

    if (sent_count > 0U)
    {
        linkg_path_record_tx_success(path, sent_bytes, sent_count);
    }

    for (index = sent_count; index < valid_count; index++)
    {
        original_index = scratch->packet_indices[index];
        results[original_index] = -EAGAIN;
        _linkg_wifi_tx_record_dropped(path, packets[original_index]);
    }

    pthread_mutex_unlock(&scratch->lock);

    return (int)sent_count;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建Wi-Fi发送模块。
 *
 * @note socket_fds和service_ports仅借用，生命周期由Wi-Fi Link保证。
 */
linkg_wifi_tx_t *linkg_wifi_tx_create(uint32_t capacity, int *socket_fds, const uint16_t *service_ports)
{
    linkg_wifi_tx_t *tx;
    uint32_t         index;
    int              ret;

    if (capacity == 0U || capacity > LINKG_LINK_TX_BATCH_SIZE_DEFAULT || socket_fds == NULL || service_ports == NULL)
    {
        return NULL;
    }

    tx = calloc(1, sizeof(*tx));
    if (tx == NULL)
    {
        return NULL;
    }

    tx->capacity = capacity;
    tx->socket_fds = socket_fds;
    tx->service_ports = service_ports;
    atomic_store(&tx->started, false);

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
    {
        ret = _linkg_wifi_tx_scratch_init(&tx->scratch[index], capacity);
        if (ret != 0)
        {
            goto fail_scratch;
        }
    }

    return tx;

fail_scratch:
    while (index > 0U)
    {
        index--;
        _linkg_wifi_tx_scratch_deinit(&tx->scratch[index]);
    }

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
        _linkg_wifi_tx_scratch_deinit(&tx->scratch[index]);
    }

    free(tx);
}

/**
 * @brief 启动Wi-Fi发送模块。
 */
int linkg_wifi_tx_start(linkg_wifi_tx_t *tx)
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

    for (index = 0U; index < LINKG_WIFI_TRAFFIC_COUNT; index++)
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
 * @brief 停止Wi-Fi发送模块。
 */
void linkg_wifi_tx_stop(linkg_wifi_tx_t *tx)
{
    if (tx == NULL)
    {
        return;
    }

    atomic_store(&tx->started, false);
}

/****************************** 数据发送 ******************************/

/**
 * @brief 提交同一业务类别的数据包并立即进行一次非阻塞发送。
 *
 * @note 不执行用户态流控、不进入等待队列、不重试。
 *       results[index]为0表示对应Packet已经被内核Socket接受；
 *       负数表示当前Packet未发送并立即结束本次发送责任。
 */
int linkg_wifi_tx_submit(linkg_wifi_tx_t *tx, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_wifi_traffic_class_t traffic_class;
    uint32_t                   index;
    int                        ret;

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

    if (!linkg_path_is_active(path))
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = -ENODEV;

            if (packets[index] != NULL)
            {
                _linkg_wifi_tx_record_dropped(path, packets[index]);
            }
        }

        return 0;
    }

    return _linkg_wifi_tx_send_direct(tx, traffic_class, path, destination, packets, count, results);
}
