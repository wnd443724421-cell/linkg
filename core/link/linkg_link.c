/**
 * @file linkg_link.c
 * @brief LinkG链路基类实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-28
 */

#include "linkg_link.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "link_internal.h"
#include "linkg_log.h"

/****************************** 生命周期 ******************************/

/**
 * @brief 创建完整具体链路对象。
 */
int linkg_link_create(const linkg_link_config_t *config, const linkg_link_ops_t *ops, const void *private_config, linkg_link_t **out)
{
    linkg_link_t *link;
    int           ret;

    if (config == NULL || ops == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    if (config->name == NULL || config->packet_pool == NULL)
    {
        return -EINVAL;
    }

    if (ops->instance_size < sizeof(linkg_link_t))
    {
        return -EINVAL;
    }

    if (ops->init == NULL || ops->deinit == NULL || ops->open == NULL || ops->close == NULL)
    {
        return -EINVAL;
    }

    if (ops->get_rx_fd == NULL || ops->send_batch == NULL || ops->receive_batch == NULL)
    {
        return -EINVAL;
    }

    link = calloc(1, ops->instance_size);
    if (link == NULL)
    {
        return -ENOMEM;
    }

    link->id          = LINKG_LINK_ID_INVALID;
    link->access      = config->access;
    link->ops         = ops;
    link->packet_pool = config->packet_pool;

    snprintf(link->name, sizeof(link->name), "%s", config->name);
    atomic_init(&link->state, LINKG_LINK_STATE_STOPPED);

    ret = linkg_link_runtime_create(link, config);
    if (ret != 0)
    {
        free(link);
        return ret;
    }

    ret = ops->init(link, private_config);
    if (ret != 0)
    {
        linkg_link_runtime_destroy(link);
        free(link);
        return ret;
    }

    *out = link;

    return 0;
}

/**
 * @brief 启动链路。
 */
int linkg_link_start(linkg_link_t *link)
{
    linkg_link_runtime_t *runtime;
    linkg_link_state_t    state;
    int                   cleanup_ret;
    int                   ret;

    if (link == NULL || link->runtime == NULL || link->ops == NULL)
    {
        return -EINVAL;
    }

    if (link->ops->open == NULL || link->ops->close == NULL)
    {
        return -EINVAL;
    }

    runtime = link->runtime;

    pthread_mutex_lock(&runtime->control_lock);

    state = atomic_load(&link->state);

    if (state == LINKG_LINK_STATE_RUNNING)
    {
        pthread_mutex_unlock(&runtime->control_lock);
        return -EALREADY;
    }

    if (state != LINKG_LINK_STATE_STOPPED)
    {
        pthread_mutex_unlock(&runtime->control_lock);
        return -EBUSY;
    }

    atomic_store(&link->state, LINKG_LINK_STATE_STARTING);

    pthread_mutex_unlock(&runtime->control_lock);

    ret = link->ops->open(link);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("open link failed, link=%s, error=%d", link->name, ret);
        goto fail_stopped;
    }

    runtime->opened = true;

    ret = linkg_thread_start(&runtime->rx_thread);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("start link RX thread failed, link=%s, error=%d", link->name, ret);
        goto fail_close;
    }

    pthread_mutex_lock(&runtime->control_lock);

    atomic_store(&link->state, LINKG_LINK_STATE_RUNNING);

    pthread_mutex_unlock(&runtime->control_lock);

    LINKG_LOG_INFO("link started, link=%s", link->name);

    return 0;

fail_close:
    cleanup_ret = link->ops->close(link);
    if (cleanup_ret != 0)
    {
        LINKG_LOG_ERROR("rollback link close failed, link=%s, error=%d", link->name, cleanup_ret);
        goto fail_failed;
    }

    runtime->opened = false;

fail_stopped:
    pthread_mutex_lock(&runtime->control_lock);

    atomic_store(&link->state, LINKG_LINK_STATE_STOPPED);

    pthread_mutex_unlock(&runtime->control_lock);

    return ret;

fail_failed:
    pthread_mutex_lock(&runtime->control_lock);

    atomic_store(&link->state, LINKG_LINK_STATE_FAILED);

    pthread_mutex_unlock(&runtime->control_lock);

    return ret;
}

/**
 * @brief 停止链路并回收运行期资源。
 *
 * @note 切换到STOPPING后会阻止新的同步发送进入；io_lock写锁负责等待已经进入的发送全部退出。
 *       调用方必须保证stop完成后，不会在仍存在旧submit调用时并发重新start或销毁链路对象。
 */
int linkg_link_stop(linkg_link_t *link)
{
    linkg_link_runtime_t *runtime;
    linkg_link_state_t    state;
    bool                  cleanup_complete;
    int                   first_error;
    int                   ret;

    if (link == NULL || link->runtime == NULL || link->ops == NULL)
    {
        return -EINVAL;
    }

    if (link->ops->close == NULL)
    {
        return -EINVAL;
    }

    runtime = link->runtime;

    pthread_mutex_lock(&runtime->control_lock);

    state = atomic_load(&link->state);

    if (state == LINKG_LINK_STATE_STOPPED)
    {
        pthread_mutex_unlock(&runtime->control_lock);
        return 0;
    }

    if (state == LINKG_LINK_STATE_STARTING || state == LINKG_LINK_STATE_STOPPING)
    {
        pthread_mutex_unlock(&runtime->control_lock);
        return -EBUSY;
    }

    if (state != LINKG_LINK_STATE_RUNNING && state != LINKG_LINK_STATE_FAILED)
    {
        pthread_mutex_unlock(&runtime->control_lock);
        return -EINVAL;
    }

    // 先禁止新的同步发送进入链路。
    atomic_store(&link->state, LINKG_LINK_STATE_STOPPING);

    pthread_mutex_unlock(&runtime->control_lock);

    first_error      = 0;
    cleanup_complete = false;

    // 先停止接收线程，确保底层接收资源不再被访问。
    if (linkg_thread_is_started(&runtime->rx_thread))
    {
        ret = linkg_thread_stop(&runtime->rx_thread);
        if (ret != 0)
        {
            LINKG_LOG_ERROR("stop link RX thread failed, link=%s, error=%d", link->name, ret);
            first_error = ret;
        }
    }

    if (!linkg_thread_is_started(&runtime->rx_thread))
    {
        /**
         * STOPPING已经阻止新的submit进入。
         *
         * io_lock写锁等待所有已经进入的submit释放读锁，
         * 获得写锁后即可保证不存在任何同步发送正在使用具体链路资源。
         */
        pthread_rwlock_wrlock(&runtime->io_lock);

        if (runtime->opened)
        {
            ret = link->ops->close(link);
            if (ret != 0)
            {
                LINKG_LOG_ERROR("close link failed, link=%s, error=%d", link->name, ret);

                if (first_error == 0)
                {
                    first_error = ret;
                }
            }
            else
            {
                runtime->opened = false;
                cleanup_complete = true;
            }
        }
        else
        {
            cleanup_complete = true;
        }

        pthread_rwlock_unlock(&runtime->io_lock);
    }
    else if (first_error == 0)
    {
        first_error = -EBUSY;
    }

    pthread_mutex_lock(&runtime->control_lock);

    if (cleanup_complete)
    {
        atomic_store(&link->state, LINKG_LINK_STATE_STOPPED);
    }
    else
    {
        atomic_store(&link->state, LINKG_LINK_STATE_FAILED);
    }

    pthread_mutex_unlock(&runtime->control_lock);

    if (first_error != 0)
    {
        return first_error;
    }

    LINKG_LOG_INFO("link stopped, link=%s", link->name);

    return 0;
}

/**
 * @brief 销毁完整具体链路对象。
 *
 * @note 调用前链路必须已经处于STOPPED状态。
 */
void linkg_link_destroy(linkg_link_t *link)
{
    linkg_link_state_t state;

    if (link == NULL)
    {
        return;
    }

    state = atomic_load(&link->state);
    if (state != LINKG_LINK_STATE_STOPPED)
    {
        LINKG_LOG_ERROR("destroy running link rejected, link=%s, state=%d", link->name, (int)state);
        return;
    }

    if (link->ops != NULL && link->ops->deinit != NULL)
    {
        link->ops->deinit(link);
    }

    linkg_link_runtime_destroy(link);

    free(link);
}

/****************************** 属性查询 ******************************/

/**
 * @brief 获取链路运行实例标识。
 */
uint32_t linkg_link_get_id(const linkg_link_t *link)
{
    if (link == NULL)
    {
        return LINKG_LINK_ID_INVALID;
    }

    return link->id;
}

/**
 * @brief 获取链路名称。
 */
const char *linkg_link_get_name(const linkg_link_t *link)
{
    if (link == NULL)
    {
        return NULL;
    }

    return link->name;
}

/**
 * @brief 获取链路接入类型。
 */
linkg_link_access_t linkg_link_get_access(const linkg_link_t *link)
{
    if (link == NULL)
    {
        return LINKG_LINK_ACCESS_NONE;
    }

    return link->access;
}

/**
 * @brief 获取链路当前运行状态。
 */
linkg_link_state_t linkg_link_get_state(const linkg_link_t *link)
{
    if (link == NULL)
    {
        return LINKG_LINK_STATE_STOPPED;
    }

    return atomic_load(&link->state);
}

/**
 * @brief 判断链路是否正在运行。
 */
bool linkg_link_is_running(const linkg_link_t *link)
{
    return linkg_link_get_state(link) == LINKG_LINK_STATE_RUNNING;
}
