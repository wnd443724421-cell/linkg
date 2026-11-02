/**
 * @file test_switch_wire.c
 * @brief Switch质量、发送计划和维护Wire协议测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "switch_wire.h"

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

static void test_plan_roundtrip(void)
{
    linkg_switch_wire_message_t   message;
    linkg_switch_wire_plan_sync_t plan;
    uint8_t                       buffer[LINKG_SWITCH_WIRE_MAX_SIZE];
    uint8_t                       original[LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE];
    size_t                        length;

    memset(&plan, 0, sizeof(plan));
    plan.classes[LINKG_TRANSPORT_CLASS_REALTIME].mode           = LINKG_SWITCH_WIRE_MODE_SINGLE;
    plan.classes[LINKG_TRANSPORT_CLASS_REALTIME].primary_access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].mode               = LINKG_SWITCH_WIRE_MODE_REDUNDANT;
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].primary_access     = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_access   = LINKG_SWITCH_WIRE_ACCESS_CELLULAR;
    plan.classes[LINKG_TRANSPORT_CLASS_DATA].mode                = LINKG_SWITCH_WIRE_MODE_SINGLE;
    plan.classes[LINKG_TRANSPORT_CLASS_DATA].primary_access      = LINKG_SWITCH_WIRE_ACCESS_CELLULAR;

    TEST_CHECK_EQ(linkg_switch_wire_encode_plan_sync(100U, &plan, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(length, LINKG_SWITCH_WIRE_PLAN_SYNC_SIZE);
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), 0);
    TEST_CHECK_EQ(message.header.message_id, 100U);
    TEST_CHECK_EQ(message.header.type, LINKG_SWITCH_WIRE_TYPE_PLAN_SYNC);
    TEST_CHECK_EQ(message.payload.plan_sync.classes[LINKG_TRANSPORT_CLASS_VIDEO].mode, LINKG_SWITCH_WIRE_MODE_REDUNDANT);
    TEST_CHECK_EQ(message.payload.plan_sync.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_access, LINKG_SWITCH_WIRE_ACCESS_CELLULAR);

    memcpy(original, buffer, length);
    buffer[LINKG_SWITCH_WIRE_HEADER_SIZE + 3U] = 1U;
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), -EINVAL);
    memcpy(buffer, original, length);
    buffer[0] = 0U;
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), -EPROTONOSUPPORT);
    memcpy(buffer, original, length);
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length - 1U, &message), -EMSGSIZE);

    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    TEST_CHECK_EQ(linkg_switch_wire_encode_plan_sync(101U, &plan, buffer, sizeof(buffer), &length), -EINVAL);
    plan.classes[LINKG_TRANSPORT_CLASS_VIDEO].secondary_access = LINKG_SWITCH_WIRE_ACCESS_CELLULAR;
    TEST_CHECK_EQ(linkg_switch_wire_encode_plan_sync(0U, &plan, buffer, sizeof(buffer), &length), -EINVAL);
}

static void test_quality_roundtrip_and_validation(void)
{
    linkg_switch_wire_message_t        message;
    linkg_switch_wire_quality_report_t report;
    uint8_t                            buffer[LINKG_SWITCH_WIRE_MAX_SIZE];
    size_t                             length;

    memset(&report, 0, sizeof(report));
    report.wifi_uplink.sample_packets = 100U;
    report.wifi_uplink.lost_packets   = 3U;
    report.cellular_uplink.sample_packets = 50U;
    report.wifi_downlink_hcc.flags = LINKG_SWITCH_WIRE_HCC_FLAG_VALID |
                                     LINKG_SWITCH_WIRE_HCC_FLAG_TX_ALLOWED;
    report.wifi_downlink_hcc.vo_queue_length    = 1U;
    report.wifi_downlink_hcc.vi_queue_length    = 2U;
    report.wifi_downlink_hcc.be_queue_length    = 3U;
    report.wifi_downlink_hcc.flowctrl_off_count = 4U;

    TEST_CHECK_EQ(linkg_switch_wire_encode_quality_report(7U, &report, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(length, LINKG_SWITCH_WIRE_QUALITY_REPORT_SIZE);
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), 0);
    TEST_CHECK_EQ(message.payload.quality_report.wifi_uplink.sample_packets, 100U);
    TEST_CHECK_EQ(message.payload.quality_report.wifi_uplink.lost_packets, 3U);
    TEST_CHECK_EQ(message.payload.quality_report.wifi_downlink_hcc.flowctrl_off_count, 4U);

    report.wifi_uplink.sample_packets = 0U;
    report.wifi_uplink.lost_packets   = 1U;
    TEST_CHECK_EQ(linkg_switch_wire_encode_quality_report(8U, &report, buffer, sizeof(buffer), &length), -EINVAL);

    memset(&report, 0, sizeof(report));
    report.wifi_downlink_hcc.flags = LINKG_SWITCH_WIRE_HCC_FLAG_TX_ALLOWED;
    TEST_CHECK_EQ(linkg_switch_wire_encode_quality_report(8U, &report, buffer, sizeof(buffer), &length), -EINVAL);
}

static void test_ack_and_maintenance(void)
{
    linkg_switch_wire_maintenance_ack_t maintenance_ack;
    linkg_switch_wire_maintenance_t     maintenance;
    linkg_switch_wire_message_t         message;
    linkg_switch_wire_plan_ack_t        ack;
    uint8_t                             buffer[LINKG_SWITCH_WIRE_MAX_SIZE];
    size_t                              length;

    ack.status = -ETIMEDOUT;
    TEST_CHECK_EQ(linkg_switch_wire_encode_plan_ack(20U, &ack, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), 0);
    TEST_CHECK_EQ(message.payload.plan_ack.status, -ETIMEDOUT);
    ack.status = 1;
    TEST_CHECK_EQ(linkg_switch_wire_encode_plan_ack(20U, &ack, buffer, sizeof(buffer), &length), -EINVAL);

    maintenance.access = LINKG_SWITCH_WIRE_ACCESS_WIFI;
    maintenance.phase  = LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN;
    TEST_CHECK_EQ(linkg_switch_wire_encode_maintenance(30U, &maintenance, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), 0);
    TEST_CHECK_EQ(message.payload.maintenance.access, LINKG_SWITCH_WIRE_ACCESS_WIFI);
    TEST_CHECK_EQ(message.payload.maintenance.phase, LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_BEGIN);

    maintenance.phase = LINKG_SWITCH_WIRE_MAINTENANCE_PHASE_NONE;
    TEST_CHECK_EQ(linkg_switch_wire_encode_maintenance(30U, &maintenance, buffer, sizeof(buffer), &length), -EINVAL);

    maintenance_ack.access = LINKG_SWITCH_WIRE_ACCESS_CELLULAR;
    TEST_CHECK_EQ(linkg_switch_wire_encode_maintenance_ack(31U, &maintenance_ack, buffer, sizeof(buffer), &length), 0);
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), 0);
    TEST_CHECK_EQ(message.payload.maintenance_ack.access, LINKG_SWITCH_WIRE_ACCESS_CELLULAR);
    buffer[LINKG_SWITCH_WIRE_HEADER_SIZE + 1U] = 1U;
    TEST_CHECK_EQ(linkg_switch_wire_decode(buffer, length, &message), -EINVAL);
}

static void test_malformed_wire_corpus(void)
{
    linkg_switch_wire_message_t message;
    uint8_t                     buffer[LINKG_SWITCH_WIRE_MAX_SIZE + 8U];
    uint32_t                    iteration;
    uint32_t                    index;
    uint32_t                    state;
    size_t                      length;
    int                         ret;

    state = 0xA341316CU;
    for (iteration = 0U; iteration < 4096U; iteration++)
    {
        length = random_next(&state) % (sizeof(buffer) + 1U);
        for (index = 0U; index < sizeof(buffer); index++)
        {
            buffer[index] = (uint8_t)random_next(&state);
        }

        ret = linkg_switch_wire_decode(buffer, length, &message);
        TEST_CHECK(ret <= 0);
    }
}

int main(void)
{
    TEST_RUN(test_plan_roundtrip);
    TEST_RUN(test_quality_roundtrip_and_validation);
    TEST_RUN(test_ack_and_maintenance);
    TEST_RUN(test_malformed_wire_corpus);

    return linkg_test_finish("switch_wire");
}
