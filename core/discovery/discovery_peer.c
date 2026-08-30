/**
 * @file discovery_peer.c
 * @brief LinkG设备发现直接Peer状态实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "discovery_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
        if (peer->liveness[index].registered &&
            peer->liveness[index].active)
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
 * @brief 老化Peer各Discovery Access存活状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
static void _linkg_discovery_age_liveness_locked(linkg_discovery_peer_t *peer, uint64_t now_us)
{
    linkg_discovery_liveness_t *liveness;
    uint32_t                    index;

    if (peer == NULL || !peer->online)
    {
        return;
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        liveness = &peer->liveness[index];

        if (!liveness->registered || !liveness->active)
        {
            continue;
        }

        if (now_us < liveness->last_seen_us)
        {
            continue;
        }

        if (now_us - liveness->last_seen_us < LINKG_DISCOVERY_LIVENESS_TIMEOUT_US)
        {
            continue;
        }

        liveness->active = false;
    }
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
 * @brief 根据当前Peer状态分类完整Report事件。
 */
static linkg_discovery_peer_event_t _linkg_discovery_classify_peer_report(const linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report)
{
    if (peer == NULL)
    {
        return LINKG_DISCOVERY_PEER_EVENT_NEW;
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
 * @brief 处理直接Peer新Discovery会话。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_restart_peer_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report, uint64_t now_us)
{
    int ret;

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

    ret = _linkg_discovery_unregister_peer_locked(peer, now_us);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_register_peer_locked(peer, report);
    if (ret != 0)
    {
        return ret;
    }

    peer->session_closed = false;

    return 0;
}

/****************************** Access映射 ******************************/

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
        *index = 0U;
        return 0;
    }

    if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        *index = 1U;
        return 0;
    }

    return -EINVAL;
}

/****************************** Access失效 ******************************/

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

        memset(&peer->liveness[access_index], 0, sizeof(peer->liveness[access_index]));

        if (!peer->online)
        {
            continue;
        }

        if (_linkg_discovery_peer_has_active_liveness_locked(peer))
        {
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

            ret = _linkg_discovery_register_peer_locked(peer, report);
            if (ret != 0)
            {
                return ret;
            }

            peer->session_closed = false;

            _linkg_discovery_clear_peer_liveness_locked(peer);
            _linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

            return 0;

        case LINKG_DISCOVERY_PEER_EVENT_REFRESH:
			_linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

			return 0;

		case LINKG_DISCOVERY_PEER_EVENT_UPDATE:
			ret = _linkg_discovery_update_peer_locked(peer, report);
			if (ret != 0)
			{
				return ret;
			}

			_linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

			return 0;

		case LINKG_DISCOVERY_PEER_EVENT_RESTART:
			ret = _linkg_discovery_restart_peer_locked(peer, report, now_us);
			if (ret != 0)
			{
				return ret;
			}

			_linkg_discovery_clear_peer_liveness_locked(peer);
			_linkg_discovery_mark_liveness_seen_locked(peer, access_index, now_us);

			return 0;

		case LINKG_DISCOVERY_PEER_EVENT_STALE:
			return 0;

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
 * 单个Access超时仅关闭对应存活状态；全部Access均失效后Peer离线。
 * 离线Peer保留Tombstone用于过滤迟到旧状态，过期后再释放槽位。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_age_peers_locked(uint64_t now_us)
{
    linkg_discovery_peer_t *peer;
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
            _linkg_discovery_age_liveness_locked(peer, now_us);

            if (_linkg_discovery_peer_has_active_liveness_locked(peer))
            {
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
