/**
 * @file discovery_peer.c
 * @brief LinkG设备发现直接Peer状态管理与事件处理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "discovery_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_log.h"

/****************************** 模块常量 ******************************/

#define LINKG_DISCOVERY_ACCESS_INDEX_WIFI      0U // Wi-Fi存活状态索引
#define LINKG_DISCOVERY_ACCESS_INDEX_CELLULAR  1U // Cellular存活状态索引

/****************************** Access映射 ******************************/

/**
 * @brief 获取Discovery Access日志名称。
 */
static const char *_linkg_discovery_access_name(uint32_t index)
{
    if (index == LINKG_DISCOVERY_ACCESS_INDEX_WIFI)
    {
        return "wifi";
    }

    if (index == LINKG_DISCOVERY_ACCESS_INDEX_CELLULAR)
    {
        return "cellular";
    }

    return "unknown";
}

/**
 * @brief 将Discovery Access转换为内部数组索引。
 */
int _linkg_discovery_access_index(linkg_link_access_t access, uint32_t *index)
{
    if (index == NULL)
    {
        return -EINVAL;
    }

    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        *index = LINKG_DISCOVERY_ACCESS_INDEX_WIFI;
        return 0;
    }

    if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        *index = LINKG_DISCOVERY_ACCESS_INDEX_CELLULAR;
        return 0;
    }

    return -EINVAL;
}

/**
 * @brief 将内部数组索引转换为Discovery Access。
 */
static int _linkg_discovery_index_access(uint32_t index, linkg_link_access_t *access)
{
    if (access == NULL)
    {
        return -EINVAL;
    }

    if (index == LINKG_DISCOVERY_ACCESS_INDEX_WIFI)
    {
        *access = LINKG_LINK_ACCESS_WIFI;
        return 0;
    }

    if (index == LINKG_DISCOVERY_ACCESS_INDEX_CELLULAR)
    {
        *access = LINKG_LINK_ACCESS_CELLULAR;
        return 0;
    }

    return -EINVAL;
}

/****************************** Peer查找 ******************************/

/**
 * @brief 根据Node ID查找直接Peer状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
static linkg_discovery_peer_t *_linkg_discovery_find_peer_locked(uint8_t node_id)
{
    linkg_discovery_peer_t *peer;
    uint32_t                index;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used)
        {
            continue;
        }

        if (peer->report.node.node_id == node_id)
        {
            return peer;
        }
    }

    return NULL;
}

/**
 * @brief 获取可用于新Peer的状态槽位。
 *
 * 优先使用空闲槽位，槽位耗尽时允许回收已经超过Tombstone
 * 保护时间且不存在遗留Route清理任务的离线Peer。
 *
 * 调用方必须持有Discovery状态锁。
 */
static linkg_discovery_peer_t *_linkg_discovery_acquire_peer_slot_locked(uint64_t now_us)
{
    linkg_discovery_peer_t *peer;
    uint32_t                index;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used)
        {
            return peer;
        }
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used || peer->online)
        {
            continue;
        }

        if (peer->route_cleanup_pending)
        {
            continue;
        }

        if (peer->offline_since_us == 0U || now_us < peer->offline_since_us)
        {
            continue;
        }

        if (now_us - peer->offline_since_us < LINKG_DISCOVERY_PEER_TOMBSTONE_TIMEOUT_US)
        {
            continue;
        }

        memset(peer, 0, sizeof(*peer));

        return peer;
    }

    return NULL;
}

/****************************** Access状态 ******************************/

/**
 * @brief 判断Peer是否仍存在活动Discovery Access。
 *
 * 调用方必须持有Discovery状态锁。
 */
static bool _linkg_discovery_peer_has_active_liveness_locked(const linkg_discovery_peer_t *peer)
{
    uint32_t index;

    if (peer == NULL)
    {
        return false;
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (peer->liveness[index].registered && peer->liveness[index].active)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 清空Peer全部Discovery Access存活状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
static void _linkg_discovery_clear_peer_liveness_locked(linkg_discovery_peer_t *peer)
{
    if (peer == NULL)
    {
        return;
    }

    memset(peer->liveness, 0, sizeof(peer->liveness));
}

/**
 * @brief 记录Peer通过指定Discovery Access收到有效完整状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
static void _linkg_discovery_mark_liveness_seen_locked(linkg_discovery_peer_t *peer, uint32_t index, uint64_t now_us)
{
    linkg_discovery_liveness_t *liveness;

    if (peer == NULL || index >= LINKG_NODE_PATH_MAX)
    {
        return;
    }

    liveness = &peer->liveness[index];

    liveness->registered   = true;
    liveness->active       = true;
    liveness->last_seen_us = now_us;
}

/**
 * @brief 老化Peer各Discovery Access并收集待清理业务Path。
 *
 * 活动Access超时后转为inactive；清理失败的inactive Access继续入选，
 * 直到对应Path注销成功，避免只在失效当轮尝试一次。
 * 调用方必须持有Discovery状态锁。
 */
static uint32_t _linkg_discovery_age_liveness_locked(linkg_discovery_peer_t *peer, uint64_t now_us)
{
    linkg_discovery_liveness_t *liveness;
    uint32_t                    expired_mask;
    uint32_t                    index;

    if (peer == NULL || !peer->online)
    {
        return 0U;
    }

    expired_mask = 0U;

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        liveness = &peer->liveness[index];

        if (!liveness->registered)
        {
            continue;
        }

        if (liveness->active)
        {
            if (now_us < liveness->last_seen_us ||
                now_us - liveness->last_seen_us < LINKG_DISCOVERY_LIVENESS_TIMEOUT_US)
            {
                continue;
            }

            LINKG_LOG_INFO("DISCOVERY: access expired, node=%u access=%s age_ms=%llu",
                           (unsigned int)peer->report.node.node_id, _linkg_discovery_access_name(index),
                           (unsigned long long)((now_us - liveness->last_seen_us) / 1000ULL));

            liveness->active = false;
        }

        /* inactive且仍registered表示Path清理尚未确认成功。 */
        expired_mask |= (1U << index);
    }

    return expired_mask;
}

/****************************** Access失效 ******************************/

/**
 * @brief 注销Peer已失效且尚未完成清理的业务Path。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_cleanup_expired_paths_locked(linkg_discovery_peer_t *peer, uint32_t expired_mask)
{
    linkg_link_access_t access;
    uint32_t            index;
    int                 first_error;
    int                 ret;

    if (peer == NULL)
    {
        return -EINVAL;
    }

    first_error = 0;

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if ((expired_mask & (1U << index)) == 0U)
        {
            continue;
        }

        ret = _linkg_discovery_index_access(index, &access);
        if (ret != 0)
        {
            if (first_error == 0)
            {
                first_error = ret;
            }

            continue;
        }

        ret = _linkg_discovery_unregister_access_path_locked(peer, access);
        if (ret == 0)
        {
            memset(&peer->liveness[index], 0, sizeof(peer->liveness[index]));
        }
        else if (first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/**
 * @brief 将指定Discovery Access从所有直接Peer中停用。
 *
 * 仅关闭指定Access的存活状态；Peer仍存在其他活动Access时继续在线，
 * 所有Access均失效后才注销整个直接Peer。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_deactivate_access_locked(linkg_link_access_t access, uint64_t now_us)
{
    linkg_discovery_peer_t *peer;
    uint32_t                access_index;
    uint32_t                peer_index;
    int                     first_error;
    int                     ret;

    ret = _linkg_discovery_access_index(access, &access_index);
    if (ret != 0)
    {
        return ret;
    }

    first_error = 0;

    for (peer_index = 0U; peer_index < LINKG_NODE_PEER_MAX; peer_index++)
    {
        peer = &g_discovery.peers[peer_index];

        if (!peer->used)
        {
            continue;
        }

        if (!peer->online)
        {
            memset(&peer->liveness[access_index], 0, sizeof(peer->liveness[access_index]));
            continue;
        }

        peer->liveness[access_index].active = false;

        if (_linkg_discovery_peer_has_active_liveness_locked(peer))
        {
            ret = _linkg_discovery_unregister_access_path_locked(peer, access);
            if (ret == 0)
            {
                memset(&peer->liveness[access_index], 0, sizeof(peer->liveness[access_index]));
            }
            else
            {
                /* 保留待清理标记，后续老化继续重试。 */
                peer->liveness[access_index].registered = true;
                if (first_error == 0)
                {
                    first_error = ret;
                }
            }

            continue;
        }

        ret = _linkg_discovery_unregister_peer_locked(peer, now_us);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }

        if (!peer->online)
        {
            _linkg_discovery_clear_peer_liveness_locked(peer);
        }
    }

    return first_error;
}

/****************************** Peer关系 ******************************/

/**
 * @brief 校验节点是否可以作为本机直接Peer。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_validate_peer_relation_locked(const linkg_discovery_report_t *report)
{
    if (report == NULL)
    {
        return -EINVAL;
    }

    if (report->node.node_id == g_discovery.local_report.node.node_id)
    {
        return -EINVAL;
    }

    if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_AP)
    {
        return report->node.role == LINKG_DEVICE_ROLE_STA ? 0 : -EINVAL;
    }

    if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_STA)
    {
        return report->node.role == LINKG_DEVICE_ROLE_AP ? 0 : -EINVAL;
    }

    return -EINVAL;
}

/****************************** Peer事件 ******************************/

/**
 * @brief 根据Peer当前Session和Revision分类完整Report事件。
 *
 * 优先过滤上一次已替代Session的迟到报文，不刷新其Access存活状态。
 */
static linkg_discovery_peer_event_t _linkg_discovery_classify_peer_report(const linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report)
{
    if (peer == NULL)
    {
        return LINKG_DISCOVERY_PEER_EVENT_NEW;
    }

    if (peer->previous_session_id != 0U && report->session_id == peer->previous_session_id)
    {
        return LINKG_DISCOVERY_PEER_EVENT_STALE;
    }

    if (report->session_id != peer->report.session_id)
    {
        return peer->online ? LINKG_DISCOVERY_PEER_EVENT_RESTART : LINKG_DISCOVERY_PEER_EVENT_NEW;
    }

    if (peer->session_closed || report->revision < peer->report.revision)
    {
        return LINKG_DISCOVERY_PEER_EVENT_STALE;
    }

    if (!peer->online)
    {
        return LINKG_DISCOVERY_PEER_EVENT_NEW;
    }

    return report->revision == peer->report.revision ?
           LINKG_DISCOVERY_PEER_EVENT_REFRESH :
           LINKG_DISCOVERY_PEER_EVENT_UPDATE;
}

/**
 * @brief 处理直接Peer新的Discovery Session。
 *
 * 清理旧Session关联的运行资源后记录旧Session ID并提交新Report。
 * 清理失败时保留旧Report且保持旧Session关闭，等待后续新Report重试；
 * 当前Access运行资源由事件处理层重新建立。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_restart_peer_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report)
{
    uint64_t old_session_id;
    int      ret;

    if (peer == NULL || report == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (peer->report.node.node_id != report->node.node_id)
    {
        return -EINVAL;
    }

    if (peer->report.session_id == report->session_id)
    {
        return -EINVAL;
    }

    old_session_id = peer->report.session_id;
    peer->session_closed = true;

    ret = _linkg_discovery_reset_peer_session_locked(peer);
    if (ret != 0)
    {
        return ret;
    }

    peer->previous_session_id = old_session_id;
    peer->report              = *report;
    peer->session_closed      = false;

    LINKG_LOG_INFO("DISCOVERY: peer session changed, node=%u old=%llu new=%llu",
                   (unsigned int)report->node.node_id,
                   (unsigned long long)old_session_id, (unsigned long long)report->session_id);

    return 0;
}

/****************************** Report处理 ******************************/

/**
 * @brief 处理通过指定Discovery Access收到的直接Peer完整状态。
 *
 * 仅实际收到当前Report的Access刷新存活时间。
 * STALE Report不刷新任何存活状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_handle_peer_report_locked(linkg_link_access_t access, const linkg_discovery_report_t *report, uint64_t now_us)
{
    linkg_discovery_peer_event_t event;
    linkg_discovery_peer_t      *peer;
    uint32_t                     access_index;
    uint64_t                     old_session_id;
    int                          ret;

    ret = _linkg_discovery_access_index(access, &access_index);
    if (ret != 0)
    {
        return ret;
    }

    if (!g_discovery.channels[access_index].registered)
    {
        return -ENODEV;
    }

    ret = _linkg_discovery_validate_report(report);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_validate_peer_relation_locked(report);
    if (ret != 0)
    {
        return ret;
    }

    peer  = _linkg_discovery_find_peer_locked(report->node.node_id);
    event = _linkg_discovery_classify_peer_report(peer, report);

    switch (event)
    {
        case LINKG_DISCOVERY_PEER_EVENT_NEW:
            if (peer == NULL)
            {
                peer = _linkg_discovery_acquire_peer_slot_locked(now_us);
                if (peer == NULL)
                {
                    return -ENOSPC;
                }
            }

            /* 注册函数会覆盖Report，需预先保存离线Tombstone的旧Session ID。 */
            old_session_id = peer->used ? peer->report.session_id : 0U;

            ret = _linkg_discovery_register_peer_locked(peer, access, report);
            if (ret != 0)
            {
                return ret;
            }

            if (old_session_id != 0U && old_session_id != report->session_id)
            {
                peer->previous_session_id = old_session_id;
            }

            peer->session_closed = false;
            _linkg_discovery_clear_peer_liveness_locked(peer);
            _linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);
            return 0;

        case LINKG_DISCOVERY_PEER_EVENT_REFRESH:
            ret = _linkg_discovery_refresh_peer_locked(peer, access, report);
            if (ret != 0)
            {
                return ret;
            }

            _linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

            return 0;

        case LINKG_DISCOVERY_PEER_EVENT_UPDATE:
            ret = _linkg_discovery_update_peer_locked(peer, access, report);
            if (ret != 0)
            {
                return ret;
            }

            _linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

            return 0;

        case LINKG_DISCOVERY_PEER_EVENT_RESTART:
            ret = _linkg_discovery_restart_peer_locked(peer, report);
            if (ret != 0)
            {
                return ret;
            }

            _linkg_discovery_clear_peer_liveness_locked(peer);
            _linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

            ret = _linkg_discovery_refresh_peer_locked(peer, access, report);
            if (ret == -EBUSY)
            {
                /* 本轮运行资源暂不可用，后续同版本Report可重试。 */
                return 0;
            }

            return ret;

        case LINKG_DISCOVERY_PEER_EVENT_STALE:
            return LINKG_DISCOVERY_REPORT_IGNORED;

        case LINKG_DISCOVERY_PEER_EVENT_INVALID:
        default:
            return -EINVAL;
    }
}

/****************************** LEAVE处理 ******************************/

/**
 * @brief 处理直接Peer主动离开状态。
 *
 * 同Session下revision小于当前状态的迟到LEAVE被忽略；
 * revision相等或更大的LEAVE均关闭当前Session。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_handle_peer_leave_locked(const linkg_discovery_leave_t *leave, uint64_t now_us)
{
    linkg_discovery_peer_t *peer;
    int                     ret;

    if (leave == NULL)
    {
        return -EINVAL;
    }

    if (leave->session_id == 0U || leave->revision == 0U)
    {
        return -EINVAL;
    }

    if (leave->node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        leave->node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    peer = _linkg_discovery_find_peer_locked(leave->node_id);
    if (peer == NULL)
    {
        return 0;
    }

    if (leave->session_id != peer->report.session_id)
    {
        return 0;
    }

    if (leave->revision < peer->report.revision)
    {
        return 0;
    }

    if (peer->session_closed)
    {
        return 0;
    }

    peer->session_closed = true;

    if (!peer->online)
    {
        return 0;
    }

    ret = _linkg_discovery_unregister_peer_locked(peer, now_us);

    if (!peer->online)
    {
        _linkg_discovery_clear_peer_liveness_locked(peer);
    }

    return ret;
}

/****************************** Peer老化 ******************************/

/**
 * @brief 老化直接Peer存活状态并回收过期Tombstone。
 *
 * 单个Access超时仅关闭对应存活状态并注销对应Path，失败后继续重试；
 * 全部Access均失效后Peer离线。
 * 离线Peer保留Tombstone用于过滤迟到旧状态，过期后再释放槽位。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_age_peers_locked(uint64_t now_us)
{
    linkg_discovery_peer_t *peer;
    uint32_t                expired_mask;
    uint32_t                index;
    int                     first_error;
    int                     ret;

    first_error = 0;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used)
        {
            continue;
        }

        if (peer->online)
        {
            expired_mask = _linkg_discovery_age_liveness_locked(peer, now_us);

            if (_linkg_discovery_peer_has_active_liveness_locked(peer))
            {
                if (expired_mask != 0U)
                {
                    ret = _linkg_discovery_cleanup_expired_paths_locked(peer, expired_mask);
                    if (ret != 0 && first_error == 0)
                    {
                        first_error = ret;
                    }
                }

                continue;
            }

            ret = _linkg_discovery_unregister_peer_locked(peer, now_us);
            if (ret != 0 && first_error == 0)
            {
                first_error = ret;
            }

            if (!peer->online)
            {
                _linkg_discovery_clear_peer_liveness_locked(peer);
            }

            continue;
        }

        if (peer->route_cleanup_pending)
        {
            ret = _linkg_discovery_cleanup_peer_route_locked(peer);
            if (ret != 0)
            {
                if (first_error == 0)
                {
                    first_error = ret;
                }

                continue;
            }
        }

        if (peer->offline_since_us == 0U || now_us < peer->offline_since_us)
        {
            continue;
        }

        if (now_us - peer->offline_since_us < LINKG_DISCOVERY_PEER_TOMBSTONE_TIMEOUT_US)
        {
            continue;
        }

        memset(peer, 0, sizeof(*peer));
    }

    return first_error;
}
