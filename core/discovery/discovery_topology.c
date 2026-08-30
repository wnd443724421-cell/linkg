/**
 * @file discovery_topology.c
 * @brief LinkG设备发现网络拓扑实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "discovery_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_route.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断指定Node ID是否存在于节点列表中。
 */
static bool _linkg_discovery_topology_contains_node(const uint8_t *node_ids, uint32_t count, uint8_t node_id)
{
    uint32_t index;

    if (node_ids == NULL)
    {
        return false;
    }

    for (index = 0U; index < count; index++)
    {
        if (node_ids[index] == node_id)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断指定AP Discovery Session当前是否为在线直接Peer。
 *
 * 调用方必须持有Discovery状态锁。
 */
static bool _linkg_discovery_ap_session_online_locked(const linkg_discovery_report_t *report)
{
    const linkg_discovery_peer_t *peer;
    uint32_t                      index;

    if (report == NULL)
    {
        return false;
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used || !peer->online)
        {
            continue;
        }

        if (peer->report.node.role != LINKG_DEVICE_ROLE_AP)
        {
            continue;
        }

        if (peer->report.node.node_id != report->node.node_id)
        {
            continue;
        }

        if (peer->report.session_id != report->session_id)
        {
            continue;
        }

        return true;
    }

    return false;
}

/**
 * @brief 从AP同步状态构造本机实际需要维护的远端STA拓扑。
 *
 * AP_SYNC携带全部在线STA，本机STA自身不需要建立虚拟路由，
 * 因此内部Topology只保存除本机之外的远端STA节点。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_build_remote_topology_locked(const linkg_discovery_ap_sync_t *sync, uint8_t *node_ids, uint32_t *node_count)
{
    uint8_t  node_id;
    uint32_t count;
    uint32_t index;

    if (sync == NULL || node_ids == NULL || node_count == NULL)
    {
        return -EINVAL;
    }

    count = 0U;

    for (index = 0U; index < sync->node_count; index++)
    {
        node_id = sync->node_ids[index];

        if (node_id == g_discovery.local_report.node.node_id)
        {
            continue;
        }

        if (count >= LINKG_RESOURCE_NETWORK_STA_MAX)
        {
            return -EOVERFLOW;
        }

        node_ids[count++] = node_id;
    }

    *node_count = count;

    return 0;
}

/**
 * @brief 应用AP下发的完整远端STA拓扑快照。
 *
 * 先确保新拓扑全部节点路由存在，再删除旧拓扑中已经不存在的路由。
 * 仅全部Route操作成功后提交新的Topology状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_apply_ap_topology_locked(const linkg_discovery_ap_sync_t *sync)
{
    uint8_t  node_ids[LINKG_RESOURCE_NETWORK_STA_MAX];
    uint32_t node_count;
    uint32_t index;
    int      first_error;
    int      ret;

    if (sync == NULL)
    {
        return -EINVAL;
    }

    memset(node_ids, 0, sizeof(node_ids));

    ret = _linkg_discovery_build_remote_topology_locked(sync, node_ids, &node_count);
    if (ret != 0)
    {
        return ret;
    }

    /**
     * 新快照中的全部Route都执行幂等ADD。
     * 已存在的Route由Route模块使用CREATE|REPLACE直接保持目标状态。
     */
    first_error = 0;

    for (index = 0U; index < node_count; index++)
    {
        ret = linkg_route_add_node(node_ids[index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    /**
     * 新Route尚未全部建立时不删除旧Route，避免状态同步失败导致
     * 已经可达的远端节点提前失去路由。
     */
    if (first_error != 0)
    {
        return first_error;
    }

    /**
     * 删除旧Topology中已经不属于新快照的远端STA Route。
     * 删除操作幂等，因此部分成功后重试仍可继续收敛。
     */
    for (index = 0U; index < g_discovery.topology.node_count; index++)
    {
        if (_linkg_discovery_topology_contains_node(node_ids, node_count, g_discovery.topology.node_ids[index]))
        {
            continue;
        }

        ret = linkg_route_remove_node(g_discovery.topology.node_ids[index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (first_error != 0)
    {
        return first_error;
    }

    memset(&g_discovery.topology, 0, sizeof(g_discovery.topology));

    g_discovery.topology.ap_session_id = sync->ap.session_id;
    g_discovery.topology.revision      = sync->topology_revision;
    g_discovery.topology.node_count    = node_count;

    if (node_count != 0U)
    {
        memcpy(g_discovery.topology.node_ids, node_ids, node_count * sizeof(node_ids[0]));
    }

    return 0;
}

/****************************** AP拓扑版本 ******************************/

/**
 * @brief 推进AP本机在线STA拓扑版本。
 *
 * 版本0保留为未建立Topology状态，推进时自动跳过0。
 * 调用方必须持有Discovery状态锁。
 */
void _linkg_discovery_advance_topology_revision_locked(void)
{
    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_AP)
    {
        return;
    }

    g_discovery.topology_revision++;

    if (g_discovery.topology_revision == 0U)
    {
        g_discovery.topology_revision = 1U;
    }
}

/****************************** AP状态查询 ******************************/

/**
 * @brief 判断STA当前是否存在在线直接AP Peer。
 *
 * 调用方必须持有Discovery状态锁。
 */
bool _linkg_discovery_has_online_ap_locked(void)
{
    const linkg_discovery_peer_t *peer;
    uint32_t                      index;

    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_STA)
    {
        return false;
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used || !peer->online)
        {
            continue;
        }

        if (peer->report.node.role == LINKG_DEVICE_ROLE_AP)
        {
            return true;
        }
    }

    return false;
}

/****************************** STA拓扑清理 ******************************/

/**
 * @brief 清除STA当前应用的AP远端拓扑。
 *
 * 仅清理由AP_SYNC建立的非直接远端STA Route；
 * Direct AP自身Route由Peer Runtime负责注销。
 *
 * Route清理失败时保留节点列表用于后续重试，同时使当前
 * AP Session及Topology版本失效，避免相同版本AP_SYNC被错误跳过。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_clear_topology_locked(void)
{
    uint32_t index;
    int      first_error;
    int      ret;

    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EPERM;
    }

    /**
     * 从此刻开始当前AP拓扑不再有效。
     * node_ids暂时保留，以便Route删除失败时后续仍有清理目标。
     */
    g_discovery.topology.ap_session_id = 0U;
    g_discovery.topology.revision      = 0U;

    first_error = 0;

    for (index = 0U; index < g_discovery.topology.node_count; index++)
    {
        if (g_discovery.topology.node_ids[index] == g_discovery.local_report.node.node_id)
        {
            continue;
        }

        ret = linkg_route_remove_node(g_discovery.topology.node_ids[index]);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (first_error != 0)
    {
        return first_error;
    }

    memset(&g_discovery.topology, 0, sizeof(g_discovery.topology));

    return 0;
}

/****************************** AP同步构造 ******************************/

/**
 * @brief 构造AP当前完整Discovery及在线STA拓扑同步状态。
 *
 * node_ids包含当前全部在线直接STA，包括具体接收STA自身。
 * 所有STA接收相同完整拓扑快照。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_build_ap_sync_locked(linkg_discovery_ap_sync_t *sync)
{
    const linkg_discovery_peer_t *peer;
    uint32_t                      index;

    if (sync == NULL)
    {
        return -EINVAL;
    }

    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_AP)
    {
        return -EPERM;
    }

    /**
     * Topology版本0仅表示尚未建立过AP拓扑版本。
     * AP在尚无已注册STA时也可能周期广播AP_SYNC用于新STA发现，
     * 因此首个空Topology快照同样需要合法的非零版本。
     */
    if (g_discovery.topology_revision == 0U)
    {
        _linkg_discovery_advance_topology_revision_locked();
    }

    memset(sync, 0, sizeof(*sync));

    sync->ap                = g_discovery.local_report;
    sync->topology_revision = g_discovery.topology_revision;

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used || !peer->online)
        {
            continue;
        }

        if (peer->report.node.role != LINKG_DEVICE_ROLE_STA)
        {
            continue;
        }

        if (sync->node_count >= LINKG_RESOURCE_NETWORK_STA_MAX)
        {
            return -EOVERFLOW;
        }

        sync->node_ids[sync->node_count++] = peer->report.node.node_id;
    }

    return 0;
}

/****************************** AP同步处理 ******************************/

/**
 * @brief 处理STA通过指定Discovery Access收到的完整AP同步状态。
 *
 * AP自身Report首先进入Direct Peer状态机，仅实际接收当前AP_SYNC
 * 的Access刷新存活状态；Topology版本独立判断并应用远端STA Route。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_handle_ap_sync_locked(linkg_link_access_t access, const linkg_discovery_ap_sync_t *sync, uint64_t now_us)
{
    int ret;

    ret = _linkg_discovery_validate_ap_sync_locked(sync);
    if (ret != 0)
    {
        return ret;
    }

    /**
     * AP_SYNC同时承担AP Direct Peer完整Report及当前Access Liveness。
     * 即使Topology版本没有变化，AP Endpoint仍可能发生独立更新。
     */
    ret = _linkg_discovery_handle_peer_report_locked(access, &sync->ap, now_us);
    if (ret != 0)
    {
        return ret;
    }

    /**
     * Peer Report处理STALE时返回成功，因此再次确认当前在线Direct AP
     * 确实属于这个AP Session，避免已经关闭的旧Session继续下发Topology。
     */
    if (!_linkg_discovery_ap_session_online_locked(&sync->ap))
    {
        return 0;
    }

    /**
     * 同一个AP Discovery Session下Topology版本单调递增。
     * 旧版本和当前版本均不需要重复修改Route，但AP自身Liveness已经刷新。
     */
    if (g_discovery.topology.ap_session_id == sync->ap.session_id &&
        g_discovery.topology.revision != 0U &&
        sync->topology_revision <= g_discovery.topology.revision)
    {
        return 0;
    }

    /**
     * AP Session变化时Topology Revision重新建立版本空间，
     * 不与旧Session的Revision进行大小比较，直接应用完整新快照。
     */
    return _linkg_discovery_apply_ap_topology_locked(sync);
}
