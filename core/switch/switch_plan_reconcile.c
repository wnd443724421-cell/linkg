/**
 * @file switch_plan_reconcile.c
 * @brief LinkG发送计划资源一致性校准实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-09
 */

#include "switch_plan_reconcile.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_path.h"

#include "switch_internal.h"
#include "switch_maintenance.h"
#include "switch_plan.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_send_plan_t   plan;             // 当前发送计划快照
    uint32_t            generation;       // Peer运行代际
    uint8_t             node_id;          // 直接Peer编号
    linkg_device_role_t role;             // 本机角色
    bool                wifi_blocked;     // Wi-Fi是否处于Maintenance Block
    bool                cellular_blocked; // Cellular是否处于Maintenance Block
} linkg_switch_reconcile_peer_t;

typedef struct
{
    uint32_t wifi_link_id;     // 当前可用Wi-Fi Path所属Link ID
    uint32_t cellular_link_id; // 当前可用Cellular Path所属Link ID
} linkg_switch_reconcile_paths_t;

/****************************** 快照与可用性 ******************************/

/**
 * @brief 在Switch锁内读取指定槽位的Peer快照。
 *
 * @return 1表示槽位已使用，0表示空槽位，负值表示错误。
 */
static int _linkg_switch_reconcile_snapshot_peer(uint32_t index, linkg_switch_reconcile_peer_t *snapshot)
{
    linkg_switch_peer_runtime_t *peer;

    if (snapshot == NULL || index >= LINKG_SWITCH_PEER_MAX)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized || !g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    peer = &g_switch.peers[index];
    if (!peer->used)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    snapshot->node_id          = peer->peer_node_id;
    snapshot->generation       = peer->generation;
    snapshot->role             = g_switch.role;
    snapshot->plan             = peer->plan;
    snapshot->wifi_blocked     = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_WIFI);
    snapshot->cellular_blocked = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_CELLULAR);

    pthread_mutex_unlock(&g_switch.lock);

    return 1;
}

/**
 * @brief 检查指定Access是否存在实际可发送的Node Path。
 */
static int _linkg_switch_reconcile_get_link(uint8_t node_id, linkg_link_access_t access, bool blocked, uint32_t *link_id)
{
    linkg_path_endpoint_t endpoint;
    linkg_link_t         *link;
    linkg_path_t         *path;
    uint32_t              current_id;
    int                   ret;

    if (link_id == NULL)
    {
        return -EINVAL;
    }

    *link_id = LINKG_LINK_ID_INVALID;

    if (blocked)
    {
        return 0;
    }

    link = linkg_link_manager_get_by_access(access);
    if (link == NULL || !linkg_link_is_running(link))
    {
        return 0;
    }

    current_id = linkg_link_get_id(link);
    if (current_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    path = NULL;
    memset(&endpoint, 0, sizeof(endpoint));

    ret = linkg_node_acquire_path(node_id, current_id, &path, &endpoint);
    if (ret == -ENOENT || ret == -ENODEV)
    {
        return 0;
    }
    if (ret != 0)
    {
        return ret;
    }

    linkg_path_release(path);
    *link_id = current_id;

    return 0;
}

/**
 * @brief 获取当前Peer真实可用的Wi-Fi与Cellular Path。
 */
static int _linkg_switch_reconcile_get_paths(const linkg_switch_reconcile_peer_t *peer, linkg_switch_reconcile_paths_t *paths)
{
    linkg_node_peer_snapshot_t snapshot;
    int                             ret;

    if (peer == NULL || paths == NULL)
    {
        return -EINVAL;
    }

    memset(paths, 0, sizeof(*paths));
    memset(&snapshot, 0, sizeof(snapshot));

    ret = linkg_node_get_peer_snapshot(peer->node_id, &snapshot);
    if (ret == -ENOENT)
    {
        // Node已注销Peer，原Plan只能作为暂存状态，不再允许发送。
        return 0;
    }
    if (ret != 0)
    {
        return ret;
    }

    if (snapshot.path_count == 0U)
    {
        return 0;
    }

    ret = _linkg_switch_reconcile_get_link(peer->node_id, LINKG_LINK_ACCESS_WIFI, peer->wifi_blocked, &paths->wifi_link_id);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_switch_reconcile_get_link(peer->node_id, LINKG_LINK_ACCESS_CELLULAR, peer->cellular_blocked, &paths->cellular_link_id);
}

/****************************** 计划计算 ******************************/

/**
 * @brief 判断Link ID是否属于当前可用的两条Path。
 */
static bool _linkg_switch_reconcile_link_available(const linkg_switch_reconcile_paths_t *paths, uint32_t link_id)
{
    return link_id != LINKG_LINK_ID_INVALID && (link_id == paths->wifi_link_id || link_id == paths->cellular_link_id);
}

/**
 * @brief 根据旧Plan和真实Path计算目标Plan，不改变任何运行资源。
 */
static void _linkg_switch_reconcile_build(const linkg_send_plan_t *current, const linkg_switch_reconcile_paths_t *paths, linkg_send_plan_t *expected)
{
    uint32_t primary_link_id;
    uint32_t secondary_link_id;
    bool     primary_available;
    bool     secondary_available;

    memset(expected, 0, sizeof(*expected));

    expected->mode              = LINKG_SEND_MODE_NONE;
    expected->primary_link_id   = LINKG_LINK_ID_INVALID;
    expected->secondary_link_id = LINKG_LINK_ID_INVALID;

    if (paths->wifi_link_id == LINKG_LINK_ID_INVALID && paths->cellular_link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    primary_available   = _linkg_switch_reconcile_link_available(paths, current->primary_link_id);
    secondary_available = _linkg_switch_reconcile_link_available(paths, current->secondary_link_id);

    // 只有原双发计划的主备实例均有效时，才继续保持REDUNDANT。
    if (current->mode == LINKG_SEND_MODE_REDUNDANT && primary_available && secondary_available && current->primary_link_id != current->secondary_link_id)
    {
        *expected = *current;
        return;
    }

    if (primary_available)
    {
        primary_link_id = current->primary_link_id;
    }
    else if (secondary_available)
    {
        primary_link_id = current->secondary_link_id;
    }
    else if (paths->wifi_link_id != LINKG_LINK_ID_INVALID)
    {
        primary_link_id = paths->wifi_link_id;
    }
    else
    {
        primary_link_id = paths->cellular_link_id;
    }

    secondary_link_id = LINKG_LINK_ID_INVALID;

    if (paths->wifi_link_id != LINKG_LINK_ID_INVALID && paths->wifi_link_id != primary_link_id)
    {
        secondary_link_id = paths->wifi_link_id;
    }
    else if (paths->cellular_link_id != LINKG_LINK_ID_INVALID && paths->cellular_link_id != primary_link_id)
    {
        secondary_link_id = paths->cellular_link_id;
    }

    expected->mode              = LINKG_SEND_MODE_SINGLE;
    expected->primary_link_id   = primary_link_id;
    expected->secondary_link_id = secondary_link_id;
}

/**
 * @brief 比较两个Plan的三个实际业务字段。
 */
static bool _linkg_switch_reconcile_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    return left->mode == right->mode && left->primary_link_id == right->primary_link_id && left->secondary_link_id == right->secondary_link_id;
}

/**
 * @brief 提交前复核Peer代际、Plan及Maintenance快照未变化。
 */
static int _linkg_switch_reconcile_validate_snapshot(const linkg_switch_reconcile_peer_t *snapshot)
{
    linkg_switch_peer_runtime_t *peer;
    bool                         wifi_blocked;
    bool                         cellular_blocked;
    int                          ret;

    ret = 0;

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized || !g_switch.running)
    {
        ret = -ESHUTDOWN;
        goto out;
    }

    peer = linkg_switch_find_peer_generation_locked(snapshot->node_id, snapshot->generation);
    if (peer == NULL)
    {
        ret = -ESTALE;
        goto out;
    }

    wifi_blocked     = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_WIFI);
    cellular_blocked = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_CELLULAR);

    if (!_linkg_switch_reconcile_plan_equal(&peer->plan, &snapshot->plan) || wifi_blocked != snapshot->wifi_blocked || cellular_blocked != snapshot->cellular_blocked || g_switch.role != snapshot->role)
    {
        ret = -EAGAIN;
    }

out:
    pthread_mutex_unlock(&g_switch.lock);

    return ret;
}

/**
 * @brief 计划进入NONE后终止STA尚未完成的旧同步重试。
 */
static void _linkg_switch_reconcile_cancel_none_sync(const linkg_switch_reconcile_peer_t *snapshot)
{
    linkg_switch_peer_runtime_t *peer;

    if (snapshot->role != LINKG_DEVICE_ROLE_STA)
    {
        return;
    }

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(snapshot->node_id, snapshot->generation);
    if (peer != NULL && peer->plan.mode == LINKG_SEND_MODE_NONE)
    {
        peer->role.sta.plan_sync.active        = false;
        peer->role.sta.plan_sync.next_retry_us = 0U;
        peer->role.sta.plan_sync.deadline_us   = 0U;
        peer->role.sta.plan_sync.last_status   = -ENETDOWN;
    }

    pthread_mutex_unlock(&g_switch.lock);
}

/**
 * @brief 本机应用目标Plan，STA需要时同步到AP。
 */
static int _linkg_switch_reconcile_commit(const linkg_switch_reconcile_peer_t *snapshot, const linkg_send_plan_t *expected, uint64_t now_us)
{
    int ret;

    ret = _linkg_switch_reconcile_validate_snapshot(snapshot);
    if (ret != 0)
    {
        return ret;
    }

    if (snapshot->role == LINKG_DEVICE_ROLE_AP || expected->mode == LINKG_SEND_MODE_NONE)
    {
        ret = linkg_switch_plan_apply_local(snapshot->node_id, snapshot->generation, expected);
        if (ret == 0 && expected->mode == LINKG_SEND_MODE_NONE)
        {
            _linkg_switch_reconcile_cancel_none_sync(snapshot);
        }

        return ret;
    }

    if (snapshot->role == LINKG_DEVICE_ROLE_STA)
    {
        return linkg_switch_plan_commit_local(snapshot->node_id, snapshot->generation, expected, now_us);
    }

    return -EPERM;
}

/****************************** 周期校准 ******************************/

/**
 * @brief 每轮处理全部当前Switch Peer，仅在真实Path和Plan不一致时提交。
 *
 * @note 必须由Switch Worker调用，不得持有g_switch.lock。
 */
int linkg_switch_plan_reconcile_process(uint64_t now_us)
{
    linkg_switch_reconcile_peer_t  peer;
    linkg_switch_reconcile_paths_t paths;
    linkg_send_plan_t              expected;
    uint32_t                       index;
    int                            first_error;
    int                            ret;

    if (now_us == 0U)
    {
        return -EINVAL;
    }

    first_error = 0;

    for (index = 0U; index < LINKG_SWITCH_PEER_MAX; index++)
    {
        ret = _linkg_switch_reconcile_snapshot_peer(index, &peer);
        if (ret < 0)
        {
            return ret;
        }
        if (ret == 0)
        {
            continue;
        }

        ret = _linkg_switch_reconcile_get_paths(&peer, &paths);
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        _linkg_switch_reconcile_build(&peer.plan, &paths, &expected);
        if (_linkg_switch_reconcile_plan_equal(&peer.plan, &expected))
        {
            continue;
        }

        // 仅在需要修改时再次确认Path，缩小状态采集到提交之间的窗口。
        ret = _linkg_switch_reconcile_get_paths(&peer, &paths);
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        _linkg_switch_reconcile_build(&peer.plan, &paths, &expected);
        if (_linkg_switch_reconcile_plan_equal(&peer.plan, &expected))
        {
            continue;
        }

        ret = _linkg_switch_reconcile_commit(&peer, &expected, now_us);
        if (ret == -EAGAIN || ret == -ESTALE || ret == -EBUSY)
        {
            // 同期Maintenance/Plan修改属于正常竞争，下轮重新校准。
            continue;
        }
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        LINKG_LOG_INFO("SWITCH-RECONCILE: peer=%u mode=%d primary=%u secondary=%u", (unsigned int)peer.node_id, (int)expected.mode, expected.primary_link_id, expected.secondary_link_id);
    }

    return first_error;
}
