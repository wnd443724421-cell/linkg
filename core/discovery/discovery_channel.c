/**
 * @file discovery_channel.c
 * @brief LinkG设备发现Channel接口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "discovery_channel.h"

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>

#include "discovery_internal.h"

/****************************** Channel管理 ******************************/

/**
 * @brief 注册指定Discovery Access通道。
 */
int linkg_discovery_channel_register(linkg_link_access_t access, linkg_discovery_channel_send_leave_func_t send_leave, void *user_data)
{
    linkg_discovery_channel_state_t *channel;
    uint32_t                         index;
    int                              ret;

    if (send_leave == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    ret = _linkg_discovery_access_index(access, &index);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    channel = &g_discovery.channels[index];

    if (channel->registered)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -EALREADY;
    }

    channel->registered = true;
    channel->send_leave = send_leave;
    channel->user_data  = user_data;

    pthread_mutex_unlock(&g_discovery.lock);

    return 0;
}

/**
 * @brief 注销指定Discovery Access通道。
 *
 * Channel注销后立即清除对应Access的全部Peer存活状态；
 * 仍存在其他活动Access的Peer继续保持在线。
 */
int linkg_discovery_channel_unregister(linkg_link_access_t access, uint64_t now_us)
{
    uint32_t index;
    int      ret;

    if (!g_discovery.initialized)
    {
        return 0;
    }

    ret = _linkg_discovery_access_index(access, &index);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.channels[index].registered)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return 0;
    }

    memset(&g_discovery.channels[index], 0, sizeof(g_discovery.channels[index]));

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return 0;
    }

    ret = _linkg_discovery_deactivate_access_locked(access, now_us);

    pthread_mutex_unlock(&g_discovery.lock);

    return ret;
}

/****************************** 本机状态 ******************************/

/**
 * @brief 获取本机当前完整Discovery状态。
 *
 * 获取前刷新Wi-Fi和Cellular数据Endpoint状态。
 */
int linkg_discovery_channel_get_local_report(linkg_discovery_report_t *report)
{
    int ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    ret = _linkg_discovery_refresh_local_endpoints();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    *report = g_discovery.local_report;

    pthread_mutex_unlock(&g_discovery.lock);

    return 0;
}

/**
 * @brief 构造AP当前完整Discovery及拓扑同步状态。
 *
 * 构造前刷新AP本机Wi-Fi和Cellular数据Endpoint状态。
 */
int linkg_discovery_channel_build_ap_sync(linkg_discovery_ap_sync_t *sync)
{
    int ret;

    if (sync == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    ret = _linkg_discovery_refresh_local_endpoints();
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    ret = _linkg_discovery_build_ap_sync_locked(sync);

    pthread_mutex_unlock(&g_discovery.lock);

    return ret;
}

/****************************** Peer状态 ******************************/

/**
 * @brief 提交通过指定Discovery Access收到的直接Peer完整状态。
 */
int linkg_discovery_channel_handle_peer_report(linkg_link_access_t access, const linkg_discovery_report_t *report, uint64_t now_us)
{
    int ret;

    if (report == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    ret = _linkg_discovery_handle_peer_report_locked(access, report, now_us);

    pthread_mutex_unlock(&g_discovery.lock);

    return ret;
}

/**
 * @brief 提交直接Peer主动离开状态。
 */
int linkg_discovery_channel_handle_peer_leave(const linkg_discovery_leave_t *leave, uint64_t now_us)
{
    int ret;

    if (leave == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    ret = _linkg_discovery_handle_peer_leave_locked(leave, now_us);

    pthread_mutex_unlock(&g_discovery.lock);

    return ret;
}

/**
 * @brief 执行直接Peer存活状态老化及Tombstone回收。
 */
int linkg_discovery_channel_age_peers(uint64_t now_us)
{
    int ret;

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    ret = _linkg_discovery_age_peers_locked(now_us);

    pthread_mutex_unlock(&g_discovery.lock);

    return ret;
}

/****************************** AP同步 ******************************/

/**
 * @brief 提交通过指定Discovery Access收到的完整AP同步状态。
 */
int linkg_discovery_channel_handle_ap_sync(linkg_link_access_t access, const linkg_discovery_ap_sync_t *sync, uint64_t now_us)
{
    int ret;

    if (sync == NULL)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    ret = _linkg_discovery_handle_ap_sync_locked(access, sync, now_us);

    pthread_mutex_unlock(&g_discovery.lock);

    return ret;
}

/****************************** Cellular目标 ******************************/

/**
 * @brief 获取当前在线直接Peer的有效Cellular数据Endpoint。
 */
int linkg_discovery_channel_get_cellular_targets(linkg_path_endpoint_t *targets, uint32_t capacity, uint32_t *count)
{
    const linkg_discovery_peer_t *peer;
    uint32_t                      index;
    uint32_t                      target_count;

    if (targets == NULL || count == NULL || capacity == 0U)
    {
        return -EINVAL;
    }

    if (!g_discovery.initialized)
    {
        return -ENODEV;
    }

    *count       = 0U;
    target_count = 0U;

    pthread_mutex_lock(&g_discovery.lock);

    if (!g_discovery.running)
    {
        pthread_mutex_unlock(&g_discovery.lock);
        return -ESHUTDOWN;
    }

    for (index = 0U; index < LINKG_NODE_PEER_MAX; index++)
    {
        peer = &g_discovery.peers[index];

        if (!peer->used || !peer->online)
        {
            continue;
        }

        if ((peer->report.path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) == 0U)
        {
            continue;
        }

        if (peer->report.cellular_endpoint.length == 0U)
        {
            continue;
        }

        if (target_count >= capacity)
        {
            pthread_mutex_unlock(&g_discovery.lock);
            return -ENOSPC;
        }

        targets[target_count++] = peer->report.cellular_endpoint;
    }

    pthread_mutex_unlock(&g_discovery.lock);

    *count = target_count;

    return 0;
}
