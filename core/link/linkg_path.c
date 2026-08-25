/**
 * @file linkg_path.c
 * @brief LinkG对端链路路径实现
 * @author Dawn
 * @version 1.2.0
 * @date 2026-08-25
 */

#include "linkg_path.h"

#include <errno.h>
#include <string.h>

/****************************** 模块常量 ******************************/

#define LINKG_PATH_LINK_ID_INVALID 0U // 无效链路ID

/****************************** 内部辅助 ******************************/

/**
 * @brief 完成路径释放。
 *
 * @note notify为true时通知路径所有者；false时由当前调用方自行处理释放结果。
 */
static bool _linkg_path_complete_release(linkg_path_t *path, bool notify)
{
    linkg_path_released_func_t released;
    linkg_path_state_t expected;
    void *user_data;

    if (path == NULL || !path->initialized)
    {
        return false;
    }

    expected = LINKG_PATH_STATE_RETIRED;

    if (!atomic_compare_exchange_strong(&path->state, &expected, LINKG_PATH_STATE_RELEASED))
    {
        return false;
    }

    if (!notify)
    {
        return true;
    }

    released = path->released;
    user_data = path->released_user_data;

    if (released != NULL)
    {
        released(path, user_data);
    }

    return true;
}

/**
 * @brief 释放一个路径异步引用。
 */
static void _linkg_path_release_reference(linkg_path_t *path)
{
    uint32_t references;

    if (path == NULL || !path->initialized)
    {
        return;
    }

    references = atomic_load(&path->reference_count);

    while (references > 0U)
    {
        if (!atomic_compare_exchange_weak(&path->reference_count, &references, references - 1U))
        {
            continue;
        }

        if (references == 1U)
        {
            (void)_linkg_path_complete_release(path, true);
        }

        return;
    }
}

/**
 * @brief 在路径统计锁保护下清空累计统计。
 */
static int _linkg_path_reset_stats(linkg_path_t *path)
{
    int ret;

    if (path == NULL || !path->initialized)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&path->stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    linkg_path_stats_reset(&path->stats);

    ret = pthread_mutex_unlock(&path->stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化路径对象。
 *
 * @note Path对象首次初始化前必须清零，成功后由Node统一管理其生命周期。
 */
int linkg_path_init(linkg_path_t *path)
{
    int ret;

    if (path == NULL)
    {
        return -EINVAL;
    }

    if (path->initialized)
    {
        return -EALREADY;
    }

    memset(path, 0, sizeof(*path));

    ret = pthread_mutex_init(&path->stats_lock, NULL);
    if (ret != 0)
    {
        memset(path, 0, sizeof(*path));
        return -ret;
    }

    atomic_init(&path->reference_count, 0U);
    atomic_init(&path->state, LINKG_PATH_STATE_EMPTY);

    path->initialized = true;

    return 0;
}

/**
 * @brief 反初始化路径对象。
 *
 * @note 只有EMPTY且不存在异步引用的Path才能反初始化，调用后对象恢复为全零状态。
 */
int linkg_path_deinit(linkg_path_t *path)
{
    int ret;

    if (path == NULL)
    {
        return -EINVAL;
    }

    if (!path->initialized)
    {
        return 0;
    }

    if (atomic_load(&path->state) != LINKG_PATH_STATE_EMPTY || atomic_load(&path->reference_count) != 0U)
    {
        return -EBUSY;
    }

    ret = pthread_mutex_destroy(&path->stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    memset(path, 0, sizeof(*path));

    return 0;
}

/**
 * @brief 激活路径并配置承载链路和下一跳端点。
 *
 * @note 调用方必须由Node保证Path生命周期和endpoint更新已经串行化。
 */
int linkg_path_activate(linkg_path_t *path, uint32_t link_id, const linkg_path_endpoint_t *next_hop, linkg_path_released_func_t released, void *user_data)
{
    int ret;

    if (path == NULL || !path->initialized || next_hop == NULL || link_id == LINKG_PATH_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    if (atomic_load(&path->state) != LINKG_PATH_STATE_EMPTY || atomic_load(&path->reference_count) != 0U)
    {
        return -EBUSY;
    }

    ret = _linkg_path_reset_stats(path);
    if (ret != 0)
    {
        return ret;
    }

    path->link_id = link_id;
    path->next_hop = *next_hop;
    path->released = released;
    path->released_user_data = user_data;

    atomic_store(&path->state, LINKG_PATH_STATE_ACTIVE);

    return 0;
}

/**
 * @brief 批量获取路径异步发送引用。
 *
 * @note 调用方必须由Node保证Path生命周期操作已经串行化。
 */
int linkg_path_acquire_batch(linkg_path_t *path, uint32_t count)
{
    if (path == NULL || !path->initialized || count == 0U)
    {
        return -EINVAL;
    }

    if (atomic_load(&path->state) != LINKG_PATH_STATE_ACTIVE)
    {
        return -ENODEV;
    }

    atomic_fetch_add(&path->reference_count, count);

    return 0;
}

/**
 * @brief 获取一个路径异步发送引用。
 */
int linkg_path_acquire(linkg_path_t *path)
{
    return linkg_path_acquire_batch(path, 1U);
}

/**
 * @brief 退役路径并停止接收新的异步引用。
 *
 * @note released返回当前调用方是否同步完成RETIRED到RELEASED转换；
 *       false表示仍存在异步引用，或者最后一个release线程已经接管异步释放通知。
 */
int linkg_path_retire(linkg_path_t *path, bool *released)
{
    linkg_path_state_t expected;

    if (path == NULL || !path->initialized || released == NULL)
    {
        return -EINVAL;
    }

    *released = false;
    expected = LINKG_PATH_STATE_ACTIVE;

    if (!atomic_compare_exchange_strong(&path->state, &expected, LINKG_PATH_STATE_RETIRED))
    {
        if (expected == LINKG_PATH_STATE_RETIRED || expected == LINKG_PATH_STATE_RELEASED)
        {
            return 0;
        }

        return -ENODEV;
    }

    if (atomic_load(&path->reference_count) == 0U)
    {
        *released = _linkg_path_complete_release(path, false);
    }

    return 0;
}

/**
 * @brief 释放一个路径异步发送引用。
 *
 * @note 每次调用消费调用方持有的一个Path引用，释放后调用方不得继续使用该引用。
 */
void linkg_path_release(linkg_path_t *path)
{
    _linkg_path_release_reference(path);
}

/**
 * @brief 将已经释放的路径恢复为空闲状态。
 *
 * @note 调用方必须保证Path已进入RELEASED且所有异步引用已经释放。
 */
int linkg_path_reset(linkg_path_t *path)
{
    int ret;

    if (path == NULL || !path->initialized)
    {
        return -EINVAL;
    }

    if (atomic_load(&path->state) == LINKG_PATH_STATE_EMPTY)
    {
        return 0;
    }

    if (atomic_load(&path->state) != LINKG_PATH_STATE_RELEASED || atomic_load(&path->reference_count) != 0U)
    {
        return -EBUSY;
    }

    ret = _linkg_path_reset_stats(path);
    if (ret != 0)
    {
        return ret;
    }

    path->link_id = LINKG_PATH_LINK_ID_INVALID;
    memset(&path->next_hop, 0, sizeof(path->next_hop));

    path->released = NULL;
    path->released_user_data = NULL;

    atomic_store(&path->state, LINKG_PATH_STATE_EMPTY);

    return 0;
}

/**
 * @brief 更新活动路径的下一跳端点。
 *
 * @note 调用方必须由Node保证Path生命周期和endpoint更新已经串行化。
 */
int linkg_path_update_endpoint(linkg_path_t *path, const linkg_path_endpoint_t *next_hop)
{
    if (path == NULL || !path->initialized || next_hop == NULL)
    {
        return -EINVAL;
    }

    if (atomic_load(&path->state) != LINKG_PATH_STATE_ACTIVE)
    {
        return -ENODEV;
    }

    path->next_hop = *next_hop;

    return 0;
}

/****************************** 统计操作 ******************************/

/**
 * @brief 记录路径底层发送成功。
 *
 * @note 调用方必须持有有效Path引用，统计锁由Path内部管理。
 */
void linkg_path_record_tx_success(linkg_path_t *path, uint64_t bytes, uint64_t packets)
{
    if (path == NULL || !path->initialized)
    {
        return;
    }

    pthread_mutex_lock(&path->stats_lock);
    linkg_path_stats_record_tx_success(&path->stats, bytes, packets);
    pthread_mutex_unlock(&path->stats_lock);
}

/**
 * @brief 记录路径底层最终发送失败。
 *
 * @note failed只记录真正进入底层发送流程后最终失败的数据，不包含本地主动丢弃和超时失效。
 */
void linkg_path_record_tx_failed(linkg_path_t *path, uint64_t bytes, uint64_t packets)
{
    if (path == NULL || !path->initialized)
    {
        return;
    }

    pthread_mutex_lock(&path->stats_lock);
    linkg_path_stats_record_tx_failed(&path->stats, bytes, packets);
    pthread_mutex_unlock(&path->stats_lock);
}

/**
 * @brief 记录路径本地主动丢弃。
 *
 * @note dropped与failed、expired互斥，不重复累计到其它失败原因字段。
 */
void linkg_path_record_tx_dropped(linkg_path_t *path, uint64_t bytes, uint64_t packets)
{
    if (path == NULL || !path->initialized)
    {
        return;
    }

    pthread_mutex_lock(&path->stats_lock);
    linkg_path_stats_record_tx_dropped(&path->stats, bytes, packets);
    pthread_mutex_unlock(&path->stats_lock);
}

/**
 * @brief 记录路径本地等待超时失效。
 *
 * @note expired与failed、dropped互斥，不重复累计到其它失败原因字段。
 */
void linkg_path_record_tx_expired(linkg_path_t *path, uint64_t bytes, uint64_t packets)
{
    if (path == NULL || !path->initialized)
    {
        return;
    }

    pthread_mutex_lock(&path->stats_lock);
    linkg_path_stats_record_tx_expired(&path->stats, bytes, packets);
    pthread_mutex_unlock(&path->stats_lock);
}

/**
 * @brief 记录从该路径实际接收成功的数据。
 *
 * @note rx是本机累计事实统计，不表示端到端丢包率。
 */
void linkg_path_record_rx(linkg_path_t *path, uint64_t bytes, uint64_t packets)
{
    if (path == NULL || !path->initialized)
    {
        return;
    }

    pthread_mutex_lock(&path->stats_lock);
    linkg_path_stats_record_rx(&path->stats, bytes, packets);
    pthread_mutex_unlock(&path->stats_lock);
}

/**
 * @brief 获取路径累计统计一致快照。
 */
int linkg_path_get_stats(linkg_path_t *path, linkg_path_stats_t *stats)
{
    int ret;

    if (path == NULL || stats == NULL)
    {
        return -EINVAL;
    }

    if (!path->initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&path->stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    *stats = path->stats;

    ret = pthread_mutex_unlock(&path->stats_lock);
    if (ret != 0)
    {
        return -ret;
    }

    return 0;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取路径当前生命周期状态。
 */
linkg_path_state_t linkg_path_get_state(const linkg_path_t *path)
{
    if (path == NULL || !path->initialized)
    {
        return LINKG_PATH_STATE_EMPTY;
    }

    return atomic_load(&path->state);
}

/**
 * @brief 获取路径当前异步引用数量。
 */
uint32_t linkg_path_get_reference_count(const linkg_path_t *path)
{
    if (path == NULL || !path->initialized)
    {
        return 0U;
    }

    return atomic_load(&path->reference_count);
}

/**
 * @brief 判断路径当前是否处于活动状态。
 */
bool linkg_path_is_active(const linkg_path_t *path)
{
    return linkg_path_get_state(path) == LINKG_PATH_STATE_ACTIVE;
}
