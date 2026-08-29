/**
 * @file linkg_transport.c
 * @brief LinkG逻辑传输层实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-29
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
 * @note 调用方必须持有g_transport.lock，返回指针不得在解锁后继续使用。
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
 * @brief 校验Transport类型。
 */
bool linkg_transport_type_valid(linkg_transport_type_t type)
{
    return type > LINKG_TRANSPORT_TYPE_NONE &&
           type < LINKG_TRANSPORT_TYPE_COUNT;
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

    ret = pthread_mutex_init(&g_transport.lock, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    g_transport.local_node_id  = local->node_id;
    g_transport.local_role     = local->role;
    g_transport.next_packet_id = 1U;
    g_transport.initialized    = true;

    ret = linkg_transport_reassembly_runtime_init();
    if (ret != 0)
    {
        goto fail_lock;
    }

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

fail_lock:
    g_transport.initialized = false;

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

    ret = pthread_mutex_destroy(&g_transport.lock);
    if (ret != 0)
    {
        g_transport.initialized = true;
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
        pthread_mutex_unlock(&g_transport.lock);
        return 0;
    }

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

    memset(peer, 0, sizeof(*peer));

    peer->peer_node_id       = peer_node_id;
    peer->stats.peer_node_id = peer_node_id;
    peer->valid              = true;

    linkg_transport_window_reset(&peer->rx_window);

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

    peer->tx_sequence = 0U;

    linkg_transport_window_reset(&peer->rx_window);

    if (clear_stats)
    {
        memset(&peer->stats, 0, sizeof(peer->stats));
        peer->stats.peer_node_id = peer->peer_node_id;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 注销直接Peer并释放Transport状态。
 */
int linkg_transport_unregister_peer(uint8_t peer_node_id)
{
    linkg_transport_peer_t *peer;
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

    memset(peer, 0, sizeof(*peer));

    if (g_transport.peer_count > 0U)
    {
        g_transport.peer_count--;
    }

    ret = pthread_mutex_unlock(&g_transport.lock);

    return ret == 0 ? 0 : -ret;
}

/****************************** 类型交付 ******************************/

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

/****************************** 统计查询 ******************************/

/**
 * @brief 获取指定直接Peer的Transport累计统计。
 */
int linkg_transport_get_peer_stats(uint8_t peer_node_id, linkg_transport_peer_stats_t *stats)
{
    linkg_transport_peer_t *peer;
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

    *stats = peer->stats;

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
