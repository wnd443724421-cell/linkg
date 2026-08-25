/**
 * @file linkg_node.c
 * @brief LinkG节点管理实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-25
 */

#include "linkg_node.h"

#include <errno.h>
#include <pthread.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_network_ops.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_node_info_t info;                       // 对端节点信息
    linkg_path_t      paths[LINKG_NODE_PATH_MAX]; // 对端路径槽位
    uint32_t          path_count;                 // 当前活动路径数量
    bool              valid;                      // 对端是否在线可用
    bool              retiring;                   // 对端是否正在等待路径退役完成
} linkg_node_peer_slot_t;

typedef struct
{
    pthread_mutex_t        lock;                         // 节点和Path生命周期保护锁
    linkg_node_info_t      local;                        // 本机节点信息
    linkg_node_peer_slot_t peers[LINKG_NODE_PEER_MAX];  // 直接对端固定槽位
    uint32_t               peer_count;                   // 当前在线直接对端数量
    bool                   initialized;                  // 节点模块是否已经初始化
} linkg_node_context_t;

/****************************** 全局上下文 ******************************/

static linkg_node_context_t g_node;

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取Node状态锁。
 */
static int _linkg_node_lock(void)
{
    int ret;

    ret = pthread_mutex_lock(&g_node.lock);

    return ret == 0 ? 0 : -ret;
}

/**
 * @brief 释放Node状态锁并保留原操作结果。
 */
static int _linkg_node_unlock(int result)
{
    int ret;

    ret = pthread_mutex_unlock(&g_node.lock);
    if (ret != 0)
    {
        return -ret;
    }

    return result;
}

/**
 * @brief 校验节点基础信息。
 */
static bool _linkg_node_info_valid(const linkg_node_info_t *info)
{
    if (info == NULL)
    {
        return false;
    }

    if (!linkg_network_ipv4_address_valid(&info->tun_address))
    {
        return false;
    }

    if (info->role != LINKG_DEVICE_ROLE_AP && info->role != LINKG_DEVICE_ROLE_STA)
    {
        return false;
    }

    if (!linkg_network_ipv4_address_valid(&info->ethernet.ip))
    {
        return false;
    }

    return linkg_network_ipv4_netmask_valid(&info->ethernet.netmask);
}

/**
 * @brief 校验直接对端角色。
 */
static bool _linkg_node_peer_role_valid(linkg_device_role_t local_role, linkg_device_role_t peer_role)
{
    if (local_role == LINKG_DEVICE_ROLE_AP)
    {
        return peer_role == LINKG_DEVICE_ROLE_STA;
    }

    if (local_role == LINKG_DEVICE_ROLE_STA)
    {
        return peer_role == LINKG_DEVICE_ROLE_AP;
    }

    return false;
}

/**
 * @brief 校验路径端点。
 */
static bool _linkg_node_endpoint_valid(const linkg_path_endpoint_t *endpoint)
{
    const struct sockaddr_in6 *address6;
    const struct sockaddr_in *address4;

    if (endpoint == NULL)
    {
        return false;
    }

    if (endpoint->address.ss_family == AF_INET)
    {
        if (endpoint->length != sizeof(struct sockaddr_in))
        {
            return false;
        }

        address4 = (const struct sockaddr_in *)&endpoint->address;

        return linkg_network_ipv4_address_valid(&address4->sin_addr) && address4->sin_port != 0U;
    }

    if (endpoint->address.ss_family == AF_INET6)
    {
        if (endpoint->length != sizeof(struct sockaddr_in6))
        {
            return false;
        }

        address6 = (const struct sockaddr_in6 *)&endpoint->address;

        return !IN6_IS_ADDR_UNSPECIFIED(&address6->sin6_addr) &&
               !IN6_IS_ADDR_MULTICAST(&address6->sin6_addr) &&
               address6->sin6_port != 0U;
    }

    return false;
}

/**
 * @brief 判断两个路径端点是否相同。
 */
static bool _linkg_node_endpoint_equal(const linkg_path_endpoint_t *left, const linkg_path_endpoint_t *right)
{
    const struct sockaddr_in6 *left6;
    const struct sockaddr_in6 *right6;
    const struct sockaddr_in *left4;
    const struct sockaddr_in *right4;

    if (left == NULL || right == NULL || left->length != right->length || left->address.ss_family != right->address.ss_family)
    {
        return false;
    }

    if (left->address.ss_family == AF_INET)
    {
        left4 = (const struct sockaddr_in *)&left->address;
        right4 = (const struct sockaddr_in *)&right->address;

        return left4->sin_addr.s_addr == right4->sin_addr.s_addr && left4->sin_port == right4->sin_port;
    }

    if (left->address.ss_family == AF_INET6)
    {
        left6 = (const struct sockaddr_in6 *)&left->address;
        right6 = (const struct sockaddr_in6 *)&right->address;

        return memcmp(&left6->sin6_addr, &right6->sin6_addr, sizeof(left6->sin6_addr)) == 0 &&
               left6->sin6_port == right6->sin6_port &&
               left6->sin6_scope_id == right6->sin6_scope_id;
    }

    return false;
}

/**
 * @brief 查找指定对端节点槽位。
 *
 * @note 调用方必须持有Node状态锁。
 */
static linkg_node_peer_slot_t *_linkg_node_find_peer_locked(const struct in_addr *tun_address)
{
    uint32_t index;

    if (tun_address == NULL)
    {
        return NULL;
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        if (!g_node.peers[index].valid && !g_node.peers[index].retiring)
        {
            continue;
        }

        if (g_node.peers[index].info.tun_address.s_addr == tun_address->s_addr)
        {
            return &g_node.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 查找空闲对端节点槽位。
 *
 * @note 调用方必须持有Node状态锁。
 */
static linkg_node_peer_slot_t *_linkg_node_find_unused_peer_locked(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        if (!g_node.peers[index].valid &&
            !g_node.peers[index].retiring &&
            g_node.peers[index].info.tun_address.s_addr == 0U)
        {
            return &g_node.peers[index];
        }
    }

    return NULL;
}

/**
 * @brief 查找指定链路路径槽位。
 *
 * @note 调用方必须持有Node状态锁。
 */
static int _linkg_node_find_path_index_locked(const linkg_node_peer_slot_t *slot, uint32_t link_id)
{
    uint32_t index;

    if (slot == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return -1;
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (linkg_path_get_state(&slot->paths[index]) == LINKG_PATH_STATE_EMPTY)
        {
            continue;
        }

        if (slot->paths[index].link_id == link_id)
        {
            return (int)index;
        }
    }

    return -1;
}

/**
 * @brief 查找指定路径对象槽位。
 *
 * @note 调用方必须持有Node状态锁。
 */
static int _linkg_node_find_path_pointer_index_locked(const linkg_node_peer_slot_t *slot, const linkg_path_t *path)
{
    uint32_t index;

    if (slot == NULL || path == NULL)
    {
        return -1;
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (&slot->paths[index] == path)
        {
            return (int)index;
        }
    }

    return -1;
}

/**
 * @brief 查找空闲路径槽位。
 *
 * @note 调用方必须持有Node状态锁。
 */
static int _linkg_node_find_unused_path_index_locked(const linkg_node_peer_slot_t *slot)
{
    uint32_t index;

    if (slot == NULL)
    {
        return -1;
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (linkg_path_get_state(&slot->paths[index]) == LINKG_PATH_STATE_EMPTY)
        {
            return (int)index;
        }
    }

    return -1;
}

/**
 * @brief 判断节点所有路径是否已经释放。
 *
 * @note 调用方必须持有Node状态锁。
 */
static bool _linkg_node_paths_empty_locked(const linkg_node_peer_slot_t *slot)
{
    uint32_t index;

    if (slot == NULL)
    {
        return false;
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (linkg_path_get_state(&slot->paths[index]) != LINKG_PATH_STATE_EMPTY)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 尝试完成对端节点释放。
 *
 * @note 调用方必须持有Node状态锁，仅当Peer处于retiring且全部Path已回收时清空槽位。
 */
static void _linkg_node_complete_peer_release_locked(linkg_node_peer_slot_t *slot)
{
    if (slot == NULL || !slot->retiring || !_linkg_node_paths_empty_locked(slot))
    {
        return;
    }

    slot->valid = false;
    slot->path_count = 0U;
    memset(&slot->info, 0, sizeof(slot->info));

    slot->retiring = false;
}

/**
 * @brief 退役并回收路径。
 *
 * @note 调用方必须持有Node状态锁；无异步引用时同步reset，否则等待Path回调。
 */
static int _linkg_node_retire_path_locked(linkg_path_t *path)
{
    bool released;
    int ret;

    if (path == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_path_retire(path, &released);
    if (ret != 0)
    {
        return ret;
    }

    if (released)
    {
        return linkg_path_reset(path);
    }

    return 0;
}

/**
 * @brief 处理异步路径释放完成。
 *
 * @note 本回调由最后一个Path异步引用释放线程触发，内部重新获取Node状态锁完成回收。
 */
static void _linkg_node_path_released(linkg_path_t *path, void *user_data)
{
    linkg_node_peer_slot_t *slot;
    linkg_path_state_t state;

    if (path == NULL || user_data == NULL)
    {
        return;
    }

    slot = (linkg_node_peer_slot_t *)user_data;

    if (_linkg_node_lock() != 0)
    {
        return;
    }

    if (_linkg_node_find_path_pointer_index_locked(slot, path) < 0)
    {
        (void)_linkg_node_unlock(0);
        return;
    }

    state = linkg_path_get_state(path);

    if (state == LINKG_PATH_STATE_RELEASED)
    {
        if (linkg_path_reset(path) != 0)
        {
            (void)_linkg_node_unlock(0);
            return;
        }
    }
    else if (state != LINKG_PATH_STATE_EMPTY)
    {
        (void)_linkg_node_unlock(0);
        return;
    }

    _linkg_node_complete_peer_release_locked(slot);

    (void)_linkg_node_unlock(0);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化节点管理模块。
 *
 * @note Node是全部Peer和Path生命周期Owner，生命周期接口必须由管理线程串行调用。
 */
int linkg_node_init(const linkg_node_info_t *local)
{
    uint32_t path_index;
    uint32_t peer_index;
    int ret;

    if (!_linkg_node_info_valid(local))
    {
        return -EINVAL;
    }

    if (g_node.initialized)
    {
        return -EALREADY;
    }

    memset(&g_node, 0, sizeof(g_node));

    ret = pthread_mutex_init(&g_node.lock, NULL);
    if (ret != 0)
    {
        return -ret;
    }

    for (peer_index = 0U; peer_index < LINKG_NODE_PEER_MAX; peer_index++)
    {
        for (path_index = 0U; path_index < LINKG_NODE_PATH_MAX; path_index++)
        {
            ret = linkg_path_init(&g_node.peers[peer_index].paths[path_index]);
            if (ret != 0)
            {
                goto fail_paths;
            }
        }
    }

    g_node.local = *local;
    g_node.initialized = true;

    return 0;

fail_paths:
    for (peer_index = 0U; peer_index < LINKG_NODE_PEER_MAX; peer_index++)
    {
        for (path_index = 0U; path_index < LINKG_NODE_PATH_MAX; path_index++)
        {
            (void)linkg_path_deinit(&g_node.peers[peer_index].paths[path_index]);
        }
    }

    (void)pthread_mutex_destroy(&g_node.lock);
    memset(&g_node, 0, sizeof(g_node));

    return ret;
}

/**
 * @brief 反初始化节点管理模块。
 *
 * @note 调用前所有Peer和Path必须已经完成回收，并且不能再有其他线程访问Node模块；
 *       该约束保证Path内部统计锁可以安全销毁。
 */
int linkg_node_deinit(void)
{
    uint32_t path_index;
    uint32_t peer_index;
    uint32_t index;
    int ret;

    if (!g_node.initialized)
    {
        return 0;
    }

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    if (g_node.peer_count != 0U)
    {
        return _linkg_node_unlock(-EBUSY);
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        if (g_node.peers[index].retiring || !_linkg_node_paths_empty_locked(&g_node.peers[index]))
        {
            return _linkg_node_unlock(-EBUSY);
        }
    }

    for (peer_index = 0U; peer_index < LINKG_NODE_PEER_MAX; peer_index++)
    {
        for (path_index = 0U; path_index < LINKG_NODE_PATH_MAX; path_index++)
        {
            ret = linkg_path_deinit(&g_node.peers[peer_index].paths[path_index]);
            if (ret != 0)
            {
                return _linkg_node_unlock(ret);
            }
        }
    }

    g_node.initialized = false;

    ret = _linkg_node_unlock(0);
    if (ret != 0)
    {
        return ret;
    }

    ret = pthread_mutex_destroy(&g_node.lock);
    if (ret != 0)
    {
        return -ret;
    }

    memset(&g_node, 0, sizeof(g_node));

    return 0;
}

/****************************** 节点查询 ******************************/

/**
 * @brief 获取本机节点信息。
 *
 * @note 返回指针归Node模块所有，仅在Node保持初始化期间有效。
 */
const linkg_node_info_t *linkg_node_get_local(void)
{
    return g_node.initialized ? &g_node.local : NULL;
}

/**
 * @brief 获取直接对端节点快照。
 */
int linkg_node_get_peer_snapshot(const struct in_addr *tun_address, linkg_node_peer_snapshot_t *snapshot)
{
    linkg_node_peer_slot_t *slot;
    int ret = 0;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (tun_address == NULL || snapshot == NULL)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    slot = _linkg_node_find_peer_locked(tun_address);
    if (slot == NULL ||
        !slot->valid ||
        slot->retiring)
    {
        ret = -ENOENT;
        goto out;
    }

    snapshot->info = slot->info;
    snapshot->path_count = slot->path_count;

out:
    return _linkg_node_unlock(ret);
}

/****************************** 对端管理 ******************************/

/**
 * @brief 注册或更新直接对端节点。
 */
int linkg_node_register_peer(const linkg_node_info_t *info)
{
    linkg_node_peer_slot_t *slot;
    int ret;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (!_linkg_node_info_valid(info))
    {
        return -EINVAL;
    }

    if (info->tun_address.s_addr == g_node.local.tun_address.s_addr)
    {
        return -EINVAL;
    }

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    if (!_linkg_node_peer_role_valid(g_node.local.role, info->role))
    {
        ret = -EINVAL;
        goto out;
    }

    slot = _linkg_node_find_peer_locked(&info->tun_address);
    if (slot != NULL)
    {
        if (slot->retiring)
        {
            ret = -EBUSY;
            goto out;
        }

        if (slot->valid)
        {
            if (slot->info.role != info->role)
            {
                ret = -EINVAL;
                goto out;
            }

            slot->info = *info;
            ret = 0;
            goto out;
        }
    }

    if (g_node.local.role == LINKG_DEVICE_ROLE_STA && g_node.peer_count != 0U)
    {
        ret = -ENOSPC;
        goto out;
    }

    slot = _linkg_node_find_unused_peer_locked();
    if (slot == NULL)
    {
        ret = -ENOSPC;
        goto out;
    }

    slot->info = *info;
    slot->path_count = 0U;
    slot->valid = true;
    slot->retiring = false;

    g_node.peer_count++;

    ret = 0;

out:
    return _linkg_node_unlock(ret);
}

/**
 * @brief 注销直接对端节点并退役全部路径。
 *
 * @note Peer先逻辑下线并停止产生新Path引用，已有异步引用完成后由Path回调最终释放Peer槽位。
 */
int linkg_node_unregister_peer(const struct in_addr *tun_address)
{
    linkg_node_peer_slot_t *slot;
    uint32_t index;
    int first_error;
    int ret;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (tun_address == NULL)
    {
        return -EINVAL;
    }

    first_error = 0;

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    slot = _linkg_node_find_peer_locked(tun_address);
    if (slot == NULL)
    {
        ret = -ENOENT;
        goto out;
    }

    if (slot->retiring)
    {
        ret = 0;
        goto out;
    }

    if (!slot->valid)
    {
        ret = -ENOENT;
        goto out;
    }

    // 先逻辑下线，禁止产生新的Path引用。
    slot->valid = false;
    slot->retiring = true;

    if (g_node.peer_count > 0U)
    {
        g_node.peer_count--;
    }

    // 退役全部路径。
    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (linkg_path_get_state(&slot->paths[index]) == LINKG_PATH_STATE_EMPTY)
        {
            continue;
        }

        ret = _linkg_node_retire_path_locked(&slot->paths[index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    slot->path_count = 0U;

    // 没有残留引用时立即回收，否则等待Path释放回调。
    _linkg_node_complete_peer_release_locked(slot);

    ret = first_error;

out:
    return _linkg_node_unlock(ret);
}

/****************************** 路径管理 ******************************/

/**
 * @brief 注册或更新指定对端链路路径。
 *
 * @note 同一Peer每个Link只保留一条Path；活动Path只更新Endpoint，不重复创建。
 */
int linkg_node_register_path(const struct in_addr *tun_address, uint32_t link_id, const linkg_path_endpoint_t *next_hop)
{
    linkg_node_peer_slot_t *slot;
    linkg_path_state_t state;
    linkg_path_t *path;
    int path_index;
    int ret;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (tun_address == NULL || link_id == LINKG_LINK_ID_INVALID || !_linkg_node_endpoint_valid(next_hop))
    {
        return -EINVAL;
    }

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    slot = _linkg_node_find_peer_locked(tun_address);
    if (slot == NULL || !slot->valid || slot->retiring)
    {
        ret = -ENOENT;
        goto out;
    }

    path_index = _linkg_node_find_path_index_locked(slot, link_id);

    if (path_index >= 0)
    {
        path = &slot->paths[path_index];
        state = linkg_path_get_state(path);

        if (state == LINKG_PATH_STATE_ACTIVE)
        {
            if (_linkg_node_endpoint_equal(&path->next_hop, next_hop))
            {
                ret = 0;
                goto out;
            }

            ret = linkg_path_update_endpoint(path, next_hop);

            goto out;
        }

        if (state == LINKG_PATH_STATE_RETIRED)
        {
            ret = -EBUSY;
            goto out;
        }

        if (state == LINKG_PATH_STATE_RELEASED)
        {
            ret = linkg_path_reset(path);
            if (ret != 0)
            {
                goto out;
            }
        }
    }
    else
    {
        path_index = _linkg_node_find_unused_path_index_locked(slot);
        if (path_index < 0)
        {
            ret = -ENOSPC;
            goto out;
        }

        path = &slot->paths[path_index];
    }

    ret = linkg_path_activate(path, link_id, next_hop, _linkg_node_path_released, slot);
    if (ret != 0)
    {
        goto out;
    }

    slot->path_count++;

out:
    return _linkg_node_unlock(ret);
}

/**
 * @brief 注销指定对端链路路径。
 *
 * @note Path存在异步引用时仅进入RETIRED，最后一个引用释放后由回调完成槽位回收。
 */
int linkg_node_unregister_path(const struct in_addr *tun_address, uint32_t link_id)
{
    linkg_node_peer_slot_t *slot;
    linkg_path_state_t state;
    linkg_path_t *path;
    int path_index;
    int ret;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (tun_address == NULL || link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    slot = _linkg_node_find_peer_locked(tun_address);
    if (slot == NULL || !slot->valid || slot->retiring)
    {
        ret = -ENOENT;
        goto out;
    }

    path_index = _linkg_node_find_path_index_locked(slot, link_id);
    if (path_index < 0)
    {
        ret = -ENOENT;
        goto out;
    }

    path = &slot->paths[path_index];
    state = linkg_path_get_state(path);

    if (state == LINKG_PATH_STATE_RETIRED)
    {
        ret = 0;
        goto out;
    }

    if (state == LINKG_PATH_STATE_RELEASED)
    {
        ret = linkg_path_reset(path);
        goto out;
    }

    if (state != LINKG_PATH_STATE_ACTIVE)
    {
        ret = -ENOENT;
        goto out;
    }

    ret = _linkg_node_retire_path_locked(path);
    if (ret != 0)
    {
        goto out;
    }

    if (slot->path_count > 0U)
    {
        slot->path_count--;
    }

out:
    return _linkg_node_unlock(ret);
}

/**
 * @brief 批量获取指定对端链路路径异步引用。
 *
 * @note 成功后调用方获得reference_count个Path引用，必须对每个引用执行一次linkg_path_release()；
 *       Node锁保证acquire与activate/retire/endpoint更新互斥。
 */
int linkg_node_acquire_path_batch(const struct in_addr *tun_address, uint32_t link_id, uint32_t reference_count, linkg_path_t **path, linkg_path_endpoint_t *next_hop)
{
    linkg_node_peer_slot_t *slot;
    linkg_path_t *node_path;
    int path_index;
    int ret;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (tun_address == NULL || path == NULL || next_hop == NULL ||
        link_id == LINKG_LINK_ID_INVALID || reference_count == 0U)
    {
        return -EINVAL;
    }

    *path = NULL;
    memset(next_hop, 0, sizeof(*next_hop));

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    slot = _linkg_node_find_peer_locked(tun_address);
    if (slot == NULL || !slot->valid || slot->retiring)
    {
        ret = -ENOENT;
        goto out;
    }

    path_index = _linkg_node_find_path_index_locked(slot, link_id);
    if (path_index < 0)
    {
        ret = -ENOENT;
        goto out;
    }

    node_path = &slot->paths[path_index];

    if (!linkg_path_is_active(node_path))
    {
        ret = -ENODEV;
        goto out;
    }

    ret = linkg_path_acquire_batch(node_path, reference_count);
    if (ret == 0)
    {
        *path = node_path;
        *next_hop = node_path->next_hop;
    }

out:
    return _linkg_node_unlock(ret);
}

/**
 * @brief 获取指定对端链路路径异步引用。
 *
 * @note 成功后调用方持有一个Path引用，使用完成后必须调用linkg_path_release()。
 */
int linkg_node_acquire_path(const struct in_addr *tun_address, uint32_t link_id, linkg_path_t **path, linkg_path_endpoint_t *next_hop)
{
    return linkg_node_acquire_path_batch(tun_address, link_id, 1U, path, next_hop);
}

/**
 * @brief 根据物理接收来源记录Path接收统计并获取直接Peer地址。
 *
 * @note 调用方必须持有Node状态锁。
 */
static int _linkg_node_account_path_rx_locked(uint32_t link_id, const linkg_path_endpoint_t *source, uint64_t bytes, uint64_t packets, struct in_addr *peer_address)
{
    linkg_node_peer_slot_t *slot;
    linkg_path_t *path;
    uint32_t peer_index;
    uint32_t path_index;

    for (peer_index = 0U; peer_index < LINKG_NODE_PEER_MAX; peer_index++)
    {
        slot = &g_node.peers[peer_index];

        if (!slot->valid || slot->retiring)
        {
            continue;
        }

        for (path_index = 0U; path_index < LINKG_NODE_PATH_MAX; path_index++)
        {
            path = &slot->paths[path_index];

            if (!linkg_path_is_active(path))
            {
                continue;
            }

            if (path->link_id != link_id || !_linkg_node_endpoint_equal(&path->next_hop, source))
            {
                continue;
            }

            linkg_path_record_rx(path, bytes, packets);
            *peer_address = slot->info.tun_address;

            return 0;
        }
    }

    return -ENOENT;
}

/**
 * @brief 批量根据物理接收来源记录Path接收统计并获取直接Peer地址。
 *
 * @note 整批只获取一次Node状态锁，各元素独立通过item.result返回处理结果。
 */
int linkg_node_account_path_rx_batch(uint32_t link_id, linkg_node_path_rx_item_t *items, uint32_t count)
{
    uint32_t index;
    int ret;

    if (!g_node.initialized)
    {
        return -ENODEV;
    }

    if (link_id == LINKG_LINK_ID_INVALID || items == NULL || count == 0U)
    {
        return -EINVAL;
    }

    for (index = 0U; index < count; index++)
    {
        memset(&items[index].peer_address, 0, sizeof(items[index].peer_address));
        items[index].result = -EINPROGRESS;

        if (items[index].packets == 0U || !_linkg_node_endpoint_valid(&items[index].source))
        {
            items[index].result = -EINVAL;
        }
    }

    ret = _linkg_node_lock();
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (items[index].result != -EINPROGRESS)
        {
            continue;
        }

        items[index].result = _linkg_node_account_path_rx_locked(link_id, &items[index].source, items[index].bytes, items[index].packets, &items[index].peer_address);
    }

    return _linkg_node_unlock(0);
}

/**
 * @brief 根据物理接收来源记录Path接收统计并获取直接Peer地址。
 *
 * @note 单包接口内部复用批量实现。
 */
int linkg_node_account_path_rx(uint32_t link_id, const linkg_path_endpoint_t *source, uint64_t bytes, uint64_t packets, struct in_addr *peer_address)
{
    linkg_node_path_rx_item_t item;
    int ret;

    if (source == NULL || peer_address == NULL)
    {
        return -EINVAL;
    }

    memset(&item, 0, sizeof(item));
    item.source = *source;
    item.bytes = bytes;
    item.packets = packets;

    ret = linkg_node_account_path_rx_batch(link_id, &item, 1U);
    if (ret != 0)
    {
        return ret;
    }

    if (item.result == 0)
    {
        *peer_address = item.peer_address;
    }

    return item.result;
}
