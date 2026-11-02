/**
 * @file test_switch_verify.c
 * @brief Switch双链路质量验证状态机测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-02
 */

#include "test_assert.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"
#include "linkg_path_probe.h"
#include "switch_policy_event.h"
#include "switch_policy_verify.h"

static uint32_t g_probe_submit_calls;
static uint32_t g_probe_cancel_calls;
static uint64_t g_next_probe_handle;

int linkg_path_probe_async_submit(uint8_t peer_node_id,
                                  uint32_t wifi_link_id,
                                  linkg_transport_class_t traffic_class,
                                  uint32_t response_timeout_ms,
                                  uint64_t *handle)
{
    if (peer_node_id == 0U || wifi_link_id == LINKG_LINK_ID_INVALID ||
        traffic_class >= LINKG_TRANSPORT_CLASS_COUNT || response_timeout_ms == 0U || handle == NULL)
    {
        return -EINVAL;
    }

    g_probe_submit_calls++;
    g_next_probe_handle++;
    *handle = g_next_probe_handle;
    return 0;
}

int linkg_path_probe_async_poll(uint64_t handle, linkg_path_probe_async_result_t *result)
{
    if (handle == 0U || result == NULL)
    {
        return -EINVAL;
    }

    memset(result, 0, sizeof(*result));
    result->state = LINKG_PATH_PROBE_ASYNC_PENDING;
    return 0;
}

int linkg_path_probe_async_cancel(uint64_t handle)
{
    if (handle == 0U)
    {
        return -EINVAL;
    }

    g_probe_cancel_calls++;
    return 0;
}

static linkg_switch_observation_t usable_observation(uint8_t peer_node_id, uint64_t collected_us)
{
    linkg_switch_observation_t observation;

    memset(&observation, 0, sizeof(observation));
    observation.valid            = true;
    observation.peer_node_id     = peer_node_id;
    observation.collected_us     = collected_us;
    observation.wifi.state       = LINKG_SWITCH_OBSERVATION_USABLE;
    observation.wifi.link_id     = 10U;
    observation.cellular.state   = LINKG_SWITCH_OBSERVATION_USABLE;
    observation.cellular.link_id = 20U;
    return observation;
}

static linkg_switch_policy_verify_begin_params_t loss_params(linkg_switch_observation_t *observation,
                                                              uint64_t now_us)
{
    linkg_switch_policy_verify_begin_params_t params;

    memset(&params, 0, sizeof(params));
    params.peer_node_id    = observation->peer_node_id;
    params.peer_generation = 7U;
    params.class_mask      = LINKG_SWITCH_POLICY_CLASS_MASK_ALL;
    params.reason          = LINKG_SWITCH_POLICY_REASON_LOSS;
    params.recovery_reason = LINKG_SWITCH_POLICY_MIGRATION_NONE;
    params.observation     = observation;
    params.now_us          = now_us;
    params.operation_ids[LINKG_TRANSPORT_CLASS_REALTIME] = 101U;
    params.operation_ids[LINKG_TRANSPORT_CLASS_VIDEO]    = 102U;
    params.operation_ids[LINKG_TRANSPORT_CLASS_DATA]     = 103U;
    return params;
}

static void test_duration_and_begin_validation(void)
{
    linkg_switch_observation_t                observation;
    linkg_switch_policy_verify_begin_params_t params;
    linkg_switch_policy_verify_runtime_t      runtime;
    uint64_t                                  now_us;

    TEST_CHECK_EQ(linkg_switch_policy_verify_duration_us(LINKG_SWITCH_POLICY_REASON_LOSS,
                                                         LINKG_SWITCH_POLICY_MIGRATION_NONE),
                  LINKG_SWITCH_POLICY_VERIFY_LOSS_DURATION_US);
    TEST_CHECK_EQ(linkg_switch_policy_verify_duration_us(LINKG_SWITCH_POLICY_REASON_RECOVERY,
                                                         LINKG_SWITCH_POLICY_MIGRATION_LOAD),
                  LINKG_SWITCH_POLICY_VERIFY_LOAD_DURATION_US);

    now_us      = 10000000U;
    observation = usable_observation(2U, now_us - 100U);
    params      = loss_params(&observation, now_us);
    memset(&runtime, 0, sizeof(runtime));

    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(NULL, &params), -EINVAL);
    params.peer_node_id = 0U;
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), -EINVAL);
    params.peer_node_id = observation.peer_node_id;
    params.class_mask   = 0U;
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), -EINVAL);
    params.class_mask = LINKG_SWITCH_POLICY_CLASS_MASK_ALL;
    params.operation_ids[LINKG_TRANSPORT_CLASS_VIDEO] = params.operation_ids[LINKG_TRANSPORT_CLASS_REALTIME];
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), -EINVAL);
    params.operation_ids[LINKG_TRANSPORT_CLASS_VIDEO] = 102U;

    observation.collected_us = now_us - LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US - 1U;
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), -EINVAL);
    observation.collected_us = now_us - 100U;
    observation.cellular.state = LINKG_SWITCH_OBSERVATION_UNUSABLE;
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), -EINVAL);
    observation.cellular.state = LINKG_SWITCH_OBSERVATION_USABLE;

    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), 0);
    TEST_CHECK(runtime.active);
    TEST_CHECK_EQ(runtime.operation_id, 101U);
    TEST_CHECK_EQ(runtime.deadline_us, now_us + LINKG_SWITCH_POLICY_VERIFY_LOSS_DURATION_US);
    TEST_CHECK_EQ(runtime.wifi_loss_score, LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE);
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), -EBUSY);
    linkg_switch_policy_verify_reset(&runtime);
    TEST_CHECK(!runtime.active);
}

static void test_early_failure_and_stable_result(void)
{
    linkg_switch_observation_t                  observation;
    linkg_switch_policy_verify_begin_params_t   begin;
    linkg_switch_policy_verify_process_output_t output;
    linkg_switch_policy_verify_process_params_t process;
    linkg_switch_policy_verify_runtime_t        runtime;
    uint64_t                                    now_us;

    now_us      = 20000000U;
    observation = usable_observation(3U, now_us - 10U);
    begin       = loss_params(&observation, now_us);
    begin.peer_node_id = observation.peer_node_id;
    memset(&runtime, 0, sizeof(runtime));
    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &begin), 0);

    memset(&process, 0, sizeof(process));
    process.observation = &observation;
    process.now_us      = now_us + 100U;
    process.activity_valid = true;
    process.active_class_mask = LINKG_SWITCH_POLICY_CLASS_MASK_ALL;
    TEST_CHECK_EQ(linkg_switch_policy_verify_process(&runtime, &process, &output), 0);
    TEST_CHECK(!output.event_ready);

    observation.collected_us   = process.now_us;
    observation.wifi.state     = LINKG_SWITCH_OBSERVATION_UNUSABLE;
    observation.cellular.state = LINKG_SWITCH_OBSERVATION_USABLE;
    TEST_CHECK_EQ(linkg_switch_policy_verify_process(&runtime, &process, &output), 0);
    TEST_CHECK(output.event_ready);
    TEST_CHECK_EQ(output.event.type, LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE);
    TEST_CHECK_EQ(output.event.payload.verify_done.result, LINKG_SWITCH_POLICY_VERIFY_CELLULAR);
    TEST_CHECK_EQ(output.event.payload.verify_done.cellular_target_mask, LINKG_SWITCH_POLICY_CLASS_MASK_ALL);
    TEST_CHECK_EQ(output.event.payload.verify_done.wifi_target_mask, 0U);

    observation.wifi.state = LINKG_SWITCH_OBSERVATION_USABLE;
    TEST_CHECK_EQ(linkg_switch_policy_verify_process(&runtime, &process, &output), 0);
    TEST_CHECK(output.event_ready);
    TEST_CHECK_EQ(output.event.payload.verify_done.result, LINKG_SWITCH_POLICY_VERIFY_CELLULAR);
    TEST_CHECK_EQ(output.event.occurred_us, now_us + 100U);
}

static void test_recovery_load_probe_lifecycle(void)
{
    linkg_switch_observation_t                observation;
    linkg_switch_policy_verify_begin_params_t params;
    linkg_switch_policy_verify_runtime_t      runtime;
    uint32_t                                  mask;
    uint64_t                                  now_us;

    g_probe_submit_calls = 0U;
    g_probe_cancel_calls = 0U;
    g_next_probe_handle  = 1000U;
    now_us               = 30000000U;
    observation          = usable_observation(4U, now_us - 10U);
    memset(&params, 0, sizeof(params));
    memset(&runtime, 0, sizeof(runtime));

    mask = LINKG_SWITCH_POLICY_CLASS_BIT(LINKG_TRANSPORT_CLASS_REALTIME) |
           LINKG_SWITCH_POLICY_CLASS_BIT(LINKG_TRANSPORT_CLASS_VIDEO);
    params.peer_node_id    = observation.peer_node_id;
    params.peer_generation = 9U;
    params.operation_ids[LINKG_TRANSPORT_CLASS_REALTIME] = 201U;
    params.operation_ids[LINKG_TRANSPORT_CLASS_VIDEO]    = 202U;
    params.class_mask      = mask;
    params.reason          = LINKG_SWITCH_POLICY_REASON_RECOVERY;
    params.recovery_reason = LINKG_SWITCH_POLICY_MIGRATION_LOAD;
    params.observation     = &observation;
    params.now_us          = now_us;

    TEST_CHECK_EQ(linkg_switch_policy_verify_begin(&runtime, &params), 0);
    TEST_CHECK_EQ(runtime.deadline_us, now_us + LINKG_SWITCH_POLICY_VERIFY_LOAD_DURATION_US);
    TEST_CHECK_EQ(runtime.wifi_load_score[LINKG_TRANSPORT_CLASS_REALTIME], LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE);
    TEST_CHECK_EQ(runtime.wifi_load_score[LINKG_TRANSPORT_CLASS_VIDEO], LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE);
    TEST_CHECK_EQ(runtime.wifi_load_score[LINKG_TRANSPORT_CLASS_DATA], 0U);
    TEST_CHECK_EQ(g_probe_submit_calls, 2U);
    TEST_CHECK(runtime.wifi_load_probe_handle[LINKG_TRANSPORT_CLASS_REALTIME] != 0U);
    TEST_CHECK(runtime.wifi_load_probe_handle[LINKG_TRANSPORT_CLASS_VIDEO] != 0U);

    linkg_switch_policy_verify_reset(&runtime);
    TEST_CHECK_EQ(g_probe_cancel_calls, 2U);
    TEST_CHECK(!runtime.active);
    TEST_CHECK_EQ(runtime.result, LINKG_SWITCH_POLICY_VERIFY_NONE);
}

int main(void)
{
    TEST_RUN(test_duration_and_begin_validation);
    TEST_RUN(test_early_failure_and_stable_result);
    TEST_RUN(test_recovery_load_probe_lifecycle);

    return linkg_test_finish("switch_verify");
}
