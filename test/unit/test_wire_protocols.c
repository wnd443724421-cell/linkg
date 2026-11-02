/**
 * @file test_wire_protocols.c
 * @brief Discovery与Probe Wire协议逻辑测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <arpa/inet.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <stdint.h>
#include <string.h>

#include "discovery_wire.h"
#include "linkg_packet_pool.h"
#include "path_probe_internal.h"

static uint32_t random_next(uint32_t *state)
{
    uint32_t value;

    value  = *state;
    value ^= value << 13U;
    value ^= value >> 17U;
    value ^= value << 5U;
    *state = value;
    return value;
}

static void make_report(linkg_discovery_report_t *report, linkg_device_role_t role, uint8_t node_id)
{
    struct sockaddr_in6 *cellular;
    struct sockaddr_in  *wifi;

    memset(report, 0, sizeof(*report));
    report->session_id   = 0x0102030405060708ULL;
    report->revision     = 0x1112131415161718ULL;
    report->node.node_id = node_id;
    report->node.role    = role;
    report->path_flags   = LINKG_DISCOVERY_PATH_WIFI_VALID | LINKG_DISCOVERY_PATH_CELLULAR_VALID;

    wifi = (struct sockaddr_in *)&report->wifi_endpoint.address;
    wifi->sin_family = AF_INET;
    wifi->sin_port   = htons(29600U);
    (void)inet_pton(AF_INET, "11.21.191.2", &wifi->sin_addr);
    report->wifi_endpoint.length = sizeof(*wifi);

    cellular = (struct sockaddr_in6 *)&report->cellular_endpoint.address;
    cellular->sin6_family = AF_INET6;
    cellular->sin6_port   = htons(29603U);
    (void)inet_pton(AF_INET6, "2001:db8::2", &cellular->sin6_addr);
    report->cellular_endpoint.length = sizeof(*cellular);

    report->wifi_mac[0] = 0x02U;
    report->wifi_mac[1] = 0x11U;
    report->wifi_mac[2] = 0x22U;
    report->wifi_mac[3] = 0x33U;
    report->wifi_mac[4] = 0x44U;
    report->wifi_mac[5] = node_id;
}

static void test_probe_wire(void)
{
    linkg_packet_pool_config_t config;
    linkg_packet_pool_t        pool;
    linkg_packet_t            *packet;
    linkg_path_probe_wire_t    wire;
    uint8_t                    original[LINKG_PATH_PROBE_WIRE_SIZE];

    memset(&pool, 0, sizeof(pool));
    config.packet_count = 1U;
    config.slot_size    = 64U;
    config.headroom     = 0U;
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), 0);

    packet = linkg_packet_pool_alloc(&pool);
    TEST_CHECK(packet != NULL);
    TEST_CHECK_EQ(linkg_path_probe_wire_encode(packet, LINKG_PATH_PROBE_WIRE_TYPE_REQUEST, 0xAABBCCDDU), 0);
    TEST_CHECK_EQ(linkg_path_probe_wire_decode(packet, &wire), 0);
    TEST_CHECK_EQ(wire.type, LINKG_PATH_PROBE_WIRE_TYPE_REQUEST);
    TEST_CHECK_EQ(wire.sequence, 0xAABBCCDDU);
    memcpy(original, linkg_packet_const_data(packet), sizeof(original));

    linkg_packet_data(packet)[0] = 'X';
    TEST_CHECK_EQ(linkg_path_probe_wire_decode(packet, &wire), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    linkg_packet_data(packet)[3] = LINKG_PATH_PROBE_WIRE_TYPE_INVALID;
    TEST_CHECK_EQ(linkg_path_probe_wire_decode(packet, &wire), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    memset(linkg_packet_data(packet) + 4U, 0, 4U);
    TEST_CHECK_EQ(linkg_path_probe_wire_decode(packet, &wire), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    packet->data_length = LINKG_PATH_PROBE_WIRE_SIZE - 1U;
    TEST_CHECK_EQ(linkg_path_probe_wire_decode(packet, &wire), -EMSGSIZE);
    packet->data_length = 0U;
    TEST_CHECK_EQ(linkg_path_probe_wire_encode(packet, LINKG_PATH_PROBE_WIRE_TYPE_REQUEST, 0U), -EINVAL);
    TEST_CHECK_EQ(linkg_path_probe_wire_encode(packet, LINKG_PATH_PROBE_WIRE_TYPE_INVALID, 1U), -EINVAL);

    linkg_packet_release(packet);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), 0);
}

static void test_discovery_sta_report(void)
{
    linkg_discovery_report_t decoded;
    linkg_discovery_report_t report;
    uint8_t                  buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    uint8_t                  original[LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE];
    uint32_t                 length;

    make_report(&report, LINKG_DEVICE_ROLE_STA, 2U);
    TEST_CHECK_EQ(linkg_discovery_wire_encode_sta_report(&report, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(length, LINKG_DISCOVERY_WIRE_STA_REPORT_SIZE);
    TEST_CHECK_EQ(linkg_discovery_wire_decode_sta_report(buffer, length, &decoded), 0);
    TEST_CHECK_EQ(decoded.session_id, report.session_id);
    TEST_CHECK_EQ(decoded.revision, report.revision);
    TEST_CHECK_EQ(decoded.node.node_id, 2U);
    TEST_CHECK_EQ(decoded.node.role, LINKG_DEVICE_ROLE_STA);
    TEST_CHECK_EQ(decoded.path_flags, report.path_flags);
    TEST_CHECK_EQ(decoded.wifi_endpoint.length, sizeof(struct sockaddr_in));
    TEST_CHECK_EQ(decoded.cellular_endpoint.length, sizeof(struct sockaddr_in6));
    TEST_CHECK(memcmp(decoded.wifi_mac, report.wifi_mac, sizeof(report.wifi_mac)) == 0);

    memcpy(original, buffer, length);
    buffer[0] ^= 0x80U;
    TEST_CHECK_EQ(linkg_discovery_wire_decode_sta_report(buffer, length, &decoded), -EPROTO);
    memcpy(buffer, original, length);
    buffer[4] = 0U;
    TEST_CHECK_EQ(linkg_discovery_wire_decode_sta_report(buffer, length, &decoded), -EPROTO);
    memcpy(buffer, original, length);
    buffer[5] = LINKG_DISCOVERY_MESSAGE_AP_SYNC;
    TEST_CHECK_EQ(linkg_discovery_wire_decode_sta_report(buffer, length, &decoded), -EPROTO);
    memcpy(buffer, original, length);
    buffer[7]--;
    TEST_CHECK_EQ(linkg_discovery_wire_decode_sta_report(buffer, length, &decoded), -EMSGSIZE);
    TEST_CHECK_EQ(linkg_discovery_wire_decode_sta_report(original, length - 1U, &decoded), -EMSGSIZE);

    report.wifi_endpoint.address.ss_family = AF_UNIX;
    TEST_CHECK_EQ(linkg_discovery_wire_encode_sta_report(&report, buffer, sizeof(buffer), &length), -EAFNOSUPPORT);
}

static void test_discovery_ap_sync_and_leave(void)
{
    linkg_discovery_ap_sync_t decoded_sync;
    linkg_discovery_ap_sync_t sync;
    linkg_discovery_leave_t   decoded_leave;
    linkg_discovery_leave_t   leave;
    uint8_t                   buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE];
    uint32_t                  length;

    memset(&sync, 0, sizeof(sync));
    make_report(&sync.ap, LINKG_DEVICE_ROLE_AP, 1U);
    sync.topology_revision = 9U;
    sync.node_count        = 3U;
    sync.node_ids[0]       = 2U;
    sync.node_ids[1]       = 3U;
    sync.node_ids[2]       = 4U;

    TEST_CHECK_EQ(linkg_discovery_wire_encode_ap_sync(&sync, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(linkg_discovery_wire_decode_ap_sync(buffer, length, &decoded_sync), 0);
    TEST_CHECK_EQ(decoded_sync.topology_revision, 9U);
    TEST_CHECK_EQ(decoded_sync.node_count, 3U);
    TEST_CHECK(memcmp(decoded_sync.node_ids, sync.node_ids, sync.node_count) == 0);
    TEST_CHECK_EQ(linkg_discovery_wire_decode_ap_sync(buffer, length - 1U, &decoded_sync), -EMSGSIZE);

    sync.node_count = LINKG_RESOURCE_NETWORK_STA_MAX + 1U;
    TEST_CHECK_EQ(linkg_discovery_wire_encode_ap_sync(&sync, buffer, sizeof(buffer), &length), -EINVAL);

    leave.session_id = 11U;
    leave.revision   = 12U;
    leave.node_id    = 2U;
    TEST_CHECK_EQ(linkg_discovery_wire_encode_peer_leave(&leave, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(length, LINKG_DISCOVERY_WIRE_PEER_LEAVE_SIZE);
    TEST_CHECK_EQ(linkg_discovery_wire_decode_peer_leave(buffer, length, &decoded_leave), 0);
    TEST_CHECK_EQ(decoded_leave.session_id, leave.session_id);
    TEST_CHECK_EQ(decoded_leave.revision, leave.revision);
    TEST_CHECK_EQ(decoded_leave.node_id, leave.node_id);
    TEST_CHECK_EQ(linkg_discovery_wire_decode_peer_leave(buffer, length - 1U, &decoded_leave), -EMSGSIZE);
}

static void test_malformed_wire_corpus(void)
{
    linkg_discovery_ap_sync_t       sync;
    linkg_discovery_leave_t         leave;
    linkg_discovery_report_t        report;
    linkg_discovery_wire_header_t   header;
    linkg_packet_pool_config_t      config;
    linkg_packet_pool_t             pool;
    linkg_packet_t                 *packet;
    linkg_path_probe_wire_t         probe;
    uint8_t                         buffer[LINKG_DISCOVERY_WIRE_AP_SYNC_MAX_SIZE + 8U];
    uint32_t                        iteration;
    uint32_t                        index;
    uint32_t                        length;
    uint32_t                        state;
    int                             ret;

    memset(&pool, 0, sizeof(pool));
    config.packet_count = 1U;
    config.slot_size    = sizeof(buffer);
    config.headroom     = 0U;
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), 0);
    packet = linkg_packet_pool_alloc(&pool);
    TEST_CHECK(packet != NULL);

    state = 0x6D2B79F5U;
    for (iteration = 0U; iteration < 4096U; iteration++)
    {
        length = random_next(&state) % (uint32_t)(sizeof(buffer) + 1U);
        for (index = 0U; index < sizeof(buffer); index++)
        {
            buffer[index] = (uint8_t)random_next(&state);
        }

        ret = linkg_discovery_wire_decode_header(buffer, length, &header);
        TEST_CHECK(ret <= 0);
        ret = linkg_discovery_wire_decode_sta_report(buffer, length, &report);
        TEST_CHECK(ret <= 0);
        ret = linkg_discovery_wire_decode_ap_sync(buffer, length, &sync);
        TEST_CHECK(ret <= 0);
        ret = linkg_discovery_wire_decode_peer_leave(buffer, length, &leave);
        TEST_CHECK(ret <= 0);

        memcpy(linkg_packet_data(packet), buffer, sizeof(buffer));
        packet->data_length = length;
        ret = linkg_path_probe_wire_decode(packet, &probe);
        TEST_CHECK(ret <= 0);
    }

    linkg_packet_release(packet);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), 0);
}

int main(void)
{
    TEST_RUN(test_probe_wire);
    TEST_RUN(test_discovery_sta_report);
    TEST_RUN(test_discovery_ap_sync_and_leave);
    TEST_RUN(test_malformed_wire_corpus);

    return linkg_test_finish("wire_protocols");
}
