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
#include "linkg_log.h"

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
 * @brief 注册或更新Report当前Access对应的业务Path。
 *
 * @return 1  当前Access对应业务Path有效并已完成注册或更新。
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

/**
 * @brief 注销新Report中已经明确失效的旧业务Path。
 *
 * 仅处理同一Peer前后完整Report中Path VALID从有效变为无效的变化。
 *
 * @return 1  至少一个旧业务Path已经失效并完成注销。
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

    if (peer->report.node.node_id != report->node.node_id ||
        peer->report.session_id != report->session_id)
    {
        return -EINVAL;
    }

    path_invalidated = false;
    first_error      = 0;

    if ((peer->report.path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) != 0U &&
        (report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) == 0U)
    {
        path_invalidated = true;

        LINKG_LOG_INFO("DISCOVERY: report invalidated access path, node=%u access=wifi",
                       (unsigned int)peer->report.node.node_id);

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

        LINKG_LOG_INFO("DISCOVERY: report invalidated access path, node=%u access=cellular",
                       (unsigned int)peer->report.node.node_id);

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

    if (plan.mode == LINKG_SEND_MODE_NONE ||
        plan.primary_link_id == LINKG_LINK_ID_INVALID)
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
 */
static void _linkg_discovery_rollback_peer_registration(uint8_t node_id, bool switch_registered, bool transport_registered, bool node_registered)
{
    int ret;

    LINKG_LOG_WARN("DISCOVERY: peer register rollback begin, node=%u switch=%u transport=%u node_runtime=%u",
                   (unsigned int)node_id,
                   switch_registered ? 1U : 0U,
                   transport_registered ? 1U : 0U,
                   node_registered ? 1U : 0U);

    if (switch_registered)
    {
        ret = linkg_switch_remove_plan(node_id);
        if (ret != 0 && ret != -ENOENT)
        {
            LINKG_LOG_WARN("DISCOVERY: peer register rollback failed, node=%u stage=switch error=%d",
                           (unsigned int)node_id,
                           ret);
        }
    }

    if (transport_registered)
    {
        ret = linkg_transport_unregister_peer(node_id);
        if (ret != 0 && ret != -ENOENT)
        {
            LINKG_LOG_WARN("DISCOVERY: peer register rollback failed, node=%u stage=transport error=%d",
                           (unsigned int)node_id,
                           ret);
        }
    }

    if (node_registered)
    {
        ret = linkg_node_unregister_peer(node_id);
        if (ret != 0 && ret != -ENOENT)
        {
            LINKG_LOG_WARN("DISCOVERY: peer register rollback failed, node=%u stage=node error=%d",
                           (unsigned int)node_id,
                           ret);
        }
    }

    LINKG_LOG_WARN("DISCOVERY: peer register rollback complete, node=%u", (unsigned int)node_id);
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

    LINKG_LOG_INFO("DISCOVERY: retry peer route cleanup, node=%u",
                   (unsigned int)peer->report.node.node_id);

    ret = linkg_route_remove_node(peer->report.node.node_id);
    if (ret != 0)
    {
        LINKG_LOG_WARN("DISCOVERY: peer route cleanup failed, node=%u error=%d",
                       (unsigned int)peer->report.node.node_id,
                       ret);
        return ret;
    }

    peer->route_cleanup_pending = false;

    LINKG_LOG_INFO("DISCOVERY: peer route cleanup complete, node=%u",
                   (unsigned int)peer->report.node.node_id);

    return 0;
}

/****************************** Peer注册 ******************************/

/**
 * @brief 建立直接Peer全部基础运行资源并提交在线状态。
 *
 * 当前Access存在有效业务Path时同步注册Path并建立初始发送计划；
 * 当前Access没有可用业务Path属于合法状态，Peer仍正常注册并保存完整Report。
 *
 * 外部运行资源全部建立成功后才提交Discovery在线状态。
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_register_peer_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access, const linkg_discovery_report_t *report)
{
    linkg_send_plan_t plan;
    bool              node_registered;
    bool              transport_registered;
    bool              switch_registered;
    int               path_result;
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

    LINKG_LOG_INFO("DISCOVERY: peer register begin, node=%u access=%s session=%llu revision=%llu flags=0x%02x",
                   (unsigned int)report->node.node_id,
                   _linkg_discovery_access_name(access),
                   (unsigned long long)report->session_id,
                   (unsigned long long)report->revision,
                   (unsigned int)report->path_flags);

    if (peer->route_cleanup_pending)
    {
        LINKG_LOG_INFO("DISCOVERY: peer register stage begin, node=%u stage=route-cleanup",
                       (unsigned int)report->node.node_id);

        ret = _linkg_discovery_cleanup_peer_route_locked(peer);

        LINKG_LOG_INFO("DISCOVERY: peer register stage end, node=%u stage=route-cleanup error=%d",
                       (unsigned int)report->node.node_id,
                       ret);

        if (ret != 0)
        {
            return ret;
        }
    }

    node_registered      = false;
    transport_registered = false;
    switch_registered    = false;

    LINKG_LOG_INFO("DISCOVERY: peer register stage begin, node=%u stage=node",
                   (unsigned int)report->node.node_id);

    ret = linkg_node_register_peer(&report->node);

    LINKG_LOG_INFO("DISCOVERY: peer register stage end, node=%u stage=node error=%d",
                   (unsigned int)report->node.node_id,
                   ret);

    if (ret != 0)
    {
        return ret;
    }

    node_registered = true;

    LINKG_LOG_INFO("DISCOVERY: peer register stage begin, node=%u stage=transport",
                   (unsigned int)report->node.node_id);

    ret = linkg_transport_register_peer(report->node.node_id);

    LINKG_LOG_INFO("DISCOVERY: peer register stage end, node=%u stage=transport error=%d",
                   (unsigned int)report->node.node_id,
                   ret);

    if (ret != 0)
    {
        _linkg_discovery_rollback_peer_registration(report->node.node_id, false, false, node_registered);
        return ret;
    }

    transport_registered = true;

    LINKG_LOG_INFO("DISCOVERY: peer register stage begin, node=%u stage=path access=%s",
                   (unsigned int)report->node.node_id,
                   _linkg_discovery_access_name(access));

    path_result = _linkg_discovery_register_access_path_locked(report, access);

    LINKG_LOG_INFO("DISCOVERY: peer register stage end, node=%u stage=path access=%s result=%d",
                   (unsigned int)report->node.node_id,
                   _linkg_discovery_access_name(access),
                   path_result);

    if (path_result < 0)
    {
        _linkg_discovery_rollback_peer_registration(report->node.node_id, false, transport_registered, node_registered);
        return path_result;
    }

    if (path_result > 0)
    {
        _linkg_discovery_build_default_send_plan(report, access, &plan);

        LINKG_LOG_INFO("DISCOVERY: peer register stage begin, node=%u stage=switch primary_link=%u",
                       (unsigned int)report->node.node_id,
                       plan.primary_link_id);

        ret = linkg_switch_set_plan(report->node.node_id, &plan);

        LINKG_LOG_INFO("DISCOVERY: peer register stage end, node=%u stage=switch error=%d",
                       (unsigned int)report->node.node_id,
                       ret);

        if (ret != 0)
        {
            _linkg_discovery_rollback_peer_registration(report->node.node_id, false, transport_registered, node_registered);
            return ret;
        }

        switch_registered = true;
    }

    LINKG_LOG_INFO("DISCOVERY: peer register stage begin, node=%u stage=route",
                   (unsigned int)report->node.node_id);

    ret = linkg_route_add_node(report->node.node_id);

    LINKG_LOG_INFO("DISCOVERY: peer register stage end, node=%u stage=route error=%d",
                   (unsigned int)report->node.node_id,
                   ret);

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

    LINKG_LOG_INFO("DISCOVERY: peer runtime registered, node=%u access=%s path=%u switch=%u peer_count=%u",
                   (unsigned int)report->node.node_id,
                   _linkg_discovery_access_name(access),
                   path_result > 0 ? 1U : 0U,
                   switch_registered ? 1U : 0U,
                   g_discovery.peer_count);

    return 0;
}

/****************************** Session重置 ******************************/

/**
 * @brief 清理直接Peer旧Discovery Session关联的运行资源。
 *
 * 保留Node Peer、Route和Discovery Peer槽位，仅撤销旧Session关联的
 * Send Plan、全部业务Path及Transport协议状态。
 *
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

    LINKG_LOG_INFO("DISCOVERY: peer session reset begin, node=%u session=%llu",
                   (unsigned int)node_id,
                   (unsigned long long)peer->report.session_id);

    /**
     * 先移除旧发送计划，禁止后续调度继续引用旧Session Path。
     */
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage begin, node=%u stage=switch", (unsigned int)node_id);
    ret = linkg_switch_remove_plan(node_id);
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage end, node=%u stage=switch error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && ret != -ENOENT)
    {
        first_error = ret;
    }

    /**
     * 退役旧Session Wi-Fi Path。
     */
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage begin, node=%u stage=wifi-path", (unsigned int)node_id);
    ret = _linkg_discovery_unregister_access_path_locked(peer, LINKG_LINK_ACCESS_WIFI);
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage end, node=%u stage=wifi-path error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    /**
     * 退役旧Session Cellular Path。
     */
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage begin, node=%u stage=cellular-path", (unsigned int)node_id);
    ret = _linkg_discovery_unregister_access_path_locked(peer, LINKG_LINK_ACCESS_CELLULAR);
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage end, node=%u stage=cellular-path error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    /**
     * 清理旧Session Transport序列、接收窗口及重组状态。
     */
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage begin, node=%u stage=transport", (unsigned int)node_id);
    ret = linkg_transport_reset_peer(node_id, false);
    LINKG_LOG_INFO("DISCOVERY: peer session reset stage end, node=%u stage=transport error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    LINKG_LOG_INFO("DISCOVERY: peer session reset complete, node=%u error=%d", (unsigned int)node_id, first_error);

    return first_error;
}

/****************************** Peer更新 ******************************/

/**
 * @brief 同步在线直接Peer同一Discovery Session下的最新完整状态。
 *
 * 新Report明确撤销已经存在的业务Path时立即注销对应旧Path；
 * 本次实际收到Report的Access仍有效时注册或更新对应业务Path。
 *
 * 当前Access首次形成有效业务Path且Peer尚无发送计划时建立Bootstrap计划；
 * 已存在发送计划时保持Switch模块维护的当前运行计划不变。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_update_peer_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access, const linkg_discovery_report_t *report)
{
    bool path_invalidated;
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

    path_invalidated = false;

    /**
     * 新Report可以明确撤销任意旧业务Path。
     */
    ret = _linkg_discovery_unregister_invalidated_paths_locked(peer, report);
    if (ret < 0)
    {
        return ret;
    }

    if (ret > 0)
    {
        path_invalidated = true;
    }

    /**
     * 仅本次实际收到Report的Access可以注册或更新对应业务Path。
     */
    ret = _linkg_discovery_register_access_path_locked(report, access);
    if (ret < 0)
    {
        return ret;
    }

    if (ret > 0)
    {
        ret = _linkg_discovery_ensure_bootstrap_send_plan_locked(report, access);
        if (ret != 0)
        {
            return ret;
        }
    }

    /**
     * Path运行资源同步完成后提交最新完整Peer Report。
     */
    peer->report = *report;

    /**
     * Switch实现后：
     * Path被明确撤销时只刷新一次发送计划，
     * 由Switch重新选择仍然有效的Primary/Secondary Path。
     */
    if (path_invalidated)
    {
        /*
        ret = linkg_switch_refresh_peer(peer->report.node.node_id);
        if (ret != 0)
        {
            return ret;
        }
        */
    }

    return 0;
}

/**
 * @brief 刷新在线直接Peer当前Access运行资源。
 *
 * 同版本Report不替换已保存的Peer完整状态，仅根据本次实际收到Report的
 * Access注册或更新对应业务Path；首次形成有效业务Path且当前尚无发送计划时
 * 建立Bootstrap发送计划。
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

    ret = _linkg_discovery_register_access_path_locked(report, access);
    if (ret < 0)
    {
        return ret;
    }

    if (ret > 0)
    {
        ret = _linkg_discovery_ensure_bootstrap_send_plan_locked(report, access);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/****************************** Path注销 ******************************/

/**
 * @brief 注销直接Peer指定Access对应的业务Path。
 *
 * Path不存在视为目标状态已经满足。
 * 调用方负责在需要时统一刷新Switch发送计划。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_unregister_access_path_locked(linkg_discovery_peer_t *peer, linkg_link_access_t access)
{
    uint32_t link_id;
    int      first_error;
    int      ret;

    if (peer == NULL || !peer->used || !peer->online)
    {
        return -EINVAL;
    }

    if (access != LINKG_LINK_ACCESS_WIFI &&
        access != LINKG_LINK_ACCESS_CELLULAR)
    {
        return -EINVAL;
    }

    first_error = 0;
    link_id     = linkg_link_manager_get_id(access);

    if (link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    LINKG_LOG_INFO("DISCOVERY: access path unregister begin, node=%u access=%s link=%u",
                   (unsigned int)peer->report.node.node_id,
                   _linkg_discovery_access_name(access),
                   link_id);

    ret = linkg_node_unregister_path(peer->report.node.node_id, link_id);

    LINKG_LOG_INFO("DISCOVERY: access path unregister end, node=%u access=%s link=%u error=%d",
                   (unsigned int)peer->report.node.node_id,
                   _linkg_discovery_access_name(access),
                   link_id,
                   ret);

    if (ret != 0 && ret != -ENOENT)
    {
        first_error = ret;
    }

    return first_error;
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

    LINKG_LOG_INFO("DISCOVERY: peer unregister begin, node=%u session=%llu revision=%llu",
                   (unsigned int)node_id,
                   (unsigned long long)peer->report.session_id,
                   (unsigned long long)peer->report.revision);

    LINKG_LOG_INFO("DISCOVERY: peer unregister stage begin, node=%u stage=switch", (unsigned int)node_id);
    ret = linkg_switch_remove_plan(node_id);
    LINKG_LOG_INFO("DISCOVERY: peer unregister stage end, node=%u stage=switch error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && ret != -ENOENT)
    {
        first_error = ret;
    }

    LINKG_LOG_INFO("DISCOVERY: peer unregister stage begin, node=%u stage=transport", (unsigned int)node_id);
    ret = linkg_transport_unregister_peer(node_id);
    LINKG_LOG_INFO("DISCOVERY: peer unregister stage end, node=%u stage=transport error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && ret != -ENOENT && first_error == 0)
    {
        first_error = ret;
    }

    LINKG_LOG_INFO("DISCOVERY: peer unregister stage begin, node=%u stage=node", (unsigned int)node_id);
    ret = linkg_node_unregister_peer(node_id);
    LINKG_LOG_INFO("DISCOVERY: peer unregister stage end, node=%u stage=node error=%d", (unsigned int)node_id, ret);

    if (ret != 0 && ret != -ENOENT && first_error == 0)
    {
        first_error = ret;
    }

    LINKG_LOG_INFO("DISCOVERY: peer unregister stage begin, node=%u stage=route", (unsigned int)node_id);
    ret = linkg_route_remove_node(node_id);
    LINKG_LOG_INFO("DISCOVERY: peer unregister stage end, node=%u stage=route error=%d", (unsigned int)node_id, ret);

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
        LINKG_LOG_INFO("DISCOVERY: peer unregister stage begin, node=%u stage=topology-clear", (unsigned int)node_id);
        ret = _linkg_discovery_clear_topology_locked();
        LINKG_LOG_INFO("DISCOVERY: peer unregister stage end, node=%u stage=topology-clear error=%d", (unsigned int)node_id, ret);

        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    LINKG_LOG_INFO("DISCOVERY: peer unregister complete, node=%u error=%d route_cleanup_pending=%u peer_count=%u",
                   (unsigned int)node_id,
                   first_error,
                   peer->route_cleanup_pending ? 1U : 0U,
                   g_discovery.peer_count);

    return first_error;
}

