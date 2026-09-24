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
#include "linkg_log.h"
#include "linkg_node.h"
#include "linkg_route.h"
#include "linkg_switch.h"
#include "linkg_transport.h"

/****************************** Path辅助 ******************************/

/**
 * @brief 获取Discovery Access日志名称。
 */
static const char *_linkg_discovery_access_name(linkg_link_access_t access)
{
    switch (access)
    {
        case LINKG_LINK_ACCESS_WIFI:
            return "wifi";

        case LINKG_LINK_ACCESS_CELLULAR:
            return "cellular";

        default:
            return "unknown";
    }
}

/**
 * @brief 获取Report指定Access对应的本地业务Link ID。
 *
 * Report未声明当前Access的业务Path有效，或本地没有业务Link时返回无效ID。
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
 * @brief 注册或更新Report当前Access对应的业务Path。
 *
 * @return 1  当前Access对应业务Path有效，注册或重复确认成功。
 * @return 0  当前Access没有可注册业务Path。
 * @return <0 注册过程中发生错误。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_register_access_path_locked(const linkg_discovery_report_t *report, linkg_link_access_t access)
{
    const linkg_path_endpoint_t *endpoint;
    uint32_t                     link_id;
    int                          ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    if (access == LINKG_LINK_ACCESS_WIFI)
    {
        if ((report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) == 0U)
        {
            return 0;
        }

        endpoint = &report->wifi_endpoint;
    }
    else if (access == LINKG_LINK_ACCESS_CELLULAR)
    {
        if ((report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U)
        {
            return 0;
        }

        endpoint = &report->cellular_endpoint;
    }
    else
    {
        return -EINVAL;
    }

    link_id = linkg_link_manager_get_id(access);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    ret = linkg_node_register_path(report->node.node_id, link_id, endpoint);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY: access path register failed, node=%u access=%s link=%u error=%d",
                       (unsigned int)report->node.node_id,
                       _linkg_discovery_access_name(access),
                       link_id,
                       ret);
        return ret;
    }

    return 1;
}

/****************************** Path注销 ******************************/

/**
 * @brief 注销直接Peer指定Access对应的业务Path。
 *
 * Path不存在视为注销成功；发送计划由Switch负责维护。
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_unregister_access_path_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access)
{
    uint32_t link_id;
    int ret;

    if (peer == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (access != LINKG_LINK_ACCESS_WIFI && access != LINKG_LINK_ACCESS_CELLULAR)
    {
        return -EINVAL;
    }

    link_id = linkg_link_manager_get_id(access);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    ret = linkg_node_unregister_path(peer->report.node.node_id, link_id);
    if (ret != 0 && ret != -ENOENT)
    {
        LINKG_LOG_WARN("DISCOVERY: access path unregister failed, node=%u access=%s link=%u error=%d",
                       (unsigned int)peer->report.node.node_id, _linkg_discovery_access_name(access), link_id, ret);
        return ret;
    }

    return 0;
}


/**
 * @brief 注销新Report明确撤销的旧业务Path。
 *
 * 仅处理同一Peer、同一Session下Path VALID从有效变为无效的变化。
 * 两种Access独立清理，单个Path注销失败不阻止另一个Path清理。
 *
 * @return 1  至少一个旧Path声明被撤销，且注销成功或Path原本不存在。
 * @return 0  本次Report没有Path失效变化。
 * @return <0 Path注销过程中发生错误。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_unregister_invalidated_paths_locked(linkg_discovery_peer_t *peer, const linkg_discovery_report_t *report)
{
    bool path_invalidated;
    int  first_error;
    int  ret;

    if (peer == NULL || report == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (peer->report.node.node_id != report->node.node_id || peer->report.session_id != report->session_id)
    {
        return -EINVAL;
    }

    path_invalidated = false;
    first_error      = 0;

    if ((peer->report.path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) != 0U &&
        (report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) == 0U)
    {
        path_invalidated = true;

        ret = _linkg_discovery_unregister_access_path_locked(peer, LINKG_LINK_ACCESS_WIFI);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if ((peer->report.path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) != 0U &&
        (report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U)
    {
        path_invalidated = true;

        ret = _linkg_discovery_unregister_access_path_locked(peer, LINKG_LINK_ACCESS_CELLULAR);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (first_error != 0)
    {
        return first_error;
    }

    return path_invalidated ? 1 : 0;
}

/****************************** Switch辅助 ******************************/

/**
 * @brief 根据首次有效Discovery Access构造Bootstrap发送计划。
 */
static void _linkg_discovery_build_default_send_plan(const linkg_discovery_report_t *report, linkg_link_access_t access, linkg_send_plan_t *plan)
{
    uint32_t link_id;

    if (report == NULL || plan == NULL)
    {
        return;
    }

    memset(plan, 0, sizeof(*plan));

    plan->mode              = LINKG_SEND_MODE_NONE;
    plan->primary_link_id   = LINKG_LINK_ID_INVALID;
    plan->secondary_link_id = LINKG_LINK_ID_INVALID;

    link_id = _linkg_discovery_report_link_id(report, access);
    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return;
    }

    plan->mode            = LINKG_SEND_MODE_SINGLE;
    plan->primary_link_id = link_id;
}

/**
 * @brief 确保首次有效业务Path已经建立Bootstrap发送计划。
 *
 * 当前Peer已经存在发送计划时保持现有计划不变；
 * 当前Peer尚无发送计划时，根据本次确认有效的Access建立初始计划。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_ensure_bootstrap_send_plan_locked(const linkg_discovery_report_t *report, linkg_link_access_t access)
{
    linkg_send_plan_t plan;
    int               ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_switch_get_plan(report->node.node_id, &plan);
    if (ret == 0)
    {
        return 0;
    }

    if (ret != -ENOENT)
    {
        return ret;
    }

    _linkg_discovery_build_default_send_plan(report, access, &plan);

    if (plan.mode == LINKG_SEND_MODE_NONE || plan.primary_link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    ret = linkg_switch_set_plan(report->node.node_id, &plan);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY: bootstrap send plan failed, node=%u access=%s primary_link=%u error=%d",
                       (unsigned int)report->node.node_id,
                       _linkg_discovery_access_name(access),
                       plan.primary_link_id,
                       ret);
        return ret;
    }

    LINKG_LOG_INFO("DISCOVERY: bootstrap send plan created, node=%u access=%s primary_link=%u",
                   (unsigned int)report->node.node_id,
                   _linkg_discovery_access_name(access),
                   plan.primary_link_id);

    return 0;
}

/****************************** 注册回滚 ******************************/

/**
 * @brief 回滚未完成的直接Peer运行资源注册。
 *
 * 按Switch、Transport、Node顺序撤销已成功建立的资源。
 * Node注销负责退役该Peer全部业务Path；回滚失败不阻止后续清理。
 */
static void _linkg_discovery_rollback_peer_registration(uint8_t node_id, bool switch_registered, bool transport_registered, bool node_registered)
{
    int ret;

    if (switch_registered)
    {
        ret = linkg_switch_remove_plan(node_id);
        if (ret != 0 && ret != -ENOENT)
        {
            LINKG_LOG_WARN("DISCOVERY: peer rollback failed, node=%u stage=switch error=%d", (unsigned int)node_id, ret);
        }
    }

    if (transport_registered)
    {
        ret = linkg_transport_unregister_peer(node_id);
        if (ret != 0 && ret != -ENOENT)
        {
            LINKG_LOG_WARN("DISCOVERY: peer rollback failed, node=%u stage=transport error=%d", (unsigned int)node_id, ret);
        }
    }

    if (node_registered)
    {
        ret = linkg_node_unregister_peer(node_id);
        if (ret != 0 && ret != -ENOENT)
        {
            LINKG_LOG_WARN("DISCOVERY: peer rollback failed, node=%u stage=node error=%d", (unsigned int)node_id, ret);
        }
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
        LINKG_LOG_WARN("DISCOVERY: peer route cleanup failed, node=%u error=%d",
                       (unsigned int)peer->report.node.node_id, ret);
        return ret;
    }

    peer->route_cleanup_pending = false;
    return 0;
}

/****************************** Peer注册 ******************************/

/**
 * @brief 注册首次上线的直接Peer及其基础运行资源。
 *
 * 仅注册本次Discovery Access对应的业务Path；无可用数据Path时仍允许Peer上线。
 * 外部运行资源建立完成后，才提交完整Peer Report并更新在线拓扑。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_register_peer_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access, const linkg_discovery_report_t *report)
{
    linkg_send_plan_t plan;
    const char *stage;
    bool transport_registered;
    bool switch_registered;
    int path_result;
    int ret;

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

    transport_registered = false;
    switch_registered    = false;

    ret = linkg_node_register_peer(&report->node);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY: peer register failed, node=%u stage=node error=%d",
                       (unsigned int)report->node.node_id, ret);
        return ret;
    }

    ret = linkg_transport_register_peer(report->node.node_id);
    if (ret != 0)
    {
        stage = "transport";
        goto rollback;
    }

    transport_registered = true;

    path_result = _linkg_discovery_register_access_path_locked(report, access);
    if (path_result < 0)
    {
        ret = path_result;
        stage = "path";
        goto rollback;
    }

    if (path_result > 0)
    {
        _linkg_discovery_build_default_send_plan(report, access, &plan);

        ret = linkg_switch_set_plan(report->node.node_id, &plan);
        if (ret != 0)
        {
            stage = "switch";
            goto rollback;
        }

        switch_registered = true;
    }

    ret = linkg_route_add_node(report->node.node_id);
    if (ret != 0)
    {
        stage = "route";
        goto rollback;
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

    LINKG_LOG_INFO("DISCOVERY: peer online, node=%u access=%s session=%llu revision=%llu path=%u peers=%u",
                   (unsigned int)report->node.node_id,
                   _linkg_discovery_access_name(access),
                   (unsigned long long)report->session_id,
                   (unsigned long long)report->revision,
                   path_result > 0 ? 1U : 0U,
                   g_discovery.peer_count);

    return 0;

rollback:
    LINKG_LOG_WARN("DISCOVERY: peer register failed, node=%u stage=%s error=%d",
                   (unsigned int)report->node.node_id, stage, ret);

    _linkg_discovery_rollback_peer_registration(report->node.node_id, switch_registered, transport_registered, true);

    return ret;
}

/****************************** Session重置 ******************************/

/**
 * @brief 清理直接Peer旧Discovery Session的运行资源。
 *
 * 保留Node Peer、Route和Discovery Peer槽位，撤销旧Session的
 * Send Plan、全部业务Path及Transport协议状态。
 * 各阶段尽力清理，返回第一个错误；不在此处提交新Session。
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_reset_peer_session_locked(linkg_discovery_peer_t *peer)
{
    uint8_t node_id;
    int     first_error;
    int     ret;

    if (peer == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    node_id     = peer->report.node.node_id;
    first_error = 0;

    ret = linkg_switch_remove_plan(node_id);
    if (ret != 0 && ret != -ENOENT)
    {
        LINKG_LOG_WARN("DISCOVERY: peer session reset failed, node=%u stage=switch error=%d", (unsigned int)node_id, ret);
        first_error = ret;
    }

    ret = _linkg_discovery_unregister_access_path_locked(peer, LINKG_LINK_ACCESS_WIFI);
    if (ret != 0)
    {
        /* Path辅助函数已记录实际注销错误。 */
        if (first_error == 0)
        {
            first_error = ret;
        }
    }

    ret = _linkg_discovery_unregister_access_path_locked(peer, LINKG_LINK_ACCESS_CELLULAR);
    if (ret != 0)
    {
        /* Path辅助函数已记录实际注销错误。 */
        if (first_error == 0)
        {
            first_error = ret;
        }
    }

    ret = linkg_transport_reset_peer(node_id, false);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY: peer session reset failed, node=%u stage=transport error=%d", (unsigned int)node_id, ret);

        if (first_error == 0)
        {
            first_error = ret;
        }
    }

    if (first_error == 0)
    {
        LINKG_LOG_INFO("DISCOVERY: peer session reset complete, node=%u session=%llu",
                       (unsigned int)node_id, (unsigned long long)peer->report.session_id);
    }

    return first_error;
}

/****************************** Peer更新 ******************************/

/**
 * @brief 更新在线直接Peer同一Discovery Session下的最新完整状态。
 *
 * 撤销新Report明确失效的旧业务Path，仅注册或更新本次接收Access的业务Path。
 * 当前Peer没有发送计划时，为首次有效业务Path建立Bootstrap计划。
 * 运行资源更新成功后提交完整Peer Report。
 * 预留STA业务Path变化通知；实际判定和PLAN_SYNC由Switch负责。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_update_peer_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access, const linkg_discovery_report_t *report)
{
    bool path_changed;
    int  ret;

    if (peer == NULL || report == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (peer->report.node.node_id != report->node.node_id ||
        peer->report.session_id != report->session_id ||
        report->revision <= peer->report.revision)
    {
        return -EINVAL;
    }

    path_changed = false;

    /* 撤销新Report明确失效的旧业务Path。 */
    ret = _linkg_discovery_unregister_invalidated_paths_locked(peer, report);
    if (ret < 0)
    {
        return ret;
    }

    if (ret > 0)
    {
        path_changed = true;
    }

    /* 仅注册或更新本次实际接收Access对应的业务Path。 */
    ret = _linkg_discovery_register_access_path_locked(report, access);
    if (ret < 0)
    {
        return ret;
    }

    if (ret > 0)
    {
        path_changed = true;

        ret = _linkg_discovery_ensure_bootstrap_send_plan_locked(report, access);
        if (ret != 0)
        {
            return ret;
        }
    }

    /* 保存完整Report，包括尚未通过对应Access确认的Endpoint。 */
    peer->report = *report;

    /* 仅STA预留Path变化通知；接口实现后由Switch Worker重新观测与判定。 */
    if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_STA && path_changed)
    {
        /* TODO: 区分实际Path变化与重复更新，再接入Switch Worker事件通知。 */
        /* linkg_switch_notify_path_changed(report->node.node_id); */
    }

    return 0;
}

/**
 * @brief 刷新在线直接Peer当前Access运行资源。
 *
 * 同版本Report不替换已保存的完整状态，仅根据本次实际收到Report的
 * Access注册或更新对应业务Path；尚无发送计划时建立Bootstrap计划。
 * 同版本刷新不主动触发Switch，周期观测负责后续计划调整。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_refresh_peer_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access, const linkg_discovery_report_t *report)
{
    int ret;

    if (peer == NULL || report == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (peer->report.node.node_id != report->node.node_id ||
        peer->report.session_id != report->session_id ||
        peer->report.revision != report->revision)
    {
        return -EINVAL;
    }

    /* 仅确认本次实际收到Report的Access，使用Core已保存的完整状态。 */
    ret = _linkg_discovery_register_access_path_locked(&peer->report, access);
    if (ret < 0)
    {
        return ret;
    }

    if (ret > 0)
    {
        ret = _linkg_discovery_ensure_bootstrap_send_plan_locked(&peer->report, access);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** Peer注销 ******************************/

/**
 * @brief 注销直接Peer运行资源并进入离线Tombstone状态。
 *
 * 按Best-effort方式撤销Switch、Transport、Node（包括其Path）和Route。
 * Route删除失败时保留route_cleanup_pending，供Peer老化流程重试。
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_unregister_peer_locked(linkg_discovery_peer_t *peer, uint64_t now_us)
{
    uint8_t node_id;
    int     first_error;
    int     ret;

    if (peer == NULL || !peer->used)
    {
        return -EINVAL;
    }

    if (!peer->online)
    {
        return 0;
    }

    node_id     = peer->report.node.node_id;
    first_error = 0;

    ret = linkg_switch_remove_plan(node_id);
    if (ret != 0 && ret != -ENOENT)
    {
        LINKG_LOG_WARN("DISCOVERY: peer unregister failed, node=%u stage=switch error=%d", (unsigned int)node_id, ret);
        first_error = ret;
    }

    ret = linkg_transport_unregister_peer(node_id);
    if (ret != 0 && ret != -ENOENT)
    {
        LINKG_LOG_WARN("DISCOVERY: peer unregister failed, node=%u stage=transport error=%d", (unsigned int)node_id, ret);

        if (first_error == 0)
        {
            first_error = ret;
        }
    }

    ret = linkg_node_unregister_peer(node_id);
    if (ret != 0 && ret != -ENOENT)
    {
        LINKG_LOG_WARN("DISCOVERY: peer unregister failed, node=%u stage=node error=%d", (unsigned int)node_id, ret);

        if (first_error == 0)
        {
            first_error = ret;
        }
    }

    ret = linkg_route_remove_node(node_id);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY: peer unregister failed, node=%u stage=route error=%d", (unsigned int)node_id, ret);

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
        if (ret != 0)
        {
            LINKG_LOG_WARN("DISCOVERY: peer unregister failed, node=%u stage=topology error=%d", (unsigned int)node_id, ret);

            if (first_error == 0)
            {
                first_error = ret;
            }
        }
    }

    LINKG_LOG_INFO("DISCOVERY: peer offline, node=%u session=%llu", (unsigned int)node_id, (unsigned long long)peer->report.session_id);

    return first_error;
}
