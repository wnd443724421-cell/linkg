/**
 * @file cellular_tx_queue.c
 * @brief LinkG蜂窝发送等待队列实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-10
 */

#include "cellular_tx_queue.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_packet_pool.h"

/****************************** 内部类型 ******************************/

/**
 * @brief 蜂窝发送等待队列运行上下文。
 */
struct linkg_cellular_tx_queue
{
    linkg_cellular_tx_queue_item_t *items;    // 固定容量环形存储区
    uint32_t                        capacity; // 最大元素数量
    uint32_t                        count;    // 当前元素数量
    uint32_t                        head;     // 当前队首索引
    uint32_t                        tail;     // 下一次入队索引
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取环形队列中的下一个索引。
 */
static uint32_t _linkg_cellular_tx_queue_next(const linkg_cellular_tx_queue_t *queue, uint32_t index)
{
    index++;

    if (index >= queue->capacity)
    {
        index = 0U;
    }

    return index;
}

/**
 * @brief 释放Queue持有的单个元素引用并清空元素。
 *
 * Queue分别持有Packet和Path的一个引用，本函数负责对称释放。
 */
static void _linkg_cellular_tx_queue_release_item(linkg_cellular_tx_queue_item_t *item)
{
    if (item == NULL)
    {
        return;
    }

    if (item->packet != NULL)
    {
        linkg_packet_release(item->packet);
    }

    if (item->path != NULL)
    {
        linkg_path_release(item->path);
    }

    memset(item, 0, sizeof(*item));
}

/****************************** 生命周期 ******************************/

/**
 * @brief 创建固定容量蜂窝发送等待队列。
 */
linkg_cellular_tx_queue_t *linkg_cellular_tx_queue_create(uint32_t capacity)
{
    linkg_cellular_tx_queue_t *queue;

    if (capacity == 0U)
    {
        return NULL;
    }

    queue = calloc(1, sizeof(*queue));
    if (queue == NULL)
    {
        return NULL;
    }

    queue->items = calloc(capacity, sizeof(*queue->items));
    if (queue->items == NULL)
    {
        free(queue);
        return NULL;
    }

    queue->capacity = capacity;

    return queue;
}

/**
 * @brief 销毁蜂窝发送等待队列并释放全部持有引用。
 *
 * 调用前不得再有其他线程访问Queue。
 */
void linkg_cellular_tx_queue_destroy(linkg_cellular_tx_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    linkg_cellular_tx_queue_clear(queue);

    free(queue->items);
    queue->items = NULL;

    free(queue);
}

/**
 * @brief 清空蜂窝发送等待队列并释放全部持有引用。
 *
 * 调用方必须保证当前没有并发Queue操作。
 */
void linkg_cellular_tx_queue_clear(linkg_cellular_tx_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    while (queue->count > 0U)
    {
        _linkg_cellular_tx_queue_release_item(&queue->items[queue->head]);

        queue->head = _linkg_cellular_tx_queue_next(queue, queue->head);
        queue->count--;
    }

    queue->head = 0U;
    queue->tail = 0U;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取当前等待队列元素数量。
 */
uint32_t linkg_cellular_tx_queue_count(const linkg_cellular_tx_queue_t *queue)
{
    if (queue == NULL)
    {
        return 0U;
    }

    return queue->count;
}

/**
 * @brief 获取等待队列最大容量。
 */
uint32_t linkg_cellular_tx_queue_capacity(const linkg_cellular_tx_queue_t *queue)
{
    if (queue == NULL)
    {
        return 0U;
    }

    return queue->capacity;
}

/**
 * @brief 获取等待队列剩余可用容量。
 */
uint32_t linkg_cellular_tx_queue_available(const linkg_cellular_tx_queue_t *queue)
{
    if (queue == NULL)
    {
        return 0U;
    }

    return queue->capacity - queue->count;
}

/****************************** 队列操作 ******************************/

/**
 * @brief 批量将待发送数据加入队尾。
 *
 * 成功入队的每个元素由Queue独立持有一个Packet引用和一个Path引用。
 * 本接口要求当前Queue能够完整容纳整个batch，不执行部分入队。
 */
int linkg_cellular_tx_queue_push_batch(linkg_cellular_tx_queue_t *queue, linkg_packet_t *const *packets, uint32_t count, linkg_path_t *path, const linkg_path_endpoint_t *destination, uint64_t enqueue_us, uint32_t *pushed_count)
{
    linkg_cellular_tx_queue_item_t *item;
    uint32_t                        index;
    int                             ret;

    if (queue == NULL || packets == NULL || path == NULL || destination == NULL || pushed_count == NULL)
    {
        return -EINVAL;
    }

    *pushed_count = 0U;

    if (count == 0U)
    {
        return 0;
    }

    if (count > linkg_cellular_tx_queue_available(queue))
    {
        return -ENOSPC;
    }

    for (index = 0U; index < count; index++)
    {
        if (packets[index] == NULL)
        {
            return -EINVAL;
        }
    }

    ret = linkg_path_acquire_batch(path, count);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        linkg_packet_retain(packets[index]);

        item = &queue->items[queue->tail];

        item->packet      = packets[index];
        item->path        = path;
        item->destination = *destination;
        item->enqueue_us  = enqueue_us;

        queue->tail = _linkg_cellular_tx_queue_next(queue, queue->tail);
        queue->count++;
    }

    *pushed_count = count;

    return 0;
}

/**
 * @brief 批量复制队首元素但不从Queue移除。
 *
 * 输出items中的Packet和Path仅借用Queue当前持有的引用。
 */
uint32_t linkg_cellular_tx_queue_peek_batch(const linkg_cellular_tx_queue_t *queue, linkg_cellular_tx_queue_item_t *items, uint32_t capacity)
{
    uint32_t copy_count;
    uint32_t queue_index;
    uint32_t index;

    if (queue == NULL || items == NULL || capacity == 0U)
    {
        return 0U;
    }

    copy_count = queue->count;
    if (copy_count > capacity)
    {
        copy_count = capacity;
    }

    queue_index = queue->head;

    for (index = 0U; index < copy_count; index++)
    {
        items[index] = queue->items[queue_index];
        queue_index  = _linkg_cellular_tx_queue_next(queue, queue_index);
    }

    return copy_count;
}

/**
 * @brief 从队首丢弃指定数量元素并释放Queue持有的引用。
 */
int linkg_cellular_tx_queue_discard_batch(linkg_cellular_tx_queue_t *queue, uint32_t count)
{
    uint32_t index;

    if (queue == NULL)
    {
        return -EINVAL;
    }

    if (count > queue->count)
    {
        return -ERANGE;
    }

    for (index = 0U; index < count; index++)
    {
        _linkg_cellular_tx_queue_release_item(&queue->items[queue->head]);

        queue->head = _linkg_cellular_tx_queue_next(queue, queue->head);
        queue->count--;
    }

    if (queue->count == 0U)
    {
        queue->head = 0U;
        queue->tail = 0U;
    }

    return 0;
}

/**
 * @brief 清理全部引用指定Path的等待元素并保持剩余元素FIFO顺序。
 *
 * @note 本接口不提供内部并发保护，调用方必须保证当前Queue操作已经串行化。
 *       被清理元素持有的Packet和Path引用会在函数返回前全部释放。
 *       只比较Path指针，不读取目标Path字段，也不清理其他失效或空元素。
 */
int linkg_cellular_tx_queue_purge_path(linkg_cellular_tx_queue_t *queue, linkg_path_t *path, uint32_t *purged_count)
{
    linkg_cellular_tx_queue_item_t *item;
    uint32_t                        original_count;
    uint32_t                        read_index;
    uint32_t                        write_index;
    uint32_t                        removed_count;
    uint32_t                        index;

    if (queue == NULL || path == NULL || purged_count == NULL)
    {
        return -EINVAL;
    }

    *purged_count = 0U;

    if (queue->count == 0U)
    {
        return 0;
    }

    original_count = queue->count;
    read_index     = queue->head;
    write_index    = queue->head;
    removed_count  = 0U;

    for (index = 0U; index < original_count; index++)
    {
        item = &queue->items[read_index];

        if (item->path == path)
        {
            _linkg_cellular_tx_queue_release_item(item);
            removed_count++;
        }
        else
        {
            if (write_index != read_index)
            {
                queue->items[write_index] = *item;
                memset(item, 0, sizeof(*item));
            }

            write_index = _linkg_cellular_tx_queue_next(queue, write_index);
        }

        read_index = _linkg_cellular_tx_queue_next(queue, read_index);
    }

    queue->count = original_count - removed_count;

    if (queue->count == 0U)
    {
        queue->head = 0U;
        queue->tail = 0U;
    }
    else
    {
        queue->tail = write_index;
    }

    *purged_count = removed_count;

    return 0;
}
