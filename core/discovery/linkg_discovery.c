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

#include "linkg_time.h"

#include "discovery_internal.h"

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

    g_discovery.peer_count         = 0U;
    g_discovery.topology_revision  = 0U;

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Discovery模块。
 *
 * 初始化阶段仅建立Discovery本地软件状态，不创建Discovery Session。
 * 本机完整状态在每次start时构造一次。
 */
int linkg_discovery_init(void)
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
 * @brief 启动新的Discovery运行会话。
 *
 * 每次启动仅构造一次本机完整初始状态并创建新的Session ID；
 * 运行期间由Endpoint刷新接口原地维护该权威状态。
 */
int linkg_discovery_start(void)
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
 * @brief 停止当前Discovery运行会话。
 *
 * 停止前通过所有已注册Discovery Channel发送当前Session的主动离开状态，
 * 随后阻止新的状态处理并撤销全部Peer及Topology运行资源。
 */
int linkg_discovery_stop(void)
{
    linkg_discovery_leave_sender_t senders[LINKG_NODE_PATH_MAX];
    linkg_discovery_leave_t        leave;
    uint64_t                       now_us;
    uint32_t                       sender_count;
    uint32_t                       index;
    int                            cleanup_error;
    int                            first_error;
    int                            ret;
    bool                           leave_ready;
    bool                           was_running;

    if (!g_discovery.initialized)
    {
        return 0;
    }

    memset(senders, 0, sizeof(senders));
    memset(&leave, 0, sizeof(leave));

    sender_count = 0U;
    leave_ready  = false;
    was_running  = false;
    first_error  = 0;

    pthread_mutex_lock(&g_discovery.lock);

    if (g_discovery.running)
    {
        was_running = true;

        ret = _linkg_discovery_build_local_leave_locked(&leave);
        if (ret == 0)
        {
            leave_ready = true;
        }
        else
        {
            _linkg_discovery_record_first_error(&first_error, ret);
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

        /**
         * 从此刻开始拒绝新的Discovery状态处理。
         * 发送函数已经完成快照，因此无需继续持有Discovery状态锁。
         */
        g_discovery.running = false;

        memset(g_discovery.channels, 0, sizeof(g_discovery.channels));
    }

    pthread_mutex_unlock(&g_discovery.lock);

    if (was_running && leave_ready)
    {
        for (index = 0U; index < sender_count; index++)
        {
            ret = senders[index].send_leave(&leave, senders[index].user_data);
            if (ret != 0)
            {
                _linkg_discovery_record_first_error(&first_error, ret);
            }
        }
    }

    now_us = linkg_time_monotonic_us();

    pthread_mutex_lock(&g_discovery.lock);

    cleanup_error = _linkg_discovery_cleanup_runtime_locked(now_us);
    if (cleanup_error == 0)
    {
        memset(&g_discovery.local_report, 0, sizeof(g_discovery.local_report));
    }

    pthread_mutex_unlock(&g_discovery.lock);

    _linkg_discovery_record_first_error(&first_error, cleanup_error);

    return first_error;
}

/**
 * @brief 反初始化Discovery模块。
 *
 * 调用前Discovery必须已经停止；存在未完成的Peer或Route清理时
 * 将再次尝试清理，全部资源释放后才销毁Discovery状态锁。
 */
int linkg_discovery_deinit(void)
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
