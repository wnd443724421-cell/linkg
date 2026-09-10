/**
 * @file link_tx.c
 * @brief LinkG链路发送处理实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-10
 */

#include "linkg_link.h"

#include <errno.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"

#include "link_internal.h"

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
 * @brief 校验链路发送业务类别。
 */
static bool _linkg_link_tx_class_valid(linkg_link_tx_class_t tx_class)
{
    return tx_class >= LINKG_LINK_TX_CLASS_REALTIME && tx_class < LINKG_LINK_TX_CLASS_COUNT;
}

/**
 * @brief 判断指定批次边界是否位于FIRST和LAST之间。
 */
static bool _linkg_link_tx_split_group(const linkg_packet_t *first, const linkg_packet_t *last)
{
    uint32_t first_group;
    uint32_t last_group;

    if (first == NULL || last == NULL)
    {
        return false;
    }

    first_group = first->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;
    last_group  = last->flags & LINKG_PACKET_FLAG_TX_GROUP_MASK;

    return first_group == LINKG_PACKET_FLAG_TX_GROUP_FIRST &&
           last_group == LINKG_PACKET_FLAG_TX_GROUP_LAST;
}

/**
 * @brief 计算下一次具体Link发送批次大小。
 *
 * @note 批次不会在相邻FIRST和LAST之间切开。
 */
static uint32_t _linkg_link_tx_get_chunk_count(linkg_packet_t *const *packets, uint32_t offset, uint32_t count, uint32_t batch_size)
{
    uint32_t remaining;
    uint32_t chunk_count;

    remaining   = count - offset;
    chunk_count = remaining > batch_size ? batch_size : remaining;

    if (chunk_count == remaining)
    {
        return chunk_count;
    }

    if (_linkg_link_tx_split_group(packets[offset + chunk_count - 1U], packets[offset + chunk_count]))
    {
        chunk_count--;
    }

    return chunk_count;
}

/**
 * @brief 完成一次具体Link批次结果检查并返回接管数量。
 */
static uint32_t _linkg_link_tx_finalize_chunk(linkg_link_t *link, linkg_link_tx_class_t tx_class, int ret, int *results, uint32_t count)
{
    uint32_t accepted_count;
    uint32_t index;

    accepted_count = 0U;

    for (index = 0U; index < count; index++)
    {
        if (results[index] == -EINPROGRESS)
        {
            LINKG_LOG_ERROR("link TX result not completed, link=%s, class=%d, index=%u",
                            link->name,
                            (int)tx_class,
                            index);

            results[index] = -EIO;
        }

        if (results[index] == 0)
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

/****************************** 数据发送 ******************************/

/**
 * @brief 通过指定链路同步批量提交同一业务类别的数据包。
 *
 * @note 当前batch必须已经由上层确定唯一的Path、下一跳Endpoint和业务类别。
 *       Link基类不执行Packet业务分类或QoS重排。
 *
 * @note 当count超过具体Link的tx_batch_size时，
 *       Link基类按照原始Packet顺序拆分为多个具体Link批次。
 *       相邻FIRST和LAST不会被拆到两个具体Link批次。
 *
 * @note packet、path和destination仅在本次调用期间由Link基类借用。
 *       具体链路如果需要在send_batch返回以后继续持有packet或path，
 *       必须在返回前显式增加对应引用。
 *
 * @return 小于0表示全部具体Link批次均未进入正常提交流程；
 *         大于等于0表示具体链路累计接受发送责任的数据包数量。
 *
 * @note 返回值大于等于0时results中的全部元素均有效：
 *       0表示具体链路已经接受该数据包的发送责任；
 *       负数表示该数据包被拒绝。
 */
int linkg_link_submit_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_link_runtime_t *runtime;
    uint32_t              accepted_count;
    uint32_t              chunk_accepted;
    uint32_t              chunk_count;
    uint32_t              offset;
    uint32_t              index;
    int                   first_error;
    int                   ret;
    bool                  processed;

    if (link == NULL || path == NULL || destination == NULL || packets == NULL || results == NULL || count == 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_link_tx_class_valid(tx_class))
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

    if (runtime->tx_batch_size == 0U)
    {
        return -EINVAL;
    }

    for (index = 0U; index < count; index++)
    {
        ret = _linkg_link_validate_tx_packet(packets[index]);
        if (ret != 0)
        {
            return ret;
        }

        results[index] = -EINPROGRESS;
    }

    if (atomic_load(&link->state) != LINKG_LINK_STATE_RUNNING)
    {
        return -ENETDOWN;
    }

    pthread_rwlock_rdlock(&runtime->io_lock);

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
    first_error    = 0;
    processed      = false;
    offset         = 0U;

    while (offset < count)
    {
        chunk_count = _linkg_link_tx_get_chunk_count(packets, offset, count, runtime->tx_batch_size);
        if (chunk_count == 0U)
        {
            first_error = -EOVERFLOW;

            for (index = offset; index < count; index++)
            {
                results[index] = first_error;
            }

            break;
        }

        ret = link->ops->send_batch(link, path, tx_class, destination, &packets[offset], chunk_count, &results[offset]);
        if (ret < 0)
        {
            for (index = 0U; index < chunk_count; index++)
            {
                results[offset + index] = ret;
            }

            if (first_error == 0)
            {
                first_error = ret;
            }

            for (index = offset + chunk_count; index < count; index++)
            {
                results[index] = ret;
            }

            break;
        }

        processed = true;

        chunk_accepted = _linkg_link_tx_finalize_chunk(link, tx_class, ret, &results[offset], chunk_count);
        accepted_count += chunk_accepted;

        offset += chunk_count;
    }

    pthread_rwlock_unlock(&runtime->io_lock);

    if (!processed && first_error != 0)
    {
        return first_error;
    }

    return (int)accepted_count;
}

/**
 * @brief 通过指定链路同步提交单个指定业务类别的数据包。
 */
int linkg_link_submit(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *packet)
{
    linkg_packet_t *packets[1];
    int             results[1];
    int             ret;

    if (packet == NULL)
    {
        return -EINVAL;
    }

    packets[0] = packet;

    ret = linkg_link_submit_batch(link, path, tx_class, destination, packets, 1U, results);
    if (ret < 0)
    {
        return ret;
    }

    return results[0];
}

/****************************** 发送清理 ******************************/

/**
 * @brief 清理当前链路中引用指定Path的待发送Packet。
 *
 * @note 本接口只接受已经退出ACTIVE状态且归属于当前Link的Path。
 *       不要求Link保持RUNNING状态，只要具体链路运行资源仍处于opened状态，
 *       就允许进入具体Link清理异步发送队列。
 *
 * @note io_lock读锁负责与linkg_link_stop()中的close写锁互斥，
 *       避免清理过程中具体Link发送队列或Socket资源被并发销毁。
 *       具体Link返回后不得再读取Path字段，因为最后一个队列引用可能已释放。
 */
int linkg_link_purge_tx_path(linkg_link_t *link, linkg_path_t *path, uint32_t *purged_count)
{
    linkg_link_runtime_t *runtime;
    int                   ret;

    if (link == NULL || path == NULL || purged_count == NULL)
    {
        return -EINVAL;
    }

    *purged_count = 0U;

    if (link->runtime == NULL || link->ops == NULL || link->ops->purge_tx_path == NULL || link->id == LINKG_LINK_ID_INVALID)
    {
        return -ENODEV;
    }

    if (path->link_id != link->id)
    {
        return -EXDEV;
    }

    if (linkg_path_is_active(path))
    {
        return -EBUSY;
    }

    runtime = link->runtime;

    pthread_rwlock_rdlock(&runtime->io_lock);

    if (!runtime->opened)
    {
        pthread_rwlock_unlock(&runtime->io_lock);
        return 0;
    }

    ret = link->ops->purge_tx_path(link, path, purged_count);

    pthread_rwlock_unlock(&runtime->io_lock);

    return ret;
}
