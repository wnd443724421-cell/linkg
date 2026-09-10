/**
 * @file link_tx.c
 * @brief LinkG链路发送处理实现
 * @author Dawn
 * @version 1.2.0
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

/****************************** 数据发送 ******************************/

/**
 * @brief 通过指定链路同步批量提交同一业务类别的数据包。
 *
 * @note 当前batch必须已经由上层确定唯一的Path、下一跳Endpoint和业务类别。
 *       Link基类不再执行Packet业务分类、QoS重排或二次分批。
 *
 * @note packet、path和destination仅在本次调用期间由Link基类借用。
 *       具体链路如果需要在send_batch返回以后继续持有packet或path，
 *       必须在返回前显式增加对应引用。
 *
 * @return 小于0表示整个batch未进入具体链路正常提交流程；
 *         大于等于0表示具体链路已经接受发送责任的数据包数量。
 *
 * @note 返回值大于等于0时results中的全部元素均有效：
 *       0表示具体链路已经接受该数据包的发送责任；
 *       负数表示该数据包被拒绝，具体链路不得在返回后继续持有该数据包。
 */
int linkg_link_submit_batch(linkg_link_t *link, linkg_path_t *path, linkg_link_tx_class_t tx_class, const linkg_path_endpoint_t *destination, linkg_packet_t *const *packets, uint32_t count, int *results)
{
    linkg_link_runtime_t *runtime;
    uint32_t              accepted_count;
    uint32_t              index;
    int                   ret;

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

    if (count > runtime->tx_batch_size)
    {
        return -EOVERFLOW;
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

    /**
     * 一个Link batch就是一次具体链路提交单元。
     * 上层已经确定业务类别、Path、下一跳和Packet顺序，
     * Link基类不得再次分类、重排或循环拆成多个物理发送调度。
     */
    ret = link->ops->send_batch(link, path, tx_class, destination, packets, count, results);
    if (ret < 0)
    {
        for (index = 0U; index < count; index++)
        {
            results[index] = ret;
        }
    }

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

    pthread_rwlock_unlock(&runtime->io_lock);

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
    results[0] = -EINPROGRESS;

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

    /**
     * Link已经关闭时具体TX stop/close路径应当已经清空等待队列，
     * 此时没有运行期异步引用需要继续清理。
     */
    if (!runtime->opened)
    {
        pthread_rwlock_unlock(&runtime->io_lock);
        return 0;
    }

    ret = link->ops->purge_tx_path(link, path, purged_count);

    pthread_rwlock_unlock(&runtime->io_lock);

    return ret;
}
