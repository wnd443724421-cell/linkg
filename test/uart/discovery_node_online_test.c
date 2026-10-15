/**
 * @file discovery_node_online_test.c
 * @brief Discovery节点在线状态查询测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "discovery_internal.h"
#include "linkg_discovery.h"

/****************************** 测试辅助 ******************************/

/**
 * @brief 重置Discovery状态并保留有效互斥锁。
 */
static void _discovery_online_test_reset(linkg_device_role_t role, uint8_t node_id)
{
    memset(&g_discovery.local_report, 0, sizeof(g_discovery.local_report));
    memset(g_discovery.peers, 0, sizeof(g_discovery.peers));
    memset(&g_discovery.topology, 0, sizeof(g_discovery.topology));

    g_discovery.local_report.node.role    = role;
    g_discovery.local_report.node.node_id = node_id;
    g_discovery.initialized               = true;
    g_discovery.running                   = true;
}

/**
 * @brief 配置一条在线直接Peer状态。
 */
static void _discovery_online_test_peer(uint32_t index, linkg_device_role_t role, uint8_t node_id, uint64_t session_id)
{
    assert(index < LINKG_NODE_PEER_MAX);

    g_discovery.peers[index].used                = true;
    g_discovery.peers[index].online              = true;
    g_discovery.peers[index].report.node.role    = role;
    g_discovery.peers[index].report.node.node_id = node_id;
    g_discovery.peers[index].report.session_id   = session_id;
}

/****************************** 测试用例 ******************************/

/**
 * @brief 验证生命周期、参数、本机及直接Peer状态。
 */
static void _discovery_online_test_direct(void)
{
    bool online;

    _discovery_online_test_reset(LINKG_DEVICE_ROLE_AP, 1U);
    g_discovery.initialized = false;
    online = true;
    assert(linkg_discovery_node_online(2U, &online) == -ENODEV && !online);

    g_discovery.initialized = true;
    g_discovery.running     = false;
    online = true;
    assert(linkg_discovery_node_online(2U, &online) == 0 && !online);

    g_discovery.running = true;
    assert(linkg_discovery_node_online(0U, &online) == -EINVAL);
    assert(linkg_discovery_node_online(1U, NULL) == -EINVAL);
    assert(linkg_discovery_node_online(1U, &online) == 0 && online);

    _discovery_online_test_peer(0U, LINKG_DEVICE_ROLE_STA, 2U, 100U);
    assert(linkg_discovery_node_online(2U, &online) == 0 && online);

    g_discovery.peers[0].online = false;
    online = true;
    assert(linkg_discovery_node_online(2U, &online) == 0 && !online);
}

/**
 * @brief 验证STA仅信任当前在线AP Session对应的有效拓扑。
 */
static void _discovery_online_test_topology(void)
{
    bool online;

    _discovery_online_test_reset(LINKG_DEVICE_ROLE_STA, 1U);
    _discovery_online_test_peer(0U, LINKG_DEVICE_ROLE_AP, 2U, 200U);

    assert(linkg_discovery_node_online(2U, &online) == 0 && online);

    g_discovery.topology.ap_session_id = 200U;
    g_discovery.topology.revision      = 1U;
    g_discovery.topology.node_count    = 1U;
    g_discovery.topology.node_ids[0]   = 3U;
    assert(linkg_discovery_node_online(3U, &online) == 0 && online);

    g_discovery.peers[0].report.session_id = 201U;
    online = true;
    assert(linkg_discovery_node_online(3U, &online) == 0 && !online);

    g_discovery.peers[0].report.session_id = 200U;
    g_discovery.topology.revision          = 0U;
    online = true;
    assert(linkg_discovery_node_online(3U, &online) == 0 && !online);

    g_discovery.topology.revision = 1U;
    g_discovery.peers[0].online   = false;
    online = true;
    assert(linkg_discovery_node_online(3U, &online) == 0 && !online);
}

/**
 * @brief 执行Discovery节点在线状态查询测试。
 */
int main(void)
{
    memset(&g_discovery, 0, sizeof(g_discovery));
    assert(pthread_mutex_init(&g_discovery.lock, NULL) == 0);

    _discovery_online_test_direct();
    _discovery_online_test_topology();

    assert(pthread_mutex_destroy(&g_discovery.lock) == 0);
    puts("PASS Discovery direct peer and topology online state queries");
    return 0;
}
