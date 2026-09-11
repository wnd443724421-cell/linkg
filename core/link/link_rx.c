/**
 * @file link_rx.c
 * @brief LinkG链路接收处理实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#include "linkg_link.h"

#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"
#include "linkg_packet_pool.h"
#include "link_internal.h"

/****************************** 模块常量 ******************************/

#define LINKG_LINK_RX_POLL_TIMEOUT_MS    1000 // 接收线程最大轮询等待时间
#define LINKG_LINK_RX_POLL_FD_COUNT      2U   // 接收线程等待描述符数量

/****************************** 内部辅助 ******************************/

/**
 * @brief 释放接收元素持有的Packet基础引用。
 *
 * receive_batch成功返回后对应Packet基础引用归Link RX所有，
 * 上层receive回调仅同步借用。
 */
static void _linkg_link_release_rx_items(linkg_link_rx_item_t *items, uint32_t count)
{
    uint32_t index;

    if (items == NULL || count == 0U)
    {
        return;
    }

    for (index = 0U; index < count; index++)
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
 * 具体Link负责产生已经接收完成的Packet，并通过receive_batch将Packet基础引用
 * 转移给Link RX。上层receive回调仅同步借用，异步保存时必须自行增加引用。
 */
void linkg_link_rx_thread(linkg_thread_t *thread, void *user_data)
{
    struct pollfd        descriptors[LINKG_LINK_RX_POLL_FD_COUNT];
    linkg_link_runtime_t *runtime;
    linkg_link_t         *link;
    uint32_t              received_count;
    int                   wakeup_fd;
    int                   link_fd;
    int                   ret;

    if (thread == NULL || user_data == NULL)
    {
        return;
    }

    link = user_data;

    if (link->runtime == NULL || link->ops == NULL)
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

        // 持续消费直到具体Link当前已经接收完成的数据暂时排空。
        while (linkg_thread_is_running(thread))
        {
            ret = link->ops->receive_batch(link, runtime->rx_items, runtime->rx_batch_size);
            if (ret < 0)
            {
                LINKG_LOG_ERROR("receive link batch failed, link=%s, capacity=%u, error=%d",
                                link->name,
                                runtime->rx_batch_size,
                                ret);
                break;
            }

            if ((uint32_t)ret > runtime->rx_batch_size)
            {
                LINKG_LOG_ERROR("invalid link RX batch result, link=%s, received=%d, capacity=%u",
                                link->name,
                                ret,
                                runtime->rx_batch_size);

                _linkg_link_release_rx_items(runtime->rx_items, runtime->rx_batch_size);
                break;
            }

            received_count = (uint32_t)ret;
            if (received_count == 0U)
            {
                break;
            }

            if (runtime->receive != NULL)
            {
                // 上层同步借用Packet，异步保存时由上层自行增加引用。
                runtime->receive(link, runtime->rx_items, received_count, runtime->receive_user_data);
            }

            // receive_batch转移给Link RX的Packet基础引用在回调返回后统一释放。
            _linkg_link_release_rx_items(runtime->rx_items, received_count);
        }
    }

    LINKG_LOG_INFO("link RX thread exited, link=%s", link->name);
}
