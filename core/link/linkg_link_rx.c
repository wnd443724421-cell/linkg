/**
 * @file linkg_link_rx.c
 * @brief LinkG链路接收处理实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#include "linkg_link.h"

#include <errno.h>
#include <poll.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "linkg_time.h"

#include "linkg_link_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_LINK_RX_POLL_TIMEOUT_MS 1000 // 接收线程最大轮询等待时间
#define LINKG_LINK_RX_POLL_FD_COUNT   2U   // 接收线程等待描述符数量
#define LINKG_LINK_RX_POOL_RETRY_MS   1U   // 数据包池耗尽重试间隔

/****************************** 内部辅助 ******************************/

/**
 * @brief 释放指定范围内接收元素持有的数据包基础引用。
 *
 * @note 接收批次中的Packet基础引用由Link RX持有，本函数负责对称释放并清空接收元素。
 */
static void _linkg_link_release_rx_items(linkg_link_rx_item_t *items, uint32_t offset, uint32_t count)
{
    uint32_t end;
    uint32_t index;

    if (items == NULL || count == 0U)
    {
        return;
    }

    end = offset + count;

    for (index = offset; index < end; index++)
    {
        if (items[index].packet != NULL)
        {
            linkg_packet_release(items[index].packet);
        }

        items[index].packet = NULL;
        memset(&items[index].source, 0, sizeof(items[index].source));
    }
}

/****************************** 数据接收 ******************************/

/**
 * @brief 运行链路批量接收线程。
 *
 * @note Link RX持有每轮从Packet Pool申请的数据包基础引用。
 *       receive回调仅同步借用有效Packet，回调需要异步保存Packet时必须自行增加引用。
 */
void linkg_link_rx_thread(linkg_thread_t *thread, void *user_data)
{
    struct pollfd        descriptors[LINKG_LINK_RX_POLL_FD_COUNT];
    linkg_link_runtime_t *runtime;
    linkg_link_t         *link;
    uint32_t              allocated_count;
    uint32_t              received_count;
    uint32_t              index;
    int                   wakeup_fd;
    int                   link_fd;
    int                   ret;

    if (thread == NULL || user_data == NULL)
    {
        return;
    }

    link = user_data;

    if (link->runtime == NULL || link->packet_pool == NULL || link->ops == NULL)
    {
        return;
    }

    if (link->ops->get_rx_fd == NULL || link->ops->receive_batch == NULL)
    {
        return;
    }

    runtime = link->runtime;

    wakeup_fd = linkg_thread_get_wakeup_fd(thread);
    if (wakeup_fd < 0)
    {
        LINKG_LOG_ERROR("get link RX wakeup fd failed, link=%s, error=%d", link->name, wakeup_fd);
        return;
    }

    link_fd = link->ops->get_rx_fd(link);
    if (link_fd < 0)
    {
        LINKG_LOG_ERROR("get link RX fd failed, link=%s, error=%d", link->name, link_fd);
        return;
    }

    memset(descriptors, 0, sizeof(descriptors));

    descriptors[0].fd     = wakeup_fd;
    descriptors[0].events = POLLIN;

    descriptors[1].fd     = link_fd;
    descriptors[1].events = POLLIN;

    LINKG_LOG_INFO("link RX thread entered, link=%s", link->name);

    while (linkg_thread_is_running(thread))
    {
        descriptors[0].revents = 0;
        descriptors[1].revents = 0;

        do
        {
            ret = poll(descriptors, LINKG_LINK_RX_POLL_FD_COUNT, LINKG_LINK_RX_POLL_TIMEOUT_MS);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (ret < 0)
        {
            if (linkg_thread_is_running(thread))
            {
                LINKG_LOG_ERROR("poll link RX descriptors failed, link=%s, error=%d", link->name, -errno);
            }

            break;
        }

        if ((descriptors[0].revents & POLLIN) != 0)
        {
            ret = linkg_thread_clear_wakeup(thread);
            if (ret != 0)
            {
                if (linkg_thread_is_running(thread))
                {
                    LINKG_LOG_ERROR("clear link RX wakeup failed, link=%s, error=%d", link->name, ret);
                }

                break;
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        if ((descriptors[0].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_ERROR("link RX wakeup fd failed, link=%s, revents=0x%x", link->name, descriptors[0].revents);
            break;
        }

        if ((descriptors[1].revents & (POLLERR | POLLHUP | POLLNVAL)) != 0)
        {
            LINKG_LOG_ERROR("link RX fd failed, link=%s, revents=0x%x", link->name, descriptors[1].revents);
            break;
        }

        if ((descriptors[1].revents & POLLIN) == 0)
        {
            continue;
        }

        // 持续接收直到当前接收队列暂时排空。
        while (linkg_thread_is_running(thread))
        {
            allocated_count = linkg_packet_pool_alloc_batch(link->packet_pool, runtime->rx_packets, runtime->rx_batch_size);
            if (allocated_count == 0U)
            {
                // 避免Packet Pool耗尽时持续触发可读轮询形成忙等。
                linkg_time_sleep_ms(LINKG_LINK_RX_POOL_RETRY_MS);
                break;
            }

            for (index = 0U; index < allocated_count; index++)
            {
                runtime->rx_items[index].packet = runtime->rx_packets[index];
                memset(&runtime->rx_items[index].source, 0, sizeof(runtime->rx_items[index].source));

                runtime->rx_packets[index] = NULL;
            }

            ret = link->ops->receive_batch(link, runtime->rx_items, allocated_count);
            if (ret < 0)
            {
                LINKG_LOG_ERROR("receive link batch failed, link=%s, capacity=%u, error=%d",
                                link->name,
                                allocated_count,
                                ret);

                _linkg_link_release_rx_items(runtime->rx_items, 0U, allocated_count);
                break;
            }

            if ((uint32_t)ret > allocated_count)
            {
                LINKG_LOG_ERROR("invalid link RX batch result, link=%s, received=%d, capacity=%u",
                                link->name,
                                ret,
                                allocated_count);

                _linkg_link_release_rx_items(runtime->rx_items, 0U, allocated_count);
                break;
            }

            received_count = (uint32_t)ret;

            if (received_count > 0U && runtime->receive != NULL)
            {
                // 上层同步借用有效Packet，异步保存时由上层自行增加引用。
                runtime->receive(link, runtime->rx_items, received_count, runtime->receive_user_data);
            }

            // 本轮申请的全部Packet基础引用始终由Link RX统一释放。
            _linkg_link_release_rx_items(runtime->rx_items, 0U, allocated_count);

            if (received_count == 0U)
            {
                break;
            }
        }
    }

    LINKG_LOG_INFO("link RX thread exited, link=%s", link->name);
}
