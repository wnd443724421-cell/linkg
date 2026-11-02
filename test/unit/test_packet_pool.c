/**
 * @file test_packet_pool.c
 * @brief Packet Pool边界和引用生命周期测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdint.h>
#include <string.h>

#include "linkg_packet_pool.h"

static void test_invalid_configuration(void)
{
    linkg_packet_pool_config_t config;
    linkg_packet_pool_t        pool;

    memset(&pool, 0, sizeof(pool));
    memset(&config, 0, sizeof(config));

    TEST_CHECK_EQ(linkg_packet_pool_init(NULL, &config), -EINVAL);
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, NULL), -EINVAL);
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), -EINVAL);

    config.packet_count = 1U;
    config.slot_size    = 64U;
    config.headroom     = 64U;
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), -EINVAL);
}

static void test_allocation_and_reference_lifecycle(void)
{
    linkg_packet_pool_config_t config;
    linkg_packet_pool_t        pool;
    linkg_packet_t            *packet;
    linkg_packet_t            *packets[12];
    uint32_t                   index;
    uint32_t                   count;

    memset(&pool, 0, sizeof(pool));
    memset(packets, 0, sizeof(packets));

    config.packet_count = 8U;
    config.slot_size    = 256U;
    config.headroom     = 32U;

    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), 0);
    TEST_CHECK_EQ(linkg_packet_pool_init(&pool, &config), -EALREADY);
    TEST_CHECK_EQ(pool.free_count, 8U);

    packet = linkg_packet_pool_alloc(&pool);
    TEST_CHECK(packet != NULL);
    TEST_CHECK_EQ(pool.free_count, 7U);
    TEST_CHECK_EQ(packet->data_offset, 32U);
    TEST_CHECK_EQ(packet->data_length, 0U);
    TEST_CHECK_EQ((uintptr_t)packet->slot % 64U, 0U);

    packet->data_length = 12U;
    memset(linkg_packet_data(packet), 0x5A, packet->data_length);
    TEST_CHECK(linkg_packet_push(packet, 16U) != NULL);
    TEST_CHECK_EQ(packet->data_offset, 16U);
    TEST_CHECK_EQ(packet->data_length, 28U);
    TEST_CHECK(linkg_packet_pull(packet, 16U) != NULL);
    TEST_CHECK_EQ(packet->data_offset, 32U);
    TEST_CHECK_EQ(packet->data_length, 12U);
    TEST_CHECK(linkg_packet_pull(packet, 13U) == NULL);
    TEST_CHECK(linkg_packet_push(packet, 33U) == NULL);

    linkg_packet_set_realtime(packet, true);
    TEST_CHECK(linkg_packet_is_realtime(packet));
    TEST_CHECK(!linkg_packet_is_video(packet));
    linkg_packet_set_video(packet, true);
    TEST_CHECK(!linkg_packet_is_realtime(packet));
    TEST_CHECK(linkg_packet_is_video(packet));
    linkg_packet_set_data(packet);
    TEST_CHECK(linkg_packet_is_data(packet));

    linkg_packet_retain(packet);
    TEST_CHECK_EQ(atomic_load(&packet->reference_count), 2U);
    linkg_packet_release(packet);
    TEST_CHECK_EQ(pool.free_count, 7U);
    linkg_packet_release(packet);
    TEST_CHECK_EQ(pool.free_count, 8U);

    count = linkg_packet_pool_alloc_batch(&pool, packets, 12U);
    TEST_CHECK_EQ(count, 8U);
    TEST_CHECK_EQ(pool.free_count, 0U);
    TEST_CHECK(linkg_packet_pool_alloc(&pool) == NULL);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), -EBUSY);

    for (index = 0U; index < count; index++)
    {
        TEST_CHECK(packets[index] != NULL);
        TEST_CHECK_EQ(atomic_load(&packets[index]->reference_count), 1U);
    }

    linkg_packet_pool_release_batch(&pool, packets, count);
    TEST_CHECK_EQ(pool.free_count, 8U);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), 0);
    TEST_CHECK_EQ(linkg_packet_pool_deinit(&pool), 0);
}

int main(void)
{
    TEST_RUN(test_invalid_configuration);
    TEST_RUN(test_allocation_and_reference_lifecycle);

    return linkg_test_finish("packet_pool");
}
