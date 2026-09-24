/**
 * @file linkg_discovery.c
 * @brief LinkG设备发现生命周期实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "linkg_discovery.h"

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_log.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

#include "discovery_cellular.h"
#include "discovery_internal.h"
#include "discovery_wifi.h"

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_discovery_channel_send_leave_func_t send_leave; // 主动离开状态发送函数
    void                                     *user_data;  // Channel私有数据
} linkg_discovery_leave_sender_t;

/****************************** 全局上下文 ******************************/

linkg_discovery_context_t g_discovery;

/****************************** 内部辅助 ******************************/

/**
 * @brief 记录清理阶段出现的首个错误。
 */
static void _linkg_discovery_record_first_error(int *first_error, int error)
{
    if (first_error == NULL)
    {
        return;
    }

    if (*first_error == 0 && error != 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 获取Discovery Channel配置使能状态。
 *
 * 调用前Discovery Core必须已经初始化。
 */
static void _linkg_discovery_get_channel_enabled(bool *wifi_enabled, bool *cellular_enabled)
{
    pthread_mutex_lock(&g_discovery.lock);

    if (wifi_enabled != NULL)
    {
        *wifi_enabled = g_discovery.wifi_channel_enabled;
    }

    if (cellular_enabled != NULL)
    {
        *cellular_enabled = g_discovery.cellular_channel_enabled;
    }

    pthread_mutex_unlock(&g_discovery.lock);
}

/**
 * @brief 判断Discovery运行资源是否已经完全清空。
 *
 * 调用方必须持有Discovery状态锁。
 */
static bool _linkg_discovery_runtime_empty_locked(void)
{
    uint32_t index;

    if (g_discovery.peer_count != 0U)
    {
        return false;
    }

    if (g_discovery.topology.node_count != 0U)
    {
        return false;
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        if (g_discovery.peers[index].used)
        {
            return false;
        }
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (g_discovery.channels[index].registered)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 清理当前Discovery Session全部Peer及Topology运行资源。
 *
 * 在线Peer统一进入注销流程；离线Peer存在遗留Route时继续重试清理。
 * 全部资源清理成功后释放当前Session所有运行状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
static int _linkg_discovery_cleanup_runtime_locked(uint64_t now_us)
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
            ret = _linkg_discovery_unregister_peer_locked(peer, now_us);
            if (ret != 0)
            {
                _linkg_discovery_record_first_error(&first_error, ret);
            }

            continue;
        }

        if (!peer->route_cleanup_pending)
        {
            continue;
        }

        ret = _linkg_discovery_cleanup_peer_route_locked(peer);
        if (ret != 0)
        {
            _linkg_discovery_record_first_error(&first_error, ret);
        }
    }

    if (g_discovery.local_report.node.role == LINKG_DEVICE_ROLE_STA &&
        g_discovery.topology.node_count != 0U)
    {
        ret = _linkg_discovery_clear_topology_locked();
        if (ret != 0)
        {
            _linkg_discovery_record_first_error(&first_error, ret);
        }
    }

    if (first_error != 0)
    {
        return first_error;
    }

    memset(g_discovery.peers, 0, sizeof(g_discovery.peers));
    memset(&g_discovery.topology, 0, sizeof(g_discovery.topology));

    g_discovery.peer_count        = 0U;
    g_discovery.topology_revision = 0U;

    return 0;
}

/****************************** Core生命周期 ******************************/

/**
 * @brief 初始化Discovery Core状态。
 */
static int _linkg_discovery_init_core(void)
{
    int ret;

    if (g_discovery.initialized)
    {
        return -EALREADY;
    }

    memset(&g_discovery, 0, sizeof(g_discovery));

    ret = pthread_mutex_init(&g_discovery.lock, NULL);
    if (ret != 0)
    {
        memset(&g_discovery, 0, sizeof(g_discovery));
        return -ret;
    }

    g_discovery.initialized = true;

    return 0;
}

/**
 * @brief 启动新的Discovery Core运行会话。
 *
 * 每次启动仅构造一次本机完整初始状态并创建新的Session ID；
 * Channel必须在Core进入running以后才能注册。
 */
static int _linkg_discovery_start_core(void)
{
    linkg_discovery_report_t local_report;
    int                      ret;

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -EALREADY;
    }

    if (!_linkg_discovery_runtime_empty_locked())
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -EBUSY;
    }

    memset(&local_report, 0, sizeof(local_report));

    ret = _linkg_discovery_build_local_report(&local_report);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return ret;
    }

    memset(g_discovery.peers, 0, sizeof(g_discovery.peers));
    memset(g_discovery.channels, 0, sizeof(g_discovery.channels));
    memset(&g_discovery.topology, 0, sizeof(g_discovery.topology));

    g_discovery.local_report      = local_report;
    g_discovery.peer_count        = 0U;
    g_discovery.topology_revision = 0U;

    if (local_report.node.role == LINKG_DEVICE_ROLE_AP)
    {
        g_discovery.topology_revision = 1U;
    }

    g_discovery.running = true;

    pthread_mutex_unlock(&g_discovery.lock);

    return 0;
}

/**
 * @brief 停止当前Discovery Core运行会话。
 *
 * 停止前通过所有已注册Channel发送当前Session主动离开状态。
 * 主动离开基于无确认数据报，只作为尽力通知，不阻断本地生命周期清理。
 * Channel在全部send_leave同步返回以前必须保持运行和注册状态。
 */
static int _linkg_discovery_stop_core(void)
{
    linkg_discovery_leave_sender_t senders[LINKG_NODE_PATH_MAX];
    linkg_discovery_leave_t        leave;
    uint64_t                       now_us;
    uint32_t                       sender_count;
    uint32_t                       index;
    int                            cleanup_error;
    int                            ret;
    bool                           leave_ready;

    if (!g_discovery.initialized)
    {
        return 0;
    }

    memset(senders, 0, sizeof(senders));
    memset(&leave, 0, sizeof(leave));

    sender_count = 0U;
    leave_ready  = false;

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);

        now_us = linkg_time_monotonic_us();

        pthread_mutex_lock(&g_discovery.lock);

        cleanup_error = _linkg_discovery_cleanup_runtime_locked(now_us);
        if (cleanup_error == 0)
        {
            memset(&g_discovery.local_report, 0, sizeof(g_discovery.local_report));
        }

        pthread_mutex_unlock(&g_discovery.lock);

        return cleanup_error;
    }

    ret = _linkg_discovery_build_local_leave_locked(&leave);
    if (ret == 0)
    {
        leave_ready = true;
    }
    else
    {
        LINKG_LOG_WARN("build local PEER_LEAVE failed during stop, error=%d", ret);
    }

    for (index = 0U; index < LINKG_NODE_PATH_MAX; index++)
    {
        if (!g_discovery.channels[index].registered ||
            g_discovery.channels[index].send_leave == NULL)
        {
            continue;
        }

        senders[sender_count].send_leave = g_discovery.channels[index].send_leave;
        senders[sender_count].user_data  = g_discovery.channels[index].user_data;
        sender_count++;
    }

    pthread_mutex_unlock(&g_discovery.lock);

    /**
     * Core仍保持running，冻结后的Channel保留Socket和注册状态。
     * send_leave可以继续使用本Session的本机状态及已知Peer目标。
     */
    if (leave_ready)
    {
        for (index = 0U; index < sender_count; index++)
        {
            ret = senders[index].send_leave(&leave, senders[index].user_data);
            if (ret != 0)
            {
                LINKG_LOG_WARN("send PEER_LEAVE failed during stop, error=%d", ret);
            }
        }
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_discovery.lock);

    g_discovery.running = false;

    memset(g_discovery.channels, 0, sizeof(g_discovery.channels));

    cleanup_error = _linkg_discovery_cleanup_runtime_locked(now_us);

    if (cleanup_error == 0)
    {
        memset(&g_discovery.local_report, 0, sizeof(g_discovery.local_report));
    }

    pthread_mutex_unlock(&g_discovery.lock);

    return cleanup_error;
}

/**
 * @brief 反初始化Discovery Core状态。
 *
 * 调用前Core必须已经停止且所有运行资源已经完成清理。
 */
static int _linkg_discovery_deinit_core(void)
{
    uint64_t now_us;
    int      ret;

    if (!g_discovery.initialized)
    {
        return 0;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -EBUSY;
    }

    now_us = linkg_time_monotonic_us();

    ret = _linkg_discovery_cleanup_runtime_locked(now_us);
    if (ret != 0)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return ret;
    }

    if (!_linkg_discovery_runtime_empty_locked())
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -EBUSY;
    }

    g_discovery.initialized = false;

    pthread_mutex_unlock(&g_discovery.lock);

    ret = pthread_mutex_destroy(&g_discovery.lock);
    if (ret != 0)
    {
        g_discovery.initialized = true;
        return -ret;
    }

    memset(&g_discovery, 0, sizeof(g_discovery));

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Discovery Core及配置启用的Channel。
 *
 * 不创建Discovery Session，不访问网络接口，也不要求Wi-Fi先于Cellular就绪。
 */
int linkg_discovery_init(void)
{
    linkg_links_config_t links;
    int                  cleanup_ret;
    int                  ret;

    memset(&links, 0, sizeof(links));

    ret = linkg_config_get_links(&links);
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_discovery_init_core();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery.lock);

    g_discovery.wifi_channel_enabled     = links.wifi.enabled;
    g_discovery.cellular_channel_enabled = links.cellular.enabled;

    pthread_mutex_unlock(&g_discovery.lock);

    if (links.wifi.enabled)
    {
        ret = linkg_discovery_wifi_init();
        if (ret != 0)
        {
            goto fail_core;
        }
    }

    if (links.cellular.enabled)
    {
        ret = linkg_discovery_cellular_init();
        if (ret != 0)
        {
            goto fail_wifi;
        }
    }

    return 0;

fail_wifi:
    if (links.wifi.enabled)
    {
        cleanup_ret = linkg_discovery_wifi_deinit();
        if (cleanup_ret != 0)
        {
            _linkg_discovery_record_first_error(&ret, cleanup_ret);
        }
    }

fail_core:
    cleanup_ret = _linkg_discovery_deinit_core();
    if (cleanup_ret != 0)
    {
        _linkg_discovery_record_first_error(&ret, cleanup_ret);
    }

    return ret;
}

/**
 * @brief 启动Discovery Core及配置启用的Channel Worker。
 *
 * Core先建立新Session，再启动各自的Worker；不等待wlan0、usb0或业务Link。
 * 接口、地址及Socket由各Channel Worker在运行期间独立检测并建立。
 */
int linkg_discovery_start(void)
{
    bool cellular_enabled;
    bool cellular_started;
    bool wifi_enabled;
    bool wifi_started;
    int  cleanup_ret;
    int  quiesce_error;
    int  ret;

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    _linkg_discovery_get_channel_enabled(&wifi_enabled, &cellular_enabled);

    if (!wifi_enabled && !cellular_enabled)
    {
        return -ENODEV;
    }

    ret = _linkg_discovery_start_core();
    if (ret != 0)
    {
        return ret;
    }

    wifi_started     = false;
    cellular_started = false;

    if (wifi_enabled)
    {
        ret = linkg_discovery_wifi_start();
        if (ret != 0)
        {
            goto fail;
        }

        wifi_started = true;
    }

    if (cellular_enabled)
    {
        ret = linkg_discovery_cellular_start();
        if (ret != 0)
        {
            goto fail;
        }

        cellular_started = true;
    }

    return 0;

fail:
    /**
     * 只冻结本轮实际启动成功的Channel。
     * 未完成start的Channel负责在自身start失败路径中回滚。
     *
     * 任一Channel不能确认已经完成quiesce时，Core必须继续保持running。
     * 否则工作线程仍可能推进revision，使随后构造的PEER_LEAVE变成迟到旧状态。
     */
    quiesce_error = 0;

    if (cellular_started)
    {
        cleanup_ret = linkg_discovery_cellular_quiesce();
        if (cleanup_ret != 0)
        {
            _linkg_discovery_record_first_error(&quiesce_error, cleanup_ret);
        }
    }

    if (wifi_started)
    {
        cleanup_ret = linkg_discovery_wifi_quiesce();
        if (cleanup_ret != 0)
        {
            _linkg_discovery_record_first_error(&quiesce_error, cleanup_ret);
        }
    }

    if (quiesce_error != 0)
    {
        return quiesce_error;
    }

    cleanup_ret = _linkg_discovery_stop_core();
    if (cleanup_ret != 0)
    {
        _linkg_discovery_record_first_error(&ret, cleanup_ret);
    }

    if (cellular_started)
    {
        cleanup_ret = linkg_discovery_cellular_stop();
        if (cleanup_ret != 0)
        {
            _linkg_discovery_record_first_error(&ret, cleanup_ret);
        }
    }

    if (wifi_started)
    {
        cleanup_ret = linkg_discovery_wifi_stop();
        if (cleanup_ret != 0)
        {
            _linkg_discovery_record_first_error(&ret, cleanup_ret);
        }
    }

    return ret;
}

/**
 * @brief 停止Discovery模块及内部Channel。
 *
 * 先冻结当前配置启用的Channel工作线程，保证Core主动LEAVE期间本机状态
 * 不再发生异步变化；随后停止Core Session，最后释放具体Channel运行资源。
 */
int linkg_discovery_stop(void)
{
    bool cellular_enabled;
    bool wifi_enabled;
    int  first_error;
    int  ret;

    if (!g_discovery.initialized)
    {
        return 0;
    }

    _linkg_discovery_get_channel_enabled(&wifi_enabled, &cellular_enabled);

    /**
     * quiesce失败时Core保持运行。
     * 不能在线程仍可能修改revision时继续构造PEER_LEAVE。
     */
    if (cellular_enabled)
    {
        ret = linkg_discovery_cellular_quiesce();
        if (ret != 0)
        {
            return ret;
        }
    }

    if (wifi_enabled)
    {
        ret = linkg_discovery_wifi_quiesce();
        if (ret != 0)
        {
            return ret;
        }
    }

    first_error = 0;

    ret = _linkg_discovery_stop_core();
    if (ret != 0)
    {
        _linkg_discovery_record_first_error(&first_error, ret);
    }

    if (cellular_enabled)
    {
        ret = linkg_discovery_cellular_stop();
        if (ret != 0)
        {
            _linkg_discovery_record_first_error(&first_error, ret);
        }
    }

    if (wifi_enabled)
    {
        ret = linkg_discovery_wifi_stop();
        if (ret != 0)
        {
            _linkg_discovery_record_first_error(&first_error, ret);
        }
    }

    return first_error;
}

/**
 * @brief 反初始化Discovery模块及内部Channel。
 *
 * 调用前Discovery必须已经停止；
 * 只释放links配置实际启用过的Channel，最后销毁Core状态。
 */
int linkg_discovery_deinit(void)
{
    bool     cellular_enabled;
    bool     wifi_enabled;
    uint64_t now_us;
    int      first_error;
    int      ret;

    if (!g_discovery.initialized)
    {
        return 0;
    }

    _linkg_discovery_get_channel_enabled(&wifi_enabled, &cellular_enabled);

    pthread_mutex_lock(&g_discovery.lock);

    if (g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -EBUSY;
    }

    now_us = linkg_time_monotonic_us();

    ret = _linkg_discovery_cleanup_runtime_locked(now_us);

    pthread_mutex_unlock(&g_discovery.lock);

    if (ret != 0)
    {
        return ret;
    }

    first_error = 0;

    if (cellular_enabled)
    {
        ret = linkg_discovery_cellular_deinit();
        if (ret != 0)
        {
            _linkg_discovery_record_first_error(&first_error, ret);
        }
    }

    if (wifi_enabled)
    {
        ret = linkg_discovery_wifi_deinit();
        if (ret != 0)
        {
            _linkg_discovery_record_first_error(&first_error, ret);
        }
    }

    if (first_error != 0)
    {
        return first_error;
    }

    return _linkg_discovery_deinit_core();
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取STA当前已知在线组网节点总数。
 *
 * 数量包含本机、当前在线AP以及AP同步的其他在线STA。
 */
int linkg_discovery_get_network_node_count(uint32_t *count)
{
    int unlock_ret;
    int ret;

    if (count == NULL)
    {
        return -EINVAL;
    }

    *count = 0U;

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_discovery.lock);
    if (ret != 0)
    {
        return -ret;
    }

    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_STA)
    {
        ret = -EPERM;
        goto out;
    }

    /**
     * 本机始终计入组网节点。
     * 未发现在线AP时，不使用可能仍在清理中的旧Topology。
     */
    *count = 1U;

    if (_linkg_discovery_has_online_ap_locked())
    {
        *count += 1U + g_discovery.topology.node_count;
    }

    ret = 0;

out:
    unlock_ret = pthread_mutex_unlock(&g_discovery.lock);
    if (ret == 0 && unlock_ret != 0)
    {
        ret = -unlock_ret;
    }

    return ret;
}

/**
 * @brief 获取STA当前应用的AP远端拓扑快照。
 */
int linkg_discovery_get_topology_snapshot(linkg_discovery_topology_snapshot_t *snapshot)
{
    int unlock_ret;
    int ret;

    if (snapshot == NULL)
    {
        return -EINVAL;
    }

    memset(snapshot, 0, sizeof(*snapshot));

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    ret = pthread_mutex_lock(&g_discovery.lock);
    if (ret != 0)
    {
        return -ret;
    }

    ret = 0;

    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_STA)
    {
        ret = -EPERM;
        goto out;
    }

    if (!g_discovery.running || !_linkg_discovery_has_online_ap_locked())
    {
        goto out;
    }

    snapshot->revision   = g_discovery.topology.revision;
    snapshot->node_count = g_discovery.topology.node_count;

    if (snapshot->node_count != 0U)
    {
        memcpy(snapshot->node_ids, g_discovery.topology.node_ids, snapshot->node_count * sizeof(snapshot->node_ids[0]));
    }

out:
    unlock_ret = pthread_mutex_unlock(&g_discovery.lock);
    if (ret == 0 && unlock_ret != 0)
    {
        ret = -unlock_ret;
    }

    return ret;
}

/****************************** 网络状态通知 ******************************/

/**
 * @brief 通知Wi-Fi Discovery检查网络状态。
 */
int linkg_discovery_notify_wifi_network_changed(void)
{
    return linkg_discovery_wifi_notify_network_changed();
}

/**
 * @brief 通知Cellular Discovery检查网络状态。
 */
int linkg_discovery_notify_cellular_network_changed(void)
{
    return linkg_discovery_cellular_notify_network_changed();
}
