#include "cellular_runtime.h"

#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>

int main(void)
{
    cellular_runtime_t runtime;
    uint64_t           now_ms;
    int                ret;

    now_ms = 100U;

    ret = cellular_runtime_init(&runtime, now_ms);
    assert(ret == 0);
    assert(runtime.state == CELLULAR_RUNTIME_STATE_IDLE);
    assert(runtime.generation == 1U);
    assert(!cellular_runtime_session_active(&runtime));

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_WAIT_SIM, 200U, 0U);
    assert(ret == 0);
    assert(runtime.previous_state == CELLULAR_RUNTIME_STATE_IDLE);
    assert(runtime.state == CELLULAR_RUNTIME_STATE_WAIT_SIM);
    assert(cellular_runtime_get_deadline(&runtime) == 0U);

    ret = cellular_runtime_begin_session(&runtime, 300U);
    assert(ret == 0);
    assert(runtime.session_generation == 1U);
    assert(cellular_runtime_session_active(&runtime));
    assert(!runtime.pin_attempted);

    ret = cellular_runtime_begin_session(&runtime, 301U);
    assert(ret == -EALREADY);

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_CHECK_SIM, 400U, 5000U);
    assert(ret == 0);
    assert(runtime.state_deadline_ms == 5400U);
    assert(!cellular_runtime_state_timed_out(&runtime, 5399U));
    assert(cellular_runtime_state_timed_out(&runtime, 5400U));

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_ENTER_PIN, 500U, 0U);
    assert(ret == 0);

    ret = cellular_runtime_mark_pin_attempted(&runtime, 501U);
    assert(ret == 0);
    assert(cellular_runtime_pin_attempted(&runtime));

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_CHECK_SIM, 600U, 5000U);
    assert(ret == 0);

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_ENTER_PIN, 700U, 0U);
    assert(ret == -EALREADY);

    ret = cellular_runtime_note_attempt(&runtime, 800U);
    assert(ret == 0);
    assert(runtime.state_attempt_count == 1U);

    ret = cellular_runtime_record_failure(&runtime, -ETIMEDOUT, 900U);
    assert(ret == 0);
    assert(cellular_runtime_has_failure(&runtime));
    assert(runtime.failed_state == CELLULAR_RUNTIME_STATE_CHECK_SIM);

    ret = cellular_runtime_schedule_retry(&runtime, CELLULAR_RUNTIME_STATE_CHECK_SIM, 1000U, 2000U);
    assert(ret == 0);
    assert(runtime.state == CELLULAR_RUNTIME_STATE_RETRY_WAIT);
    assert(runtime.retry_target_state == CELLULAR_RUNTIME_STATE_CHECK_SIM);
    assert(runtime.retry_count == 1U);
    assert(!cellular_runtime_retry_due(&runtime, 2999U));
    assert(cellular_runtime_retry_due(&runtime, 3000U));

    ret = cellular_runtime_enter(&runtime, runtime.retry_target_state, 3000U, 5000U);
    assert(ret == 0);
    assert(runtime.state == CELLULAR_RUNTIME_STATE_CHECK_SIM);
    assert(runtime.retry_target_state == CELLULAR_RUNTIME_STATE_NONE);

    ret = cellular_runtime_schedule_action(&runtime, 3100U, 500U);
    assert(ret == 0);
    assert(!cellular_runtime_action_due(&runtime, 3599U));
    assert(cellular_runtime_action_due(&runtime, 3600U));
    assert(cellular_runtime_get_deadline(&runtime) == 3600U);

    cellular_runtime_clear_action(&runtime, 3600U);
    assert(runtime.next_action_ms == 0U);

    cellular_runtime_clear_failure(&runtime, 3700U);
    assert(!cellular_runtime_has_failure(&runtime));

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_ONLINE, 4000U, 0U);
    assert(ret == 0);
    assert(cellular_runtime_online(&runtime));
    assert(strcmp(cellular_runtime_state_name(runtime.state), "ONLINE") == 0);

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_WAIT_SIM, 4100U, 0U);
    assert(ret == -EBUSY);

    cellular_runtime_end_session(&runtime, 4200U);
    assert(!cellular_runtime_session_active(&runtime));
    assert(!cellular_runtime_pin_attempted(&runtime));

    ret = cellular_runtime_enter(&runtime, CELLULAR_RUNTIME_STATE_WAIT_SIM, 4300U, 0U);
    assert(ret == 0);

    return 0;
}
