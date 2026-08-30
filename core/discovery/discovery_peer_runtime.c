/**
 * @file discovery_peer_runtime.c
 * @brief LinkG设备发现直接Peer运行资源实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "discovery_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_route.h"
#include "linkg_switch.h"
#include "linkg_transport.h"

/****************************** Path辅助 ******************************/

/**
 * @brief 获取Report指定Access当前对应的本地业务Link ID。
 */
static uint32_t _linkg_discovery_report_link_id(const linkg_discovery_report_t *report, linkg_link_access_t access)
{
    if (report == NULL)
    {
        return LINKG_LINK_ID_INVALID;
    }

    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        if ((report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) == 0U)
        {
            return LINKG_LINK_ID_INVALID;
        }

        return linkg_link_manager_get_id(LINKG_LINK_ACCESS_WIFI);
    }

    if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        if ((report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U)
        {
            return LINKG_LINK_ID_INVALID;
        }

        return linkg_link_manager_get_id(LINKG_LINK_ACCESS_CELLULAR);
    }

    return LINKG_LINK_ID_INVALID;
}

/**
 * @brief 注册或更新Report当前全部可用数据Path。
 */
static int _linkg_discovery_register_report_paths(const linkg_discovery_report_t *report)
{
    uint32_t link_id;
    int      ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    link_id = _linkg_discovery_report_link_id(report, LINKG_LINK_ACCESS_WIFI);
    if (link_id != LINKG_LINK_ID_INVALID)
    {
        ret = linkg_node_register_path(report->node.node_id, link_id, &report->wifi_endpoint);
        if (ret != 0)
        {
            return ret;
        }
    }

    link_id = _linkg_discovery_report_link_id(report, LINKG_LINK_ACCESS_CELLULAR);
    if (link_id != LINKG_LINK_ID_INVALID)
    {
        ret = linkg_node_register_path(report->node.node_id, link_id, &report->cellular_endpoint);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 注销新Report中已经失效的旧数据Path。
 */
static int _linkg_discovery_unregister_obsolete_paths(const linkg_discovery_report_t *current, const linkg_discovery_report_t *next)
{
    uint32_t current_link_id;
    uint32_t next_link_id;
    int      first_error;
    int      ret;

    if (current == NULL || next == NULL)
    {
        return -EINVAL;
    }

    first_error = 0;

    current_link_id = _linkg_discovery_report_link_id(current, LINKG_LINK_ACCESS_WIFI);
    next_link_id    = _linkg_discovery_report_link_id(next, LINKG_LINK_ACCESS_WIFI);

    if (current_link_id != LINKG_LINK_ID_INVALID &&
        next_link_id == LINKG_LINK_ID_INVALID)
    {
        ret = linkg_node_unregister_path(current->node.node_id, current_link_id);
        if (ret != 0 && ret != -ENOENT)
        {
            first_error = ret;
        }
    }

    current_link_id = _linkg_discovery_report_link_id(current, LINKG_LINK_ACCESS_CELLULAR);
    next_link_id    = _linkg_discovery_report_link_id(next, LINKG_LINK_ACCESS_CELLULAR);

    if (current_link_id != LINKG_LINK_ID_INVALID &&
        next_link_id == LINKG_LINK_ID_INVALID)
    {
        ret = linkg_node_unregister_path(current->node.node_id, current_link_id);
        if (ret != 0 && ret != -ENOENT && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}

/****************************** Switch辅助 ******************************/

/**
 * @brief 根据Peer当前可用数据Path构造默认发送计划。
 *
 * Wi-Fi优先作为主链路，Cellular作为备用链路。
 * SINGLE模式保留secondary，使REDUNDANT调度策略可以同时使用主备链路。
 */
static void _linkg_discovery_build_send_plan(const linkg_discovery_report_t *report, linkg_send_plan_t *plan)
{
    uint32_t wifi_link_id;
    uint32_t cellular_link_id;

    memset(plan, 0, sizeof(*plan));

    plan->mode              = LINKG_SEND_MODE_NONE;
    plan->primary_link_id   = LINKG_LINK_ID_INVALID;
    plan->secondary_link_id = LINKG_LINK_ID_INVALID;

    if (report == NULL)
    {
        return;
    }

    wifi_link_id     = _linkg_discovery_report_link_id(report, LINKG_LINK_ACCESS_WIFI);
    cellular_link_id = _linkg_discovery_report_link_id(report, LINKG_LINK_ACCESS_CELLULAR);

    if (wifi_link_id != LINKG_LINK_ID_INVALID)
    {
        plan->mode            = LINKG_SEND_MODE_SINGLE;
        plan->primary_link_id = wifi_link_id;

        if (cellular_link_id != LINKG_LINK_ID_INVALID &&
            cellular_link_id != wifi_link_id)
        {
            plan->secondary_link_id = cellular_link_id;
        }

        return;
    }

    if (cellular_link_id != LINKG_LINK_ID_INVALID)
    {
        plan->mode            = LINKG_SEND_MODE_SINGLE;
        plan->primary_link_id = cellular_link_id;
    }
}

/****************************** 注册回滚 ******************************/

/**
 * @brief 回滚未完成的直接Peer运行资源注册。
 */
static void _linkg_discovery_rollback_peer_registration(uint8_t node_id, bool switch_registered, bool transport_registered, bool node_registered)
{
    if (switch_registered)
    {
        (void)linkg_switch_remove_plan(node_id);
    }

    if (transport_registered)
    {
        (void)linkg_transport_unregister_peer(node_id);
    }

    if (node_registered)
    {
        (void)linkg_node_unregister_peer(node_id);
    }
}

/****************************** Route清理 ******************************/

/**
 * @brief 重试清理离线Peer遗留的虚拟节点路由。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_cleanup_peer_route_locked(linkg_discovery_peer_t *peer)
{
    int ret;

    if (peer == NULL || !peer->used)
    {
        return -EINVAL;
    }

    if (peer->online)
    {
        return -EBUSY;
    }

    if (!peer->route_cleanup_pending)
    {
        return 0;
    }

    ret = linkg_route_remove_node(peer->report.node.node_id);
    if (ret != 0)
    {
        return ret;
    }

    peer->route_cleanup_pending = false;

    return 0;
}

/****************************** Peer注册 ******************************/

/**
 * @brief 建立直接Peer全部运行资源并提交在线状态。
 *
 * 外部运行资源全部建立成功后才提交Discovery在线状态。
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_register_peer_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report)
{
    linkg_send_plan_t plan;
    bool              node_registered;
    bool              transport_registered;
    bool              switch_registered;
    int               ret;

    if (peer == NULL || report == NULL)
    {
        return -EINVAL;
    }

    if (peer->online)
    {
        return -EALREADY;
    }

    if (peer->used && peer->report.node.node_id != report->node.node_id)
    {
        return -EINVAL;
    }

    if (g_discovery.peer_count >= LINKG_NODE_PEER_MAX)
    {
        return -ENOSPC;
    }

    if (peer->route_cleanup_pending)
    {
        ret = _linkg_discovery_cleanup_peer_route_locked(peer);
        if (ret != 0)
        {
            return ret;
        }
    }

    node_registered      = false;
    transport_registered = false;
    switch_registered    = false;

    ret = linkg_node_register_peer(&report->node);
    if (ret != 0)
    {
        return ret;
    }

    node_registered = true;

    ret = linkg_transport_register_peer(report->node.node_id);
    if (ret != 0)
    {
        _linkg_discovery_rollback_peer_registration(report->node.node_id, false, false, node_registered);
        return ret;
    }

    transport_registered = true;

    ret = _linkg_discovery_register_report_paths(report);
    if (ret != 0)
    {
        _linkg_discovery_rollback_peer_registration(report->node.node_id, false, transport_registered, node_registered);
        return ret;
    }

    _linkg_discovery_build_send_plan(report, &plan);

    ret = linkg_switch_set_plan(report->node.node_id, &plan);
    if (ret != 0)
    {
        _linkg_discovery_rollback_peer_registration(report->node.node_id, false, transport_registered, node_registered);
        return ret;
    }

    switch_registered = true;

    ret = linkg_route_add_node(report->node.node_id);
    if (ret != 0)
    {
        _linkg_discovery_rollback_peer_registration(report->node.node_id, switch_registered, transport_registered, node_registered);
        return ret;
    }

    peer->used                  = true;
    peer->online                = true;
    peer->route_cleanup_pending = false;
    peer->report                = *report;
    peer->offline_since_us      = 0U;

    g_discovery.peer_count++;

    if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_AP)
    {
        _linkg_discovery_advance_topology_revision_locked();
    }

    return 0;
}

/****************************** Peer更新 ******************************/

/**
 * @brief 同步在线直接Peer最新完整状态。
 *
 * 同Session更新仅同步Path与发送计划；
 * Discovery Session变化时额外重置Transport序列和接收窗口。
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_update_peer_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report)
{
    linkg_send_plan_t plan;
    bool              session_changed;
    int               ret;

    if (peer == NULL || report == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (peer->report.node.node_id != report->node.node_id)
    {
        return -EINVAL;
    }

    session_changed = peer->report.session_id != report->session_id;

    if (!session_changed &&
        report->revision <= peer->report.revision)
    {
        return -EINVAL;
    }

    if (session_changed)
    {
        ret = linkg_transport_reset_peer(report->node.node_id, false);
        if (ret != 0)
        {
            return ret;
        }
    }

    ret = _linkg_discovery_register_report_paths(report);
    if (ret != 0)
    {
        return ret;
    }

    _linkg_discovery_build_send_plan(report, &plan);

    ret = linkg_switch_set_plan(report->node.node_id, &plan);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_unregister_obsolete_paths(&peer->report, report);
    if (ret != 0)
    {
        return ret;
    }

    peer->report = *report;

    return 0;
}

/****************************** Peer注销 ******************************/

/**
 * @brief 注销直接Peer全部运行资源并进入离线Tombstone状态。
 *
 * 内存运行资源按Best-effort方式全部撤销；Route删除失败时保留
 * route_cleanup_pending，由Peer老化流程继续重试。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_unregister_peer_locked(linkg_discovery_peer_t *peer, uint64_t now_us)
{
    int first_error;
    int ret;

    if (peer == NULL || !peer->used)
    {
        return -EINVAL;
    }

    if (!peer->online)
    {
        return 0;
    }

    first_error = 0;

    ret = linkg_switch_remove_plan(peer->report.node.node_id);
    if (ret != 0 && ret != -ENOENT)
    {
        first_error = ret;
    }

    ret = linkg_transport_unregister_peer(peer->report.node.node_id);
    if (ret != 0 && ret != -ENOENT && first_error == 0)
    {
        first_error = ret;
    }

    ret = linkg_node_unregister_peer(peer->report.node.node_id);
    if (ret != 0 && ret != -ENOENT && first_error == 0)
    {
        first_error = ret;
    }

    ret = linkg_route_remove_node(peer->report.node.node_id);
    if (ret != 0)
    {
        peer->route_cleanup_pending = true;

        if (first_error == 0)
        {
            first_error = ret;
        }
    }
    else
    {
        peer->route_cleanup_pending = false;
    }

    peer->online           = false;
    peer->offline_since_us = now_us;

    if (g_discovery.peer_count > 0U)
    {
        g_discovery.peer_count--;
    }

    if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_AP)
    {
        _linkg_discovery_advance_topology_revision_locked();
    }
    else if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_STA)
    {
        ret = _linkg_discovery_clear_topology_locked();
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    return first_error;
}
