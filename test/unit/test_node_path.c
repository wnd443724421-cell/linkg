/**
 * @file test_node_path.c
 * @brief Node、Peer和Path资源生命周期逻辑测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_link_manager.h"
#include "linkg_node.h"
#include "linkg_path.h"

static uint32_t g_purge_calls;
static uint32_t g_purged_paths;

int linkg_link_manager_purge_tx_path(linkg_path_t *path, uint32_t *purged_count)
{
    if (path == NULL)
    {
        return -EINVAL;
    }

    g_purge_calls++;
    g_purged_paths++;
    if (purged_count != NULL)
    {
        *purged_count = 0U;
    }

    return 0;
}

static linkg_path_endpoint_t ipv4_endpoint(const char *address, uint16_t port)
{
    linkg_path_endpoint_t endpoint;
    struct sockaddr_in   *socket_address;

    memset(&endpoint, 0, sizeof(endpoint));
    socket_address = (struct sockaddr_in *)&endpoint.address;
    socket_address->sin_family = AF_INET;
    socket_address->sin_port   = htons(port);
    (void)inet_pton(AF_INET, address, &socket_address->sin_addr);
    endpoint.length = sizeof(*socket_address);

    return endpoint;
}

static void path_released(linkg_path_t *path, void *user_data)
{
    uint32_t *calls;

    calls = (uint32_t *)user_data;
    if (path != NULL && calls != NULL)
    {
        (*calls)++;
    }
}

static void test_path_lifecycle_and_statistics(void)
{
    linkg_path_endpoint_t endpoint;
    linkg_path_stats_t    stats;
    linkg_path_t          path;
    uint32_t              released_calls;
    bool                  released;

    memset(&path, 0, sizeof(path));
    released_calls = 0U;
    endpoint = ipv4_endpoint("192.0.2.10", 4000U);

    TEST_CHECK_EQ(linkg_path_init(NULL), -EINVAL);
    TEST_CHECK_EQ(linkg_path_init(&path), 0);
    TEST_CHECK_EQ(linkg_path_init(&path), -EALREADY);
    TEST_CHECK_EQ(linkg_path_activate(&path, LINKG_LINK_ID_INVALID, &endpoint, NULL, NULL), -EINVAL);
    TEST_CHECK_EQ(linkg_path_activate(&path, 7U, &endpoint, path_released, &released_calls), 0);
    TEST_CHECK(linkg_path_is_active(&path));
    TEST_CHECK_EQ(linkg_path_deinit(&path), -EBUSY);

    linkg_path_record_tx_success_class(&path, 1U, 1000U, 4U);
    linkg_path_record_tx_failed(&path, 200U, 1U);
    linkg_path_record_tx_queue_overflow_class(&path, 2U, 300U, 2U);
    linkg_path_record_tx_expired(&path, 400U, 3U);
    linkg_path_record_rx(&path, 500U, 5U);
    TEST_CHECK_EQ(linkg_path_get_stats(&path, &stats), 0);
    TEST_CHECK_EQ(stats.tx_bytes, 1000U);
    TEST_CHECK_EQ(stats.tx_packets, 4U);
    TEST_CHECK_EQ(stats.tx_failed_bytes, 200U);
    TEST_CHECK_EQ(stats.tx_dropped_bytes, 300U);
    TEST_CHECK_EQ(stats.tx_queue_overflow_packets, 2U);
    TEST_CHECK_EQ(stats.tx_expired_packets, 3U);
    TEST_CHECK_EQ(stats.rx_bytes, 500U);
    TEST_CHECK_EQ(stats.tx_classes[1].tx_packets, 4U);
    TEST_CHECK_EQ(stats.tx_classes[2].tx_queue_overflow_packets, 2U);

    TEST_CHECK_EQ(linkg_path_acquire_batch(&path, 2U), 0);
    TEST_CHECK_EQ(linkg_path_get_reference_count(&path), 2U);
    TEST_CHECK_EQ(linkg_path_retire(&path, &released), 0);
    TEST_CHECK(!released);
    TEST_CHECK_EQ(linkg_path_get_state(&path), LINKG_PATH_STATE_RETIRED);
    TEST_CHECK_EQ(linkg_path_acquire(&path), -ENODEV);
    linkg_path_release(&path);
    TEST_CHECK_EQ(released_calls, 0U);
    linkg_path_release(&path);
    TEST_CHECK_EQ(released_calls, 1U);
    TEST_CHECK_EQ(linkg_path_get_state(&path), LINKG_PATH_STATE_RELEASED);
    linkg_path_release(&path);
    TEST_CHECK_EQ(released_calls, 1U);

    TEST_CHECK_EQ(linkg_path_reset(&path), 0);
    TEST_CHECK_EQ(linkg_path_get_state(&path), LINKG_PATH_STATE_EMPTY);
    TEST_CHECK_EQ(linkg_path_deinit(&path), 0);
    TEST_CHECK_EQ(linkg_path_deinit(&path), 0);
}

static void test_node_peer_and_deferred_release(void)
{
    linkg_node_info_t          local;
    linkg_node_info_t          peer;
    linkg_node_peer_snapshot_t snapshot;
    linkg_path_endpoint_t      endpoint;
    linkg_path_endpoint_t      endpoints[2];
    linkg_path_endpoint_t      next_hop;
    linkg_path_stats_t         stats;
    linkg_path_t              *path;
    uint32_t                   count;
    uint8_t                    peer_node_id;

    g_purge_calls  = 0U;
    g_purged_paths = 0U;
    local.node_id = 1U;
    local.role    = LINKG_DEVICE_ROLE_AP;
    peer.node_id  = 2U;
    peer.role     = LINKG_DEVICE_ROLE_STA;
    endpoint      = ipv4_endpoint("192.0.2.20", 5000U);

    TEST_CHECK_EQ(linkg_node_get_peer_count(&count), -ENODEV);
    local.node_id = 0U;
    TEST_CHECK_EQ(linkg_node_init(&local), -EINVAL);
    local.node_id = 1U;
    TEST_CHECK_EQ(linkg_node_init(&local), 0);
    TEST_CHECK_EQ(linkg_node_init(&local), -EALREADY);
    TEST_CHECK_EQ(linkg_node_get_local()->node_id, 1U);

    peer.role = LINKG_DEVICE_ROLE_AP;
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), -EINVAL);
    peer.role = LINKG_DEVICE_ROLE_STA;
    peer.node_id = local.node_id;
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), -EINVAL);
    peer.node_id = 2U;
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), 0);
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), 0);
    TEST_CHECK_EQ(linkg_node_get_peer_count(&count), 0);
    TEST_CHECK_EQ(count, 1U);

    TEST_CHECK_EQ(linkg_node_register_path(peer.node_id, 10U, &endpoint), 0);
    TEST_CHECK_EQ(linkg_node_register_path(peer.node_id, 10U, &endpoint), 0);
    endpoint = ipv4_endpoint("192.0.2.21", 5001U);
    TEST_CHECK_EQ(linkg_node_register_path(peer.node_id, 10U, &endpoint), 0);
    TEST_CHECK_EQ(linkg_node_register_path(peer.node_id, 11U, &endpoint), 0);
    TEST_CHECK_EQ(linkg_node_register_path(peer.node_id, 12U, &endpoint), -ENOSPC);
    TEST_CHECK_EQ(linkg_node_get_peer_snapshot(peer.node_id, &snapshot), 0);
    TEST_CHECK_EQ(snapshot.path_count, 2U);

    TEST_CHECK_EQ(linkg_node_get_path_endpoints(10U, endpoints, 2U, &count), 0);
    TEST_CHECK_EQ(count, 1U);
    TEST_CHECK_EQ(linkg_node_get_path_endpoints(10U, endpoints, 0U, &count), -EINVAL);
    TEST_CHECK_EQ(linkg_node_acquire_path_batch(peer.node_id, 10U, 2U, &path, &next_hop), 0);
    TEST_CHECK(path != NULL);
    TEST_CHECK_EQ(linkg_path_get_reference_count(path), 2U);

    linkg_path_record_rx(path, 128U, 2U);
    TEST_CHECK_EQ(linkg_path_get_stats(path, &stats), 0);
    TEST_CHECK_EQ(stats.rx_packets, 2U);
    peer_node_id = 0U;
    TEST_CHECK_EQ(linkg_node_account_path_rx(10U, &next_hop, 64U, 1U, &peer_node_id), 0);
    TEST_CHECK_EQ(peer_node_id, peer.node_id);
    TEST_CHECK_EQ(linkg_path_get_stats(path, &stats), 0);
    TEST_CHECK_EQ(stats.rx_packets, 3U);

    TEST_CHECK_EQ(linkg_node_unregister_peer(peer.node_id), 0);
    TEST_CHECK_EQ(g_purge_calls, 2U);
    TEST_CHECK_EQ(g_purged_paths, 2U);
    TEST_CHECK_EQ(linkg_node_get_peer_count(&count), 0);
    TEST_CHECK_EQ(count, 0U);
    TEST_CHECK_EQ(linkg_node_get_peer_snapshot(peer.node_id, &snapshot), -ENOENT);
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), -EBUSY);
    TEST_CHECK_EQ(linkg_node_deinit(), -EBUSY);

    linkg_path_release(path);
    TEST_CHECK_EQ(linkg_node_deinit(), -EBUSY);
    linkg_path_release(path);
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), 0);
    TEST_CHECK_EQ(linkg_node_unregister_peer(peer.node_id), 0);
    TEST_CHECK_EQ(linkg_node_deinit(), 0);
    TEST_CHECK_EQ(linkg_node_deinit(), 0);
}

static void test_sta_single_peer_constraint(void)
{
    linkg_node_info_t local;
    linkg_node_info_t peer;

    local.node_id = 10U;
    local.role    = LINKG_DEVICE_ROLE_STA;
    peer.node_id  = 1U;
    peer.role     = LINKG_DEVICE_ROLE_AP;

    TEST_CHECK_EQ(linkg_node_init(&local), 0);
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), 0);
    peer.node_id = 2U;
    TEST_CHECK_EQ(linkg_node_register_peer(&peer), -ENOSPC);
    TEST_CHECK_EQ(linkg_node_unregister_peer(1U), 0);
    TEST_CHECK_EQ(linkg_node_deinit(), 0);
}

int main(void)
{
    TEST_RUN(test_path_lifecycle_and_statistics);
    TEST_RUN(test_node_peer_and_deferred_release);
    TEST_RUN(test_sta_single_peer_constraint);

    return linkg_test_finish("node_path");
}
