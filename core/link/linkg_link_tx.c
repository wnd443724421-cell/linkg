/**
 * @file linkg_link_tx.c
 * @brief LinkG链路发送处理实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#include "linkg_link.h"

#include <errno.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"

#include "linkg_link_internal.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验待发送数据包。
 */
static int _linkg_link_validate_tx_packet(const linkg_packet_t *packet)
{
    if (packet == NULL || packet->pool == NULL || packet->slot == NULL)
    {
        return -EINVAL;
    }

    if (packet->data_offset > packet->pool->slot_size)
    {
        return -EINVAL;
    }

    if (packet->data_length == 0U)
    {
        return -EINVAL;
    }

    if (packet->data_length > packet->pool->slot_size - packet->data_offset)
    {
        return -EMSGSIZE;
    }

    return 0;
}

/**
 * @brief 根据数据包业务标志确定发送类别。
 */
static linkg_link_tx_class_t _linkg_link_tx_class(const linkg_packet_t *packet)
{
    if (linkg_packet_is_realtime(packet))
    {
        return LINKG_LINK_TX_CLASS_REALTIME;
    }

    if (linkg_packet_is_video(packet))
    {
        return LINKG_LINK_TX_CLASS_VIDEO;
    }

    return LINKG_LINK_TX_CLASS_DATA;
}

/**
 * @brief 同步提交当前批次中的指定业务类别。
 *
 * @note 调用方已经持有Link运行期io_lock读锁。
 *       packet和path仅在本次调用期间借用，具体链路需要异步持有时必须自行增加引用。
 */
static uint32_t _linkg_link_send_class(linkg_link_t *link, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, const linkg_link_tx_class_t *packet_classes, uint32_t chunk_count, uint32_t chunk_offset, linkg_link_tx_class_t tx_class, int *results)
{
    linkg_packet_t *class_packets[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t        class_indices[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    int             class_results[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t        accepted_count;
    uint32_t        class_count;
    uint32_t        item_index;
    uint32_t        index;
    int             ret;

    class_count = 0U;

    for (index = 0U; index < chunk_count; index++)
    {
        item_index = chunk_offset + index;

        if (results[item_index] != -EINPROGRESS || packet_classes[index] != tx_class)
        {
            continue;
        }

        class_packets[class_count] = packets[item_index];
        class_indices[class_count] = item_index;
        class_results[class_count] = -EINPROGRESS;
        class_count++;
    }

    if (class_count == 0U)
    {
        return 0U;
    }

    /**
     * STOPPING会在每次具体提交前重新检查运行状态，
     * 使已经进入当前submit的调用在停止开始后尽快终止后续提交。
     */
    if (atomic_load(&link->state) != LINKG_LINK_STATE_RUNNING)
    {
        ret = -ENETDOWN;
    }
    else if (!linkg_path_is_active(path))
    {
        ret = -ENODEV;
    }
    else
    {
        ret = link->ops->send_batch(link, path, tx_class, destination, class_packets, class_count, class_results);
    }

    if (ret < 0)
    {
        for (index = 0U; index < class_count; index++)
        {
            class_results[index] = ret;
        }
    }

    accepted_count = 0U;

    for (index = 0U; index < class_count; index++)
    {
        item_index = class_indices[index];

        if (class_results[index] == -EINPROGRESS)
        {
            LINKG_LOG_ERROR("link TX result not completed, link=%s, class=%d, index=%u",
                            link->name,
                            (int)tx_class,
                            index);

            class_results[index] = -EIO;
        }

        results[item_index] = class_results[index];

        if (class_results[index] == 0)
        {
            accepted_count++;
        }
    }

    if (ret >= 0 && (uint32_t)ret != accepted_count)
    {
        LINKG_LOG_ERROR("link TX batch result mismatch, link=%s, class=%d, ret=%d, accepted=%u",
                        link->name,
                        (int)tx_class,
                        ret,
                        accepted_count);
    }

    return accepted_count;
}

/**
 * @brief 同步发送一个内部批次。
 *
 * @note count不得超过LINKG_LINK_TX_BATCH_SIZE_DEFAULT。
 */
static uint32_t _linkg_link_submit_chunk(linkg_link_t *link, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t offset, uint32_t count, int *results)
{
    linkg_link_tx_class_t packet_classes[LINKG_LINK_TX_BATCH_SIZE_DEFAULT];
    uint32_t              accepted_count;
    uint32_t              item_index;
    uint32_t              index;
    int                   ret;

    for (index = 0U; index < count; index++)
    {
        item_index = offset + index;
        results[item_index] = -EINPROGRESS;

        ret = _linkg_link_validate_tx_packet(packets[item_index]);
        if (ret != 0)
        {
            results[item_index] = ret;
            continue;
        }

        packet_classes[index] = _linkg_link_tx_class(packets[item_index]);
    }

    accepted_count = 0U;

    // 同一内部批次固定按照实时、视频、普通数据顺序提交。
    accepted_count += _linkg_link_send_class(link, path, destination, packets, packet_classes, count, offset, LINKG_LINK_TX_CLASS_REALTIME, results);
    accepted_count += _linkg_link_send_class(link, path, destination, packets, packet_classes, count, offset, LINKG_LINK_TX_CLASS_VIDEO, results);
    accepted_count += _linkg_link_send_class(link, path, destination, packets, packet_classes, count, offset, LINKG_LINK_TX_CLASS_DATA, results);

    return accepted_count;
}

/****************************** 数据发送 ******************************/

/**
 * @brief 通过指定链路同步批量提交数据包。
 *
 * @note packet、path和destination仅在本次调用期间由Link基类借用。
 *       具体链路如果需要在send_batch返回以后继续持有packet或path，
 *       必须在返回前显式增加对应引用。
 *
 * @return 小于0表示整个batch未进入正常提交流程；
 *         大于等于0表示具体链路已经接受发送责任的数据包数量。
 *
 * @note 返回值大于等于0时results中的全部元素均有效：
 *       0表示具体链路已经接受该数据包的发送责任；
 *       负数表示该数据包被拒绝，具体链路不得在返回后继续持有该数据包。
 */
int linkg_link_submit_batch(linkg_link_t *link, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_link_runtime_t *runtime;
    uint32_t              accepted_count;
    uint32_t              chunk_count;
    uint32_t              offset;

    if (link == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (link->runtime == NULL || link->ops == NULL || link->ops->send_batch == NULL || link->id == LINKG_LINK_ID_INVALID)
    {
        return -ENODEV;
    }

    if (path->link_id != link->id)
    {
        return -EXDEV;
    }

    runtime = link->runtime;

    // STOPPING后快速拒绝新的同步提交，避免close写锁持续等待新的reader。
    if (atomic_load(&link->state) != LINKG_LINK_STATE_RUNNING)
    {
        return -ENETDOWN;
    }

    pthread_rwlock_rdlock(&runtime->io_lock);

    // 获取生命周期读锁后再次确认链路仍然处于可发送状态。
    if (atomic_load(&link->state) != LINKG_LINK_STATE_RUNNING || !runtime->opened)
    {
        pthread_rwlock_unlock(&runtime->io_lock);
        return -ENETDOWN;
    }

    if (!linkg_path_is_active(path))
    {
        pthread_rwlock_unlock(&runtime->io_lock);
        return -ENODEV;
    }

    accepted_count = 0U;
    offset         = 0U;

    while (offset < count)
    {
        chunk_count = count - offset;

        if (chunk_count > runtime->tx_batch_size)
        {
            chunk_count = runtime->tx_batch_size;
        }

        /**
         * 当前内部临时数组容量固定为LINKG_LINK_TX_BATCH_SIZE_DEFAULT，
         * 因此单个chunk不能超过该容量。
         */
        if (chunk_count > LINKG_LINK_TX_BATCH_SIZE_DEFAULT)
        {
            chunk_count = LINKG_LINK_TX_BATCH_SIZE_DEFAULT;
        }

        accepted_count += _linkg_link_submit_chunk(link, path, destination, packets, offset, chunk_count, results);
        offset += chunk_count;
    }

    pthread_rwlock_unlock(&runtime->io_lock);

    return (int)accepted_count;
}

/**
 * @brief 通过指定链路同步发送单个数据包。
 */
int linkg_link_submit(linkg_link_t *link, linkg_path_t *path, const linkg_path_endpoint_t *destination, linkg_packet_t *packet)
{
    linkg_packet_t *packets[1];
    int             results[1];
    int             ret;

    if (packet == NULL)
    {
        return -EINVAL;
    }

    packets[0] = packet;
    results[0] = -EINPROGRESS;

    ret = linkg_link_submit_batch(link, path, destination, packets, 1U, results);
    if (ret < 0)
    {
        return ret;
    }

    return results[0];
}
