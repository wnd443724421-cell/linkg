/**
 * @file linkg_transport.c
 * @brief LinkG逻辑传输层实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-09-11
 */

#include "transport_internal.h"

#include <errno.h>
#include <string.h>

#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_system_resources.h"

/****************************** 模块状态 ******************************/

linkg_transport_context_t g_transport;

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验节点编号。
 */
static bool _linkg_transport_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN &&
           node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 初始化全部固定Peer槽位的Class发送顺序锁。
 */
static int _linkg_transport_peer_tx_locks_init(void)
{
    uint32_t initialized_count;
    uint32_t peer_index;
    uint32_t class_index;
    int      ret;

    initialized_count = 0U;

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            ret = pthread_mutex_init(&g_transport.peers[peer_index].classes[class_index].tx_order_lock, NULL);
            if (ret != 0)
            {
                goto fail;
            }

            initialized_count++;
        }
    }

    return 0;

fail:
    while (initialized_count > 0U)
    {
        initialized_count--;

        peer_index  = initialized_count / LINKG_TRANSPORT_CLASS_COUNT;
        class_index = initialized_count % LINKG_TRANSPORT_CLASS_COUNT;

        (void)pthread_mutex_destroy(&g_transport.peers[peer_index].classes[class_index].tx_order_lock);
    }

    return -ret;
}

/**
 * @brief 销毁全部固定Peer槽位的Class发送顺序锁。
 */
static void _linkg_transport_peer_tx_locks_deinit(void)
{
    uint32_t peer_index;
    uint32_t class_index;

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            (void)pthread_mutex_destroy(&g_transport.peers[peer_index].classes[class_index].tx_order_lock);
        }
    }
}

/**
 * @brief 初始化全部固定Peer槽位的生命周期代际。
 */
static void _linkg_transport_peer_epochs_init(void)
{
    uint32_t peer_index;

    for (peer_index = 0U; peer_index < LINKG_TRANSPORT_PEER_MAX; peer_index++)
    {
        atomic_init(&g_transport.peers[peer_index].lifecycle_epoch, 0U);
    }
}

/**
 * @brief 锁定指定Peer全部业务类别的发送顺序锁。
 *
 * @note 调用方必须已经持有g_transport.lock。
 *       三个Class始终按照固定索引顺序加锁。
 */
static int _linkg_transport_peer_tx_order_lock_all(linkg_transport_peer_t *peer)
{
    uint32_t class_index;
    int      ret;

    if (peer == NULL)
    {
        return -EINVAL;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        ret = pthread_mutex_lock(&peer->classes[class_index].tx_order_lock);
        if (ret != 0)
        {
            while (class_index > 0U)
            {
                class_index--;
                pthread_mutex_unlock(&peer->classes[class_index].tx_order_lock);
            }

            return -ret;
        }
    }

    return 0;
}

/**
 * @brief 解锁指定Peer全部业务类别的发送顺序锁。
 *
 * @note 三个Class按照与加锁相反的顺序释放。
 */
static void _linkg_transport_peer_tx_order_unlock_all(linkg_transport_peer_t *peer)
{
    uint32_t class_index;

    if (peer == NULL)
    {
        return;
    }

    class_index = LINKG_TRANSPORT_CLASS_COUNT;

    while (class_index > 0U)
    {
        class_index--;
        pthread_mutex_unlock(&peer->classes[class_index].tx_order_lock);
    }
}

/**
 * @brief 重置指定Peer的三业务类别协议状态。
 *
 * @note 调用方必须持有g_transport.lock，并确保当前Peer不存在并发TX。
 *       本函数不会修改tx_order_lock、peer_node_id和valid。
 */
static void _linkg_transport_reset_peer_classes(linkg_transport_peer_t *peer, bool clear_stats)
{
    linkg_transport_peer_class_t *peer_class;
    uint32_t                      class_index;

    if (peer == NULL)
    {
        return;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        peer_class = &peer->classes[class_index];

        peer_class->tx_sequence = 0U;

        linkg_transport_window_reset(&peer_class->rx_window);

        if (clear_stats)
        {
            memset(&peer_class->stats, 0, sizeof(peer_class->stats));
        }
    }
}

/**
 * @brief 计算固定Peer槽位的下一个可用偶数代际。
 */
static uint32_t _linkg_transport_peer_next_active_epoch(uint32_t current_epoch)
{
    uint32_t next_epoch;

    next_epoch = current_epoch + 1U;

    if ((next_epoch & 1U) != 0U)
    {
        next_epoch++;
    }

    return next_epoch;
}

/**
 * @brief 将Peer切换到清理中的奇数代际并等待当前同步发送完成。
 *
 * @note 调用方必须持有g_transport.lock。
 */
static int _linkg_transport_peer_quiesce_locked(linkg_transport_peer_t *peer, bool clear_stats, uint32_t *quiescing_epoch)
{
    uint32_t active_epoch;
    int      ret;

    if (peer == NULL || quiescing_epoch == NULL)
    {
        return -EINVAL;
    }

    if (!linkg_transport_peer_epoch_read_active(peer, &active_epoch))
    {
        return -EBUSY;
    }

    *quiescing_epoch = active_epoch + 1U;

    atomic_store_explicit(&peer->lifecycle_epoch, *quiescing_epoch, memory_order_release);

    ret = _linkg_transport_peer_tx_order_lock_all(peer);
    if (ret != 0)
    {
        atomic_store_explicit(&peer->lifecycle_epoch, active_epoch, memory_order_release);
        return ret;
    }

    _linkg_transport_reset_peer_classes(peer, clear_stats);

    _linkg_transport_peer_tx_order_unlock_all(peer);

    return 0;
}

/**
 * @brief 在Peer已经停止接收新分片后同步清理两类分片缓存。
 *
 * @note 调用期间不得持有g_transport.lock。
 */
static int _linkg_transport_reset_peer_caches(uint8_t peer_node_id)
{
    int first_error;
    int ret;

    first_error = 0;

    ret = linkg_transport_reassembly_reset_peer(peer_node_id);
    if (ret != 0)
    {
        first_error = ret;
    }

    ret = linkg_transport_forward_pair_reset_peer(peer_node_id);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/**
 * @brief 查找空闲Peer槽位。
 *
 * @note 调用方必须持有g_transport.lock。
 */
static linkg_transport_peer_t *_linkg_transport_find_unused_locked(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_PEER_MAX; index++)
    {
        if (!g_transport.peers[index].valid)
        {
            return &g_transport.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 查找指定直接Peer。
 *
 * @note 调用方必须持有g_transport.lock。
 *       返回指针的普通成员不得在解锁后无保护访问，
 *       lifecycle_epoch只能通过原子辅助函数读取。
 */
linkg_transport_peer_t *linkg_transport_find_peer_locked(uint8_t peer_node_id)
{
    uint32_t index;

    if (!_linkg_transport_node_id_valid(peer_node_id))
    {
        return NULL;
    }

    for (index = 0U; index < LINKG_TRANSPORT_PEER_MAX; index++)
    {
        if (g_transport.peers[index].valid &&
            g_transport.peers[index].peer_node_id == peer_node_id)
        {
            return &g_transport.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 读取Peer当前生命周期代际并判断是否允许数据面进入。
 *
 * @note Peer槽位地址在Transport生命周期内保持稳定。
 */
bool linkg_transport_peer_epoch_read_active(const linkg_transport_peer_t *peer, uint32_t *peer_epoch)
{
    uint32_t current_epoch;

    if (peer == NULL)
    {
        return false;
    }

    current_epoch = atomic_load_explicit(&peer->lifecycle_epoch, memory_order_acquire);

    if (peer_epoch != NULL)
    {
        *peer_epoch = current_epoch;
    }

    return (current_epoch & 1U) == 0U;
}

/**
 * @brief 判断RX捕获的Peer代际是否仍为当前可用代际。
 */
bool linkg_transport_peer_epoch_matches(const linkg_transport_peer_t *peer, uint32_t peer_epoch)
{
    uint32_t current_epoch;

    if (peer == NULL || (peer_epoch & 1U) != 0U)
    {
        return false;
    }

    current_epoch = atomic_load_explicit(&peer->lifecycle_epoch, memory_order_acquire);

    return current_epoch == peer_epoch;
}

/**
 * @brief 校验Transport类型。
 */
bool linkg_transport_type_valid(linkg_transport_type_t type)
{
    return type > LINKG_TRANSPORT_TYPE_NONE &&
           type < LINKG_TRANSPORT_TYPE_COUNT;
}

/**
 * @brief 校验Transport业务类别。
 */
bool linkg_transport_class_valid(linkg_transport_class_t traffic_class)
{
    return traffic_class >= LINKG_TRANSPORT_CLASS_REALTIME &&
           traffic_class < LINKG_TRANSPORT_CLASS_COUNT;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Transport模块。
 */
int linkg_transport_init(void)
{
    const linkg_node_info_t *local;
    int                      ret;

    if (g_transport.initialized)
    {
        return -EALREADY;
    }

    local = linkg_node_get_local();
    if (local == NULL)
    {
        return -ENODEV;
    }

    if (!_linkg_transport_node_id_valid(local->node_id))
    {
        return -EINVAL;
    }

    if (local->role != LINKG_DEVICE_ROLE_AP &&
        local->role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    memset(&g_transport, 0, sizeof(g_transport));
    _linkg_transport_peer_epochs_init();

    ret = pthread_mutex_init(&g_transport.lock, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    ret = _linkg_transport_peer_tx_locks_init();
    if (ret != 0)
    {
        goto fail_lock;
    }

    g_transport.local_node_id  = local->node_id;
    g_transport.local_role     = local->role;
    g_transport.next_packet_id = 1U;
    g_transport.initialized    = true;

    ret = linkg_transport_reassembly_runtime_init();
    if (ret != 0)
    {
        goto fail_peer_locks;
    }

    /**
     * STA不具备中继能力，不需要初始化中继分片配对资源。
     * AP收到非本机Transport Frame时才可能进入中继Fast Path。
     */
    if (g_transport.local_role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_transport_forward_pair_runtime_init();
        if (ret != 0)
        {
            goto fail_reassembly;
        }
    }

    ret = linkg_link_manager_register_receive_handler(linkg_transport_receive_batch, NULL);
    if (ret != 0)
    {
        goto fail_forward_pair;
    }

    return 0;

fail_forward_pair:
    if (g_transport.local_role == LINKG_DEVICE_ROLE_AP)
    {
        (void)linkg_transport_forward_pair_runtime_deinit();
    }

fail_reassembly:
    (void)linkg_transport_reassembly_runtime_deinit();

fail_peer_locks:
    g_transport.initialized = false;

    _linkg_transport_peer_tx_locks_deinit();

fail_lock:
    pthread_mutex_destroy(&g_transport.lock);
    memset(&g_transport, 0, sizeof(g_transport));

    return ret;
}

/**
 * @brief 反初始化Transport模块。
 */
int linkg_transport_deinit(void)
{
    int ret;

    if (!g_transport.initialized)
    {
        return 0;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_transport.peer_count != 0U)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -EBUSY;
    }

    pthread_mutex_unlock(&g_transport.lock);

    ret = linkg_link_manager_unregister_receive_handler();
    if (ret != 0)
    {
        return ret;
    }

    if (g_transport.local_role == LINKG_DEVICE_ROLE_AP)
    {
        ret = linkg_transport_forward_pair_runtime_deinit();
        if (ret != 0)
        {
            return ret;
        }
    }

    ret = linkg_transport_reassembly_runtime_deinit();
    if (ret != 0)
    {
        return ret;
    }

    g_transport.initialized = false;

    _linkg_transport_peer_tx_locks_deinit();

    ret = pthread_mutex_destroy(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    memset(&g_transport, 0, sizeof(g_transport));

    return 0;
}

/****************************** Peer状态 ******************************/

/**
 * @brief 注册直接Peer的Transport状态。
 */
int linkg_transport_register_peer(uint8_t peer_node_id)
{
    linkg_transport_peer_t *peer;
    uint32_t                current_epoch;
    int                     ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_transport_node_id_valid(peer_node_id) ||
        peer_node_id == g_transport.local_node_id)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    peer = linkg_transport_find_peer_locked(peer_node_id);
    if (peer != NULL)
    {
        if (!linkg_transport_peer_epoch_read_active(peer, NULL))
        {
            pthread_mutex_unlock(&g_transport.lock);
            return -EBUSY;
        }

        pthread_mutex_unlock(&g_transport.lock);
        return 0;
    }

    /**
     * STA在当前主从拓扑中只允许存在一个直接Peer。
     * WiFi和5G只是同一Peer的不同Path，不会增加Peer数量。
     */
    if (g_transport.local_role == LINKG_DEVICE_ROLE_STA &&
        g_transport.peer_count != 0U)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOSPC;
    }

    peer = _linkg_transport_find_unused_locked();
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOSPC;
    }

    /**
     * 未使用槽位不存在并发TX，可以直接复位协议状态。
     * tx_order_lock已经在Transport初始化阶段完成初始化。
     */
    _linkg_transport_reset_peer_classes(peer, true);

    current_epoch      = atomic_load_explicit(&peer->lifecycle_epoch, memory_order_relaxed);
    peer->peer_node_id = peer_node_id;
    peer->valid        = true;

    atomic_store_explicit(&peer->lifecycle_epoch,
                          _linkg_transport_peer_next_active_epoch(current_epoch),
                          memory_order_release);

    g_transport.peer_count++;

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 重置直接Peer的Transport状态。
 */
int linkg_transport_reset_peer(uint8_t peer_node_id, bool clear_stats)
{
    linkg_transport_peer_t *peer;
    uint32_t                current_epoch;
    uint32_t                quiescing_epoch;
    int                     cache_ret;
    int                     ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_transport_node_id_valid(peer_node_id))
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    peer = linkg_transport_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    ret = _linkg_transport_peer_quiesce_locked(peer, clear_stats, &quiescing_epoch);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return ret;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    cache_ret = _linkg_transport_reset_peer_caches(peer_node_id);

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    current_epoch = atomic_load_explicit(&peer->lifecycle_epoch, memory_order_acquire);

    if (!peer->valid ||
        peer->peer_node_id != peer_node_id ||
        current_epoch != quiescing_epoch)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ESTALE;
    }

    atomic_store_explicit(&peer->lifecycle_epoch, quiescing_epoch + 1U, memory_order_release);

    ret = pthread_mutex_unlock(&g_transport.lock);
    if (cache_ret != 0)
    {
        return cache_ret;
    }

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 注销直接Peer并释放Transport协议状态。
 */
int linkg_transport_unregister_peer(uint8_t peer_node_id)
{
    linkg_transport_peer_t *peer;
    uint32_t                current_epoch;
    uint32_t                quiescing_epoch;
    int                     cache_ret;
    int                     ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_transport_node_id_valid(peer_node_id))
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    peer = linkg_transport_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    ret = _linkg_transport_peer_quiesce_locked(peer, true, &quiescing_epoch);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return ret;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    cache_ret = _linkg_transport_reset_peer_caches(peer_node_id);

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    current_epoch = atomic_load_explicit(&peer->lifecycle_epoch, memory_order_acquire);

    if (!peer->valid ||
        peer->peer_node_id != peer_node_id ||
        current_epoch != quiescing_epoch)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ESTALE;
    }

    peer->peer_node_id = 0U;
    peer->valid        = false;

    if (g_transport.peer_count > 0U)
    {
        g_transport.peer_count--;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);
    if (cache_ret != 0)
    {
        return cache_ret;
    }

    return ret == 0 ? 0 : -ret;
}

/****************************** 本机交付 ******************************/

/**
 * @brief 注册指定Transport类型的本机交付函数。
 */
int linkg_transport_register_handler(linkg_transport_type_t type, linkg_transport_handler_func_t handler, void *user_data)
{
    int ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!linkg_transport_type_valid(type) || handler == NULL)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_transport.handlers[type].handler != NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -EALREADY;
    }

    g_transport.handlers[type].handler   = handler;
    g_transport.handlers[type].user_data = user_data;

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 注销指定Transport类型的本机交付函数。
 */
int linkg_transport_unregister_handler(linkg_transport_type_t type)
{
    int ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!linkg_transport_type_valid(type))
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_transport.handlers[type].handler == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    memset(&g_transport.handlers[type], 0, sizeof(g_transport.handlers[type]));

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 中继调度 ******************************/

/**
 * @brief 注册Transport中继调度处理函数。
 */
int linkg_transport_register_forward_handler(linkg_transport_forward_handler_func_t handler, void *user_data)
{
    int ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (handler == NULL)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_transport.forward_handler.handler != NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -EALREADY;
    }

    g_transport.forward_handler.handler   = handler;
    g_transport.forward_handler.user_data = user_data;

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 注销Transport中继调度处理函数。
 */
int linkg_transport_unregister_forward_handler(void)
{
    int ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_transport.forward_handler.handler == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    memset(&g_transport.forward_handler, 0, sizeof(g_transport.forward_handler));

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 统计查询 ******************************/

/**
 * @brief 获取指定直接Peer的Transport累计统计。
 *
 * @note g_transport.lock保护RX统计和Peer生命周期，
 *       三个tx_order_lock保护对应Class的TX统计。
 */
int linkg_transport_get_peer_stats(uint8_t peer_node_id, linkg_transport_peer_stats_t *stats)
{
    linkg_transport_peer_t *peer;
    uint32_t                class_index;
    int                     ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_transport_node_id_valid(peer_node_id) || stats == NULL)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    peer = linkg_transport_find_peer_locked(peer_node_id);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return -ENOENT;
    }

    ret = _linkg_transport_peer_tx_order_lock_all(peer);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_transport.lock);
        return ret;
    }

    memset(stats, 0, sizeof(*stats));

    stats->peer_node_id = peer->peer_node_id;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        stats->classes[class_index] = peer->classes[class_index].stats;
    }

    _linkg_transport_peer_tx_order_unlock_all(peer);

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 获取Transport全局异常统计。
 */
int linkg_transport_get_global_stats(linkg_transport_global_stats_t *stats)
{
    int ret;

    if (!g_transport.initialized)
    {
        return -ENODEV;
    }

    if (stats == NULL)
    {
        return -EINVAL;
    }

    ret = pthread_mutex_lock(&g_transport.lock);
    if (ret != 0)
    {
        return -ret;
    }

    *stats = g_transport.stats;

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}
