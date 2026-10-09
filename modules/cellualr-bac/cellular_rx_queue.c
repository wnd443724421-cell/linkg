/**
 * @file cellular_rx_queue.c
 * @brief LinkG蜂窝接收等待队列实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-10
 */

#include "cellular_rx_queue.h"

#include <stdatomic.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_packet_pool.h"

/****************************** 内部类型 ******************************/

/**
 * @brief 蜂窝接收SPSC等待队列。
 */
struct linkg_cellular_rx_queue
{
    linkg_cellular_rx_queue_item_t *items;    // 固定容量Queue元素数组
    uint32_t                        capacity; // Queue最大元素数量
    _Atomic uint32_t                head;     // Consumer单调读取位置
    _Atomic uint32_t                tail;     // Producer单调写入位置
};

/****************************** 生命周期 ******************************/

/**
 * @brief 创建蜂窝接收等待队列。
 */
linkg_cellular_rx_queue_t *linkg_cellular_rx_queue_create(uint32_t capacity)
{
    linkg_cellular_rx_queue_t *queue;

    if (capacity == 0U || capacity > (UINT32_MAX / 2U))
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

    atomic_init(&queue->head, 0U);
    atomic_init(&queue->tail, 0U);

    return queue;
}

/**
 * @brief 销毁蜂窝接收等待队列。
 *
 * 调用前Producer和Consumer必须已经停止，Queue中剩余Packet基础引用由本函数释放。
 */
void linkg_cellular_rx_queue_destroy(linkg_cellular_rx_queue_t *queue)
{
    if (queue == NULL)
    {
        return;
    }

    linkg_cellular_rx_queue_clear(queue);

    free(queue->items);
    queue->items    = NULL;
    queue->capacity = 0U;

    free(queue);
}

/**
 * @brief 清空蜂窝接收等待队列。
 *
 * 调用前Producer和Consumer必须已经停止，Queue中剩余Packet基础引用由本函数释放。
 */
void linkg_cellular_rx_queue_clear(linkg_cellular_rx_queue_t *queue)
{
    linkg_cellular_rx_queue_item_t *item;
    uint32_t                        head;
    uint32_t                        tail;
    uint32_t                        index;

    if (queue == NULL || queue->items == NULL)
    {
        return;
    }

    head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);

    while (head != tail)
    {
        index = head % queue->capacity;
        item  = &queue->items[index];

        if (item->packet != NULL)
        {
            linkg_packet_release(item->packet);
        }

        head++;
    }

    memset(queue->items, 0, (size_t)queue->capacity * sizeof(*queue->items));

    atomic_store_explicit(&queue->head, 0U, memory_order_relaxed);
    atomic_store_explicit(&queue->tail, 0U, memory_order_relaxed);
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取当前Queue元素数量。
 */
uint32_t linkg_cellular_rx_queue_count(const linkg_cellular_rx_queue_t *queue)
{
    uint32_t head;
    uint32_t tail;
    uint32_t count;

    if (queue == NULL)
    {
        return 0U;
    }

    head  = atomic_load_explicit(&queue->head, memory_order_acquire);
    tail  = atomic_load_explicit(&queue->tail, memory_order_acquire);
    count = tail - head;

    if (count > queue->capacity)
    {
        count = queue->capacity;
    }

    return count;
}

/**
 * @brief 获取Queue最大元素数量。
 */
uint32_t linkg_cellular_rx_queue_capacity(const linkg_cellular_rx_queue_t *queue)
{
    if (queue == NULL)
    {
        return 0U;
    }

    return queue->capacity;
}

/**
 * @brief 获取当前Queue剩余可用元素数量。
 */
uint32_t linkg_cellular_rx_queue_available(const linkg_cellular_rx_queue_t *queue)
{
    uint32_t count;

    if (queue == NULL)
    {
        return 0U;
    }

    count = linkg_cellular_rx_queue_count(queue);

    return queue->capacity - count;
}

/****************************** 队列操作 ******************************/

/**
 * @brief 批量加入已经接收完成的Packet。
 *
 * 本函数仅允许单个Producer调用。成功加入的连续前缀Packet基础引用从调用方转移给Queue，
 * 未加入元素仍由调用方持有，不额外增加Packet引用。
 */
uint32_t linkg_cellular_rx_queue_push_batch(linkg_cellular_rx_queue_t *queue, const linkg_cellular_rx_queue_item_t *items, uint32_t count)
{
    uint32_t head;
    uint32_t tail;
    uint32_t used;
    uint32_t available;
    uint32_t push_count;
    uint32_t offset;
    uint32_t first_count;

    if (queue == NULL || items == NULL || count == 0U)
    {
        return 0U;
    }

    tail = atomic_load_explicit(&queue->tail, memory_order_relaxed);
    head = atomic_load_explicit(&queue->head, memory_order_acquire);

    used = tail - head;
    if (used >= queue->capacity)
    {
        return 0U;
    }

    available  = queue->capacity - used;
    push_count = count;

    if (push_count > available)
    {
        push_count = available;
    }

    offset      = tail % queue->capacity;
    first_count = queue->capacity - offset;

    if (first_count > push_count)
    {
        first_count = push_count;
    }

    memcpy(&queue->items[offset], items, (size_t)first_count * sizeof(*items));

    if (push_count > first_count)
    {
        memcpy(queue->items, &items[first_count], (size_t)(push_count - first_count) * sizeof(*items));
    }

    atomic_store_explicit(&queue->tail, tail + push_count, memory_order_release);

    return push_count;
}

/**
 * @brief 批量取出已经接收完成的Packet。
 *
 * 本函数仅允许单个Consumer调用。成功取出的Packet基础引用从Queue转移给调用方，
 * Queue不再持有对应Packet引用。
 */
uint32_t linkg_cellular_rx_queue_pop_batch(linkg_cellular_rx_queue_t *queue, linkg_cellular_rx_queue_item_t *items, uint32_t capacity)
{
    uint32_t head;
    uint32_t tail;
    uint32_t available;
    uint32_t pop_count;
    uint32_t offset;
    uint32_t first_count;

    if (queue == NULL || items == NULL || capacity == 0U)
    {
        return 0U;
    }

    head = atomic_load_explicit(&queue->head, memory_order_relaxed);
    tail = atomic_load_explicit(&queue->tail, memory_order_acquire);

    available = tail - head;
    if (available == 0U)
    {
        return 0U;
    }

    pop_count = capacity;
    if (pop_count > available)
    {
        pop_count = available;
    }

    offset      = head % queue->capacity;
    first_count = queue->capacity - offset;

    if (first_count > pop_count)
    {
        first_count = pop_count;
    }

    memcpy(items, &queue->items[offset], (size_t)first_count * sizeof(*items));

    if (pop_count > first_count)
    {
        memcpy(&items[first_count], queue->items, (size_t)(pop_count - first_count) * sizeof(*items));
    }

    atomic_store_explicit(&queue->head, head + pop_count, memory_order_release);

    return pop_count;
}
