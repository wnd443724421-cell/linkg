/**
 * @file switch_plan_reconcile.c
 * @brief LinkG发送计划与实际Path资源的一致性维护
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
    linkg_send_plan_t   plan;             // 本轮采集的完整Peer发送计划
    uint32_t            generation;       // Peer运行代际，防止旧快照作用到新Peer
    uint8_t             node_id;          // 直接Peer节点编号
    bool                wifi_blocked;     // 本机或对端Maintenance禁止使用Wi-Fi
    bool                cellular_blocked; // 本机或对端Maintenance禁止使用Cellular
} linkg_switch_reconcile_peer_t;

typedef struct
{
    uint32_t wifi_link_id;     // 已确认可用的Wi-Fi Link ID，无效表示不可用
    uint32_t cellular_link_id; // 已确认可用的Cellular Link ID，无效表示不可用
} linkg_switch_reconcile_paths_t;

/****************************** Peer快照 ******************************/

/**
 * @brief 在Switch锁内读取指定槽位的Peer和Maintenance快照。
 *
 * @return 1表示有效Peer，0表示空槽位，负值表示错误。
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

    peer = &g_switch.peers[index];
    if (!peer->used)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    snapshot->node_id          = peer->peer_node_id;
    snapshot->generation       = peer->generation;
    snapshot->plan             = peer->plan;
    snapshot->wifi_blocked     = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_WIFI);
    snapshot->cellular_blocked = linkg_switch_maintenance_access_blocked_locked(&peer->maintenance, LINKG_LINK_ACCESS_CELLULAR);

    pthread_mutex_unlock(&g_switch.lock);

    return 1;
}

/****************************** Path资源查询 ******************************/

/**
 * @brief 查询Peer指定Access对应的真实可用Path。
 *
 * 只判断Link运行状态、Node Path存在性及Maintenance约束；
 * 不在这里依据RSSI、丢包率或RTT作策略切换判断。
 * Link或Path不存在属于正常资源状态，不视为模块处理错误。
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
    if (ret == -ENOENT || ret == -ENODEV || ret == -ENETDOWN)
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
 * @brief 每轮独立获取Peer的Wi-Fi与Cellular可用资源。
 *
 * 无需先查询Node Path总数；逐Access获取Path已经包含Peer存在性检查。
 */
static int _linkg_switch_reconcile_get_paths(const linkg_switch_reconcile_peer_t *peer, linkg_switch_reconcile_paths_t *paths)
{
    int ret;

    if (peer == NULL || paths == NULL)
    {
        return -EINVAL;
    }

    memset(paths, 0, sizeof(*paths));

    ret = _linkg_switch_reconcile_get_link(peer->node_id, LINKG_LINK_ACCESS_WIFI, peer->wifi_blocked, &paths->wifi_link_id);
    if (ret != 0)
    {
        return ret;
    }

    return _linkg_switch_reconcile_get_link(peer->node_id, LINKG_LINK_ACCESS_CELLULAR, peer->cellular_blocked, &paths->cellular_link_id);
}

/****************************** 计划资源修复 ******************************/

/**
 * @brief 检查指定Link ID是否属于本轮确认可用的Path。
 */
static bool _linkg_switch_reconcile_link_available(const linkg_switch_reconcile_paths_t *paths, uint32_t link_id)
{
    return link_id != LINKG_LINK_ID_INVALID &&
           (link_id == paths->wifi_link_id || link_id == paths->cellular_link_id);
}

/**
 * @brief 根据真实Path修复单个业务类别的发送计划。
 *
 * 仅维护资源关系，不根据质量主动切换主链路，也不主动开启REDUNDANT：
 * 1. 原双发两条Path均有效时维持REDUNDANT；
 * 2. Primary有效则原样保留，补充或移除Secondary候选；
 * 3. Primary失效时优先提升仍可用的旧Secondary；
 * 4. 两个旧Link均不可用时，选择一个实际可用Path兜底；
 * 5. 所有Path失效时停止该业务发送。
 */
static void _linkg_switch_reconcile_build_class(const linkg_class_send_plan_t *current, const linkg_switch_reconcile_paths_t *paths, linkg_class_send_plan_t *expected)
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

    if (current->mode == LINKG_SEND_MODE_REDUNDANT && primary_available && secondary_available &&
        current->primary_link_id != current->secondary_link_id)
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
 * @brief 独立修复三类业务，保留各类当前的有效主链路与冗余模式。
 */
static void _linkg_switch_reconcile_build(const linkg_send_plan_t *current, const linkg_switch_reconcile_paths_t *paths, linkg_send_plan_t *expected)
{
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        _linkg_switch_reconcile_build_class(&current->classes[class_index], paths, &expected->classes[class_index]);
    }
}

/**
 * @brief 判断两个Peer发送计划的业务字段是否一致。
 */
static bool _linkg_switch_reconcile_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (left->classes[class_index].mode != right->classes[class_index].mode ||
            left->classes[class_index].primary_link_id != right->classes[class_index].primary_link_id ||
            left->classes[class_index].secondary_link_id != right->classes[class_index].secondary_link_id)
        {
            return false;
        }
    }

    return true;
}

/****************************** 计划提交 ******************************/

/**
 * @brief 将资源修复后的完整Plan条件更新到本机，不发起远端同步。
 *
 * AP和STA各自按本机真实Link/Path修复发送计划；质量切换和PLAN_SYNC
 * 不属于本模块职责。Plan模块在同一Switch锁内比较previous并写入，
 * 避免本轮过期快照覆盖Policy或Maintenance刚提交的新计划。
 * 写入时仍须遵守本机及对端Maintenance约束。
 */
static int _linkg_switch_reconcile_commit(const linkg_switch_reconcile_peer_t *snapshot, const linkg_send_plan_t *expected, uint64_t now_us)
{
    return linkg_switch_plan_update_if_current(snapshot->node_id,
                                                snapshot->generation,
                                                &snapshot->plan,
                                                expected,
                                                false,
                                                now_us);
}

/****************************** 周期资源校准 ******************************/

/**
 * @brief 周期修复全部直接Peer的发送计划资源关系。
 *
 * AP与STA独立维护本地Link/Path增减及硬失效，不发起PLAN_SYNC；
 * RSSI/丢包/RTT策略切换由Policy负责，远端计划同步由Plan模块负责。
 * 由Switch Worker调用，调用时不得持有g_switch.lock。
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

        ret = _linkg_switch_reconcile_commit(&peer, &expected, now_us);
        if (ret == -EAGAIN || ret == -ESTALE || ret == -EBUSY ||
            ret == -ENOENT || ret == -ENODEV || ret == -ENETDOWN)
        {
            // 本轮采集后资源或Maintenance发生变化，下一轮重新核对。
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

        LINKG_LOG_INFO("SWITCH-RECONCILE: peer=%u repaired plan resources", (unsigned int)peer.node_id);
    }

    return first_error;
}
