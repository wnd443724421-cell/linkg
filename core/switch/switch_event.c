/**
 * @file switch_event.c
 * @brief LinkG链路切换内部事件实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-18
 */

#include "switch_event.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_system_resources.h"
#include "linkg_thread.h"

#include "switch_internal.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断Switch内部事件类型是否合法。
 */
static bool _linkg_switch_event_type_valid(linkg_switch_event_type_t type)
{
    return type > LINKG_SWITCH_EVENT_NONE &&
           type < LINKG_SWITCH_EVENT_COUNT;
}

/**
 * @brief 判断Switch事件基础字段是否合法。
 */
static bool _linkg_switch_event_valid(const linkg_switch_event_t *event)
{
    if (event == NULL)
    {
        return false;
    }

    if (!_linkg_switch_event_type_valid(event->type))
    {
        return false;
    }

    if (event->peer_node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        event->peer_node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return false;
    }

    if (event->message_id == LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID)
    {
        return false;
    }

    return true;
}

/**
 * @brief 获取循环队列中的下一个槽位索引。
 */
static uint32_t _linkg_switch_event_next_index(uint32_t index)
{
    index++;

    if (index >= LINKG_SWITCH_EVENT_QUEUE_CAPACITY)
    {
        index = 0U;
    }

    return index;
}

/****************************** 事件操作 ******************************/

/**
 * @brief 将控制事件加入Switch内部队列并立即唤醒Worker。
 *
 * 事件内容按值复制，队列不持有外部Packet、Wire Buffer或其它对象引用。
 */
int linkg_switch_event_post(const linkg_switch_event_t *event)
{
    linkg_switch_event_queue_t *queue;
    linkg_switch_peer_runtime_t *peer;
    uint32_t                    slot;
    int                         ret;

    if (!_linkg_switch_event_valid(event))
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (!g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    peer = linkg_switch_find_peer_locked(event->peer_node_id);
    if (peer == NULL || peer->generation == LINKG_SWITCH_PEER_GENERATION_INVALID)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOENT;
    }

    queue = &g_switch.event_queue;

    if (queue->count >= LINKG_SWITCH_EVENT_QUEUE_CAPACITY)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENOSPC;
    }

    slot                               = queue->tail;
    queue->items[slot]                 = *event;
    queue->items[slot].peer_generation = peer->generation;
    queue->tail                        = _linkg_switch_event_next_index(queue->tail);
    queue->count++;

    /**
     * 入队和Worker唤醒保持同一个Switch锁保护。
     * linkg_thread_wakeup仅向非阻塞eventfd写入信号，
     * 失败时可安全回滚尚未对Worker可见的本次入队。
     */
    ret = linkg_thread_wakeup(&g_switch.thread);
    if (ret != 0)
    {
        queue->tail = slot;

        if (queue->count != 0U)
        {
            queue->count--;
        }

        memset(&queue->items[slot], 0, sizeof(queue->items[slot]));

        pthread_mutex_unlock(&g_switch.lock);
        return ret;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}


/**
 * @brief 从Switch内部队列取出最早控制事件，调用方持有Switch锁。
 */
bool linkg_switch_event_pop_locked(linkg_switch_event_t *event)
{
    linkg_switch_event_queue_t *queue;
    uint32_t                    slot;

    if (event == NULL)
    {
        return false;
    }

    memset(event, 0, sizeof(*event));

    queue = &g_switch.event_queue;

    if (queue->count == 0U)
    {
        return false;
    }

    slot   = queue->head;
    *event = queue->items[slot];

    memset(&queue->items[slot], 0, sizeof(queue->items[slot]));

    queue->head = _linkg_switch_event_next_index(queue->head);
    queue->count--;

    return true;
}

/**
 * @brief 清空Switch内部事件队列，调用方持有Switch锁。
 */
void linkg_switch_event_reset_locked(void)
{
    memset(&g_switch.event_queue, 0, sizeof(g_switch.event_queue));
}
