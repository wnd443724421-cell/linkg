/**
 * @file test_transport.c
 * @brief Transport Wire、接收窗口和分片重组逻辑测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <errno.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_packet_pool.h"
#include "linkg_time.h"
#include "linkg_transport_types.h"
#include "transport_internal.h"

linkg_transport_context_t g_transport;

bool linkg_transport_type_valid(linkg_transport_type_t type)
{
    return type > LINKG_TRANSPORT_TYPE_NONE && type < LINKG_TRANSPORT_TYPE_COUNT;
}

bool linkg_transport_class_valid(linkg_transport_class_t traffic_class)
{
    return traffic_class >= LINKG_TRANSPORT_CLASS_REALTIME &&
           traffic_class < LINKG_TRANSPORT_CLASS_COUNT;
}

bool linkg_transport_peer_epoch_matches(const linkg_transport_peer_t *peer, uint32_t peer_epoch)
{
    return peer != NULL &&
           peer->valid &&
           (peer_epoch & 1U) == 0U &&
           atomic_load(&peer->lifecycle_epoch) == peer_epoch;
}

static linkg_packet_t *create_payload(linkg_packet_pool_t *pool, const uint8_t *data, uint32_t length)
{
    linkg_packet_t *packet;

    packet = linkg_packet_pool_alloc(pool);
    if (packet == NULL || length > linkg_packet_capacity(packet))
    {
        return NULL;
    }

    memcpy(linkg_packet_data(packet), data, length);
    packet->data_length = length;
    return packet;
}

static linkg_packet_t *create_fragment(linkg_packet_pool_t *pool,
                                       const uint8_t *data,
                                       uint32_t length,
                                       const linkg_transport_header_t *header,
                                       const linkg_transport_fragment_header_t *fragment)
{
    linkg_packet_t *packet;

    packet = create_payload(pool, data, length);
    if (packet == NULL)
    {
        return NULL;
    }

    if (linkg_transport_wire_fragment_encode(packet, fragment) != 0 ||
        linkg_transport_wire_encode(packet, header) != 0)
    {
        linkg_packet_release(packet);
        return NULL;
    }

    return packet;
}

static void test_wire_roundtrip_and_rejection(void)
{
    const uint8_t payload[] = {0x11U, 0x22U, 0x33U};
    linkg_packet_pool_config_t config;
    linkg_packet_pool_t        pool;
    linkg_packet_t            *packet;
    linkg_transport_header_t   decoded;
    linkg_transport_header_t   header;
    uint8_t                    original[LINKG_TRANSPORT_WIRE_HEADER_SIZE];

    memset(&pool, 0, sizeof(pool));
    config.packet_count = 2U;
    config.slot_size    = 2048U;
    config.headroom     = 64U;
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), 0);

    packet = create_payload(&pool, payload, sizeof(payload));
    TEST_CHECK(packet != NULL);

    memset(&header, 0, sizeof(header));
    header.type                = LINKG_TRANSPORT_TYPE_USER_DATA;
    header.sequence            = 0x12345678U;
    header.source_node_id      = 1U;
    header.destination_node_id = 254U;

    TEST_CHECK_EQ(linkg_transport_wire_encode(packet, &header), 0);
    TEST_CHECK_EQ(packet->data_length, sizeof(payload) + LINKG_TRANSPORT_WIRE_HEADER_SIZE);
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), 0);
    TEST_CHECK_EQ(decoded.magic, LINKG_TRANSPORT_WIRE_MAGIC);
    TEST_CHECK_EQ(decoded.version, LINKG_TRANSPORT_WIRE_VERSION);
    TEST_CHECK_EQ(decoded.type, header.type);
    TEST_CHECK_EQ(decoded.sequence, header.sequence);
    TEST_CHECK_EQ(decoded.source_node_id, 1U);
    TEST_CHECK_EQ(decoded.destination_node_id, 254U);

    TEST_CHECK_EQ(linkg_transport_wire_update_sequence(packet, UINT32_MAX), 0);
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), 0);
    TEST_CHECK_EQ(decoded.sequence, UINT32_MAX);

    memcpy(original, linkg_packet_data(packet), sizeof(original));

    linkg_packet_data(packet)[0] ^= 0x80U;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    linkg_packet_data(packet)[4] = 0U;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    linkg_packet_data(packet)[5] = LINKG_TRANSPORT_TYPE_COUNT;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    linkg_packet_data(packet)[6] = 0x80U;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    linkg_packet_data(packet)[12] = 0U;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    linkg_packet_data(packet)[14] = 1U;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EPROTO);
    memcpy(linkg_packet_data(packet), original, sizeof(original));

    packet->data_length = LINKG_TRANSPORT_WIRE_HEADER_SIZE - 1U;
    TEST_CHECK_EQ(linkg_transport_wire_decode(packet, &decoded), -EMSGSIZE);

    linkg_packet_release(packet);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), 0);
}

static void test_receive_window(void)
{
    linkg_transport_rx_window_t window;
    uint32_t                    lost;

    memset(&window, 0xA5, sizeof(window));
    linkg_transport_window_reset(&window);

    TEST_CHECK_EQ(linkg_transport_window_accept(NULL, 1U), LINKG_TRANSPORT_WINDOW_INVALID);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 100U), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 100U), LINKG_TRANSPORT_WINDOW_DUPLICATE);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 102U), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 101U), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 101U), LINKG_TRANSPORT_WINDOW_DUPLICATE);

    lost = 99U;
    TEST_CHECK_EQ(linkg_transport_window_accept_ex(&window, 1126U, &lost), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(lost, 512U);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 102U), LINKG_TRANSPORT_WINDOW_TOO_OLD);

    linkg_transport_window_reset(&window);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, UINT32_MAX - 1U), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, 0U), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, UINT32_MAX), LINKG_TRANSPORT_WINDOW_ACCEPT);
    TEST_CHECK_EQ(linkg_transport_window_accept(&window, UINT32_MAX), LINKG_TRANSPORT_WINDOW_DUPLICATE);
}

static void test_fragment_wire_and_reassembly(void)
{
    uint8_t                              payload[LINKG_TRANSPORT_PACKET_MAX_SIZE];
    linkg_packet_pool_config_t           config;
    linkg_packet_pool_t                  pool;
    linkg_packet_t                      *first_packet;
    linkg_packet_t                      *tail_packet;
    linkg_transport_fragment_header_t    first_fragment;
    linkg_transport_fragment_header_t    tail_fragment;
    linkg_transport_header_t             header;
    linkg_transport_peer_t               peer;
    linkg_transport_reassembly_submit_item_t item;
    uint32_t                              index;

    for (index = 0U; index < sizeof(payload); index++)
    {
        payload[index] = (uint8_t)(index * 31U + 7U);
    }

    memset(&g_transport, 0, sizeof(g_transport));
    memset(&pool, 0, sizeof(pool));
    memset(&peer, 0, sizeof(peer));

    config.packet_count = 6U;
    config.slot_size    = 2048U;
    config.headroom     = 64U;
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), 0);
    TEST_CHECK_EQ(linkg_time_init(), 0);
    TEST_CHECK_EQ(linkg_transport_reassembly_runtime_init(), 0);
    TEST_CHECK_EQ(linkg_transport_reassembly_runtime_init(), -EALREADY);

    peer.valid = true;
    atomic_init(&peer.lifecycle_epoch, 2U);

    memset(&header, 0, sizeof(header));
    header.type                = LINKG_TRANSPORT_TYPE_USER_DATA;
    header.flags               = LINKG_TRANSPORT_FLAG_FRAGMENT;
    header.sequence            = 10U;
    header.source_node_id      = 1U;
    header.destination_node_id = 2U;

    first_fragment.packet_id       = 77U;
    first_fragment.fragment_offset = 0U;
    first_fragment.packet_length   = sizeof(payload);
    tail_fragment                  = first_fragment;
    tail_fragment.fragment_offset  = LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE;

    first_packet = create_fragment(&pool,
                                   payload,
                                   LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE,
                                   &header,
                                   &first_fragment);
    tail_packet = create_fragment(&pool,
                                  payload + LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE,
                                  sizeof(payload) - LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE,
                                  &header,
                                  &tail_fragment);
    TEST_CHECK(first_packet != NULL);
    TEST_CHECK(tail_packet != NULL);

    memset(&item, 0, sizeof(item));
    item.header          = &header;
    item.fragment_header = &first_fragment;
    item.packet          = first_packet;
    item.peer            = &peer;
    item.peer_epoch      = 2U;
    item.traffic_class   = LINKG_TRANSPORT_CLASS_VIDEO;
    item.peer_node_id    = 2U;

    TEST_CHECK_EQ(linkg_transport_reassembly_submit_batch(&item, 1U), 0);
    TEST_CHECK_EQ(item.result, 0);
    TEST_CHECK(item.completed_packet == NULL);
    linkg_packet_release(first_packet);

    item.fragment_header = &tail_fragment;
    item.packet          = tail_packet;
    TEST_CHECK_EQ(linkg_transport_reassembly_submit_batch(&item, 1U), 0);
    TEST_CHECK_EQ(item.result, 1);
    TEST_CHECK(item.completed_packet != NULL);
    TEST_CHECK_EQ(item.completed_packet->data_length, sizeof(payload));
    TEST_CHECK(memcmp(linkg_packet_const_data(item.completed_packet), payload, sizeof(payload)) == 0);

    linkg_packet_release(tail_packet);
    linkg_packet_release(item.completed_packet);
    TEST_CHECK_EQ(pool.free_count, pool.packet_count);

    first_packet = create_fragment(&pool,
                                   payload,
                                   LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE,
                                   &header,
                                   &first_fragment);
    TEST_CHECK(first_packet != NULL);
    item.fragment_header = &first_fragment;
    item.packet          = first_packet;
    item.peer_epoch      = 3U;
    TEST_CHECK_EQ(linkg_transport_reassembly_submit_batch(&item, 1U), 0);
    TEST_CHECK_EQ(item.result, -ESTALE);
    linkg_packet_release(first_packet);

    first_packet = create_fragment(&pool,
                                   payload,
                                   LINKG_TRANSPORT_FRAGMENT_PAYLOAD_MAX_SIZE,
                                   &header,
                                   &first_fragment);
    TEST_CHECK(first_packet != NULL);
    item.packet     = first_packet;
    item.peer_epoch = 2U;
    TEST_CHECK_EQ(linkg_transport_reassembly_submit_batch(&item, 1U), 0);
    TEST_CHECK_EQ(item.result, 0);
    TEST_CHECK_EQ(linkg_transport_reassembly_reset_peer(2U), 0);
    linkg_packet_release(first_packet);
    TEST_CHECK_EQ(pool.free_count, pool.packet_count);

    TEST_CHECK_EQ(linkg_transport_reassembly_reset_peer(0U), -EINVAL);
    TEST_CHECK_EQ(linkg_transport_reassembly_runtime_deinit(), 0);
    TEST_CHECK_EQ(linkg_transport_reassembly_runtime_deinit(), 0);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), 0);
}

int main(void)
{
    TEST_RUN(test_wire_roundtrip_and_rejection);
    TEST_RUN(test_receive_window);
    TEST_RUN(test_fragment_wire_and_reassembly);

    return linkg_test_finish("transport");
}
