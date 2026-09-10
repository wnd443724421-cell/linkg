/**
 * @file link_runtime.c
 * @brief LinkG链路运行资源实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-09-10
 */

#include "link_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_log.h"

/****************************** 模块常量 ******************************/

#define LINKG_LINK_RX_THREAD_NAME      "link-rx" // 链路接收线程名称
#define LINKG_LINK_RX_THREAD_CPU_CORE  1         // RX线程绑定CPU1

/****************************** 运行资源 ******************************/

/**
 * @brief 创建链路基类运行资源。
 */
int linkg_link_runtime_create(linkg_link_t *link, const linkg_link_config_t *config)
{
    linkg_thread_config_t rx_thread_config;
    linkg_link_runtime_t *runtime;
    int                   ret;

    if (link == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (link->runtime != NULL)
    {
        return -EALREADY;
    }

    if (config->tx_batch_size == 0U || config->rx_batch_size == 0U)
    {
        return -EINVAL;
    }

    runtime = calloc(1, sizeof(*runtime));
    if (runtime == NULL)
    {
        return -ENOMEM;
    }

    runtime->tx_batch_size     = config->tx_batch_size;
    runtime->rx_batch_size     = config->rx_batch_size;
    runtime->receive           = config->receive;
    runtime->receive_user_data = config->receive_user_data;
    runtime->opened            = false;

    ret = pthread_mutex_init(&runtime->control_lock, NULL);
    if (ret != 0)
    {
        ret = -ret;
        goto fail_runtime;
    }

    ret = pthread_rwlock_init(&runtime->io_lock, NULL);
    if (ret != 0)
    {
        ret = -ret;
        goto fail_control_lock;
    }

    runtime->rx_items = calloc(runtime->rx_batch_size, sizeof(*runtime->rx_items));
    if (runtime->rx_items == NULL)
    {
        ret = -ENOMEM;
        goto fail_io_lock;
    }

    memset(&rx_thread_config, 0, sizeof(rx_thread_config));

    rx_thread_config.cpu_core         = LINKG_LINK_RX_THREAD_CPU_CORE;
    rx_thread_config.affinity_enabled = true;

    ret = linkg_thread_init_with_config(&runtime->rx_thread,
                                        LINKG_LINK_RX_THREAD_NAME,
                                        linkg_link_rx_thread,
                                        link,
                                        &rx_thread_config);
    if (ret != 0)
    {
        goto fail_rx_items;
    }

    link->runtime = runtime;

    return 0;

fail_rx_items:
    free(runtime->rx_items);

fail_io_lock:
    pthread_rwlock_destroy(&runtime->io_lock);

fail_control_lock:
    pthread_mutex_destroy(&runtime->control_lock);

fail_runtime:
    free(runtime);

    return ret;
}

/**
 * @brief 销毁链路基类运行资源。
 *
 * @note 调用前链路必须已经停止，接收线程和同步发送调用均不得继续访问runtime。
 */
void linkg_link_runtime_destroy(linkg_link_t *link)
{
    linkg_link_runtime_t *runtime;
    int                   ret;

    if (link == NULL || link->runtime == NULL)
    {
        return;
    }

    runtime = link->runtime;

    linkg_thread_deinit(&runtime->rx_thread);

    ret = pthread_rwlock_destroy(&runtime->io_lock);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("destroy link IO lock failed, link=%s, error=%d", link->name, ret);
    }

    ret = pthread_mutex_destroy(&runtime->control_lock);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("destroy link control lock failed, link=%s, error=%d", link->name, ret);
    }

    free(runtime->rx_items);
    free(runtime);

    link->runtime = NULL;
}
