/**
 * @file cellular_runtime.c
 * @brief LinkG蜂窝网络内部运行状态协调实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-01
 */

#include "cellular_runtime.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断蜂窝运行状态枚举是否有效。
 */
static bool _cellular_runtime_state_valid(cellular_runtime_state_t state)
{
    switch (state)
    {
        case CELLULAR_RUNTIME_STATE_NONE:
        case CELLULAR_RUNTIME_STATE_IDLE:
        case CELLULAR_RUNTIME_STATE_WAIT_SIM:
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
        case CELLULAR_RUNTIME_STATE_ENTER_PIN:
        case CELLULAR_RUNTIME_STATE_WAIT_PIN:
        case CELLULAR_RUNTIME_STATE_WAIT_PUK:
        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
        case CELLULAR_RUNTIME_STATE_PREPARE_PDP:
        case CELLULAR_RUNTIME_STATE_ACTIVATE_PDP:
        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
        case CELLULAR_RUNTIME_STATE_START_NETDEV:
        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
        case CELLULAR_RUNTIME_STATE_ONLINE:
        case CELLULAR_RUNTIME_STATE_RETRY_WAIT:
            return true;

        default:
            return false;
    }
}

/**
 * @brief 判断指定运行状态是否要求当前存在有效SIM会话。
 */
static bool _cellular_runtime_state_requires_session(cellular_runtime_state_t state)
{
    return state != CELLULAR_RUNTIME_STATE_NONE &&
           state != CELLULAR_RUNTIME_STATE_IDLE &&
           state != CELLULAR_RUNTIME_STATE_WAIT_SIM;
}

/**
 * @brief 根据当前时间和相对时长计算绝对期限。
 */
static int _cellular_runtime_make_deadline(uint64_t now_ms, uint64_t timeout_ms, uint64_t *deadline_ms)
{
    if (deadline_ms == NULL)
    {
        return -EINVAL;
    }

    if (timeout_ms == 0U)
    {
        *deadline_ms = 0U;
        return 0;
    }

    if (now_ms > UINT64_MAX - timeout_ms)
    {
        return -ERANGE;
    }

    *deadline_ms = now_ms + timeout_ms;

    return 0;
}

/**
 * @brief 提交一次蜂窝逻辑运行状态变化。
 */
static void _cellular_runtime_commit(cellular_runtime_t *runtime, uint64_t now_ms)
{
    runtime->generation++;
    runtime->updated_ms = now_ms;
}

/**
 * @brief 清空最近一次失败记录。
 */
static void _cellular_runtime_clear_failure_fields(cellular_runtime_t *runtime)
{
    runtime->failed_state  = CELLULAR_RUNTIME_STATE_NONE;
    runtime->failure_ms    = 0U;
    runtime->last_error    = 0;
    runtime->failure_valid = false;
}

/**
 * @brief 将蜂窝运行状态初始化为IDLE。
 */
static void _cellular_runtime_initialize(cellular_runtime_t *runtime, uint64_t now_ms)
{
    memset(runtime, 0, sizeof(*runtime));

    runtime->state              = CELLULAR_RUNTIME_STATE_IDLE;
    runtime->previous_state     = CELLULAR_RUNTIME_STATE_NONE;
    runtime->failed_state       = CELLULAR_RUNTIME_STATE_NONE;
    runtime->retry_target_state = CELLULAR_RUNTIME_STATE_NONE;
    runtime->generation         = 1U;
    runtime->updated_ms         = now_ms;
    runtime->state_entered_ms   = now_ms;
}

/**
 * @brief 返回两个非零绝对期限中更早的一个。
 */
static uint64_t _cellular_runtime_min_deadline(uint64_t left, uint64_t right)
{
    if (left == 0U)
    {
        return right;
    }

    if (right == 0U)
    {
        return left;
    }

    return left < right ? left : right;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝内部逻辑运行状态。
 */
int cellular_runtime_init(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    _cellular_runtime_initialize(runtime, now_ms);

    return 0;
}

/**
 * @brief 重置蜂窝内部全部逻辑运行状态。
 */
void cellular_runtime_reset(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return;
    }

    _cellular_runtime_initialize(runtime, now_ms);
}

/**
 * @brief 开始一个新的SIM插卡会话并清空会话级策略状态。
 *
 * @note 本函数不改变当前连接状态；Owner应在WAIT_SIM阶段确认SIM已经插入后调用，
 *       再显式进入CHECK_SIM状态。重复插入通知不会重置PIN安全保护。
 */
int cellular_runtime_begin_session(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (runtime->session_active)
    {
        return -EALREADY;
    }

    if (runtime->state != CELLULAR_RUNTIME_STATE_IDLE &&
        runtime->state != CELLULAR_RUNTIME_STATE_WAIT_SIM)
    {
        return -EPROTO;
    }

    if (runtime->session_generation == UINT64_MAX)
    {
        return -EOVERFLOW;
    }

    runtime->session_generation++;
    runtime->state_attempt_count = 0U;
    runtime->retry_count         = 0U;
    runtime->pin_attempted       = false;
    runtime->session_active      = true;
    runtime->retry_target_state  = CELLULAR_RUNTIME_STATE_NONE;
    runtime->next_action_ms      = 0U;

    _cellular_runtime_clear_failure_fields(runtime);
    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 结束当前SIM插卡会话并清空会话级调度状态。
 *
 * @note 本函数不改变当前连接状态；Owner应在确认SIM已经拔出后调用，
 *       再显式进入WAIT_SIM状态。
 */
void cellular_runtime_end_session(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL || !runtime->session_active)
    {
        return;
    }

    runtime->session_active      = false;
    runtime->pin_attempted       = false;
    runtime->state_attempt_count = 0U;
    runtime->retry_count         = 0U;
    runtime->retry_target_state  = CELLULAR_RUNTIME_STATE_NONE;
    runtime->state_deadline_ms   = 0U;
    runtime->next_action_ms      = 0U;

    _cellular_runtime_commit(runtime, now_ms);
}

/****************************** 状态控制 ******************************/

/**
 * @brief 进入指定蜂窝运行状态并重新建立状态级时间边界。
 *
 * @note 重复进入当前状态会重新计算进入时间、超时时间和状态尝试次数，
 *       但不会覆盖最近一次不同状态记录。RETRY_WAIT必须通过schedule_retry进入。
 */
int cellular_runtime_enter(cellular_runtime_t *runtime, cellular_runtime_state_t state, uint64_t now_ms, uint64_t timeout_ms)
{
    uint64_t deadline_ms;
    int      ret;

    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (!_cellular_runtime_state_valid(state) ||
        state == CELLULAR_RUNTIME_STATE_NONE ||
        state == CELLULAR_RUNTIME_STATE_RETRY_WAIT)
    {
        return -EINVAL;
    }

    if (_cellular_runtime_state_requires_session(state) && !runtime->session_active)
    {
        return -ENODEV;
    }

    if (!_cellular_runtime_state_requires_session(state) && runtime->session_active)
    {
        return -EBUSY;
    }

    if (state == CELLULAR_RUNTIME_STATE_ENTER_PIN && runtime->pin_attempted)
    {
        return -EALREADY;
    }

    ret = _cellular_runtime_make_deadline(now_ms, timeout_ms, &deadline_ms);
    if (ret != 0)
    {
        return ret;
    }

    if (runtime->state != state)
    {
        runtime->previous_state = runtime->state;
    }

    runtime->state               = state;
    runtime->state_entered_ms    = now_ms;
    runtime->state_deadline_ms   = deadline_ms;
    runtime->next_action_ms      = 0U;
    runtime->state_attempt_count = 0U;
    runtime->retry_target_state  = CELLULAR_RUNTIME_STATE_NONE;

    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 进入统一重试等待状态并记录退避结束后的目标状态。
 */
int cellular_runtime_schedule_retry(cellular_runtime_t *runtime, cellular_runtime_state_t target_state, uint64_t now_ms, uint64_t retry_delay_ms)
{
    uint64_t retry_deadline_ms;
    int      ret;

    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (!runtime->session_active)
    {
        return -ENODEV;
    }

    if (!_cellular_runtime_state_valid(target_state) ||
        target_state == CELLULAR_RUNTIME_STATE_NONE ||
        target_state == CELLULAR_RUNTIME_STATE_IDLE ||
        target_state == CELLULAR_RUNTIME_STATE_WAIT_SIM ||
        target_state == CELLULAR_RUNTIME_STATE_ENTER_PIN ||
        target_state == CELLULAR_RUNTIME_STATE_ONLINE ||
        target_state == CELLULAR_RUNTIME_STATE_RETRY_WAIT)
    {
        return -EINVAL;
    }

    if (retry_delay_ms == 0U)
    {
        return -EINVAL;
    }

    if (runtime->retry_count == UINT32_MAX)
    {
        return -EOVERFLOW;
    }

    ret = _cellular_runtime_make_deadline(now_ms, retry_delay_ms, &retry_deadline_ms);
    if (ret != 0)
    {
        return ret;
    }

    if (runtime->state != CELLULAR_RUNTIME_STATE_RETRY_WAIT)
    {
        runtime->previous_state = runtime->state;
    }

    runtime->state               = CELLULAR_RUNTIME_STATE_RETRY_WAIT;
    runtime->retry_target_state  = target_state;
    runtime->state_entered_ms    = now_ms;
    runtime->state_deadline_ms   = retry_deadline_ms;
    runtime->next_action_ms      = 0U;
    runtime->state_attempt_count = 0U;
    runtime->retry_count++;

    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 安排当前状态下一次主动处理时间。
 */
int cellular_runtime_schedule_action(cellular_runtime_t *runtime, uint64_t now_ms, uint64_t delay_ms)
{
    uint64_t next_action_ms;
    int      ret;

    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (delay_ms == 0U)
    {
        return -EINVAL;
    }

    ret = _cellular_runtime_make_deadline(now_ms, delay_ms, &next_action_ms);
    if (ret != 0)
    {
        return ret;
    }

    if (runtime->next_action_ms == next_action_ms)
    {
        return 0;
    }

    runtime->next_action_ms = next_action_ms;
    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 取消当前状态已经安排的主动处理时间。
 */
void cellular_runtime_clear_action(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL || runtime->next_action_ms == 0U)
    {
        return;
    }

    runtime->next_action_ms = 0U;
    _cellular_runtime_commit(runtime, now_ms);
}

/**
 * @brief 记录当前状态已经执行一次主动动作或探测。
 */
int cellular_runtime_note_attempt(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (runtime->state_attempt_count == UINT32_MAX)
    {
        return -EOVERFLOW;
    }

    runtime->state_attempt_count++;
    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/****************************** 失败状态 ******************************/

/**
 * @brief 记录当前运行状态发生的一次可恢复失败。
 *
 * @note 本函数只记录失败，不改变当前状态，也不选择重试目标；
 *       对应失败策略由network-cell Owner显式执行。
 */
int cellular_runtime_record_failure(cellular_runtime_t *runtime, int error, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (error >= 0)
    {
        return -EINVAL;
    }

    if (!_cellular_runtime_state_valid(runtime->state) ||
        runtime->state == CELLULAR_RUNTIME_STATE_NONE)
    {
        return -EPROTO;
    }

    runtime->failed_state  = runtime->state;
    runtime->failure_ms    = now_ms;
    runtime->last_error    = error;
    runtime->failure_valid = true;

    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 清空最近一次运行状态失败记录。
 */
void cellular_runtime_clear_failure(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL || !runtime->failure_valid)
    {
        return;
    }

    _cellular_runtime_clear_failure_fields(runtime);
    _cellular_runtime_commit(runtime, now_ms);
}

/****************************** PIN控制 ******************************/

/**
 * @brief 标记当前SIM会话已经自动尝试过用户PIN。
 */
int cellular_runtime_mark_pin_attempted(cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (!runtime->session_active)
    {
        return -ENODEV;
    }

    if (runtime->state != CELLULAR_RUNTIME_STATE_ENTER_PIN)
    {
        return -EPROTO;
    }

    if (runtime->pin_attempted)
    {
        return -EALREADY;
    }

    runtime->pin_attempted = true;
    _cellular_runtime_commit(runtime, now_ms);

    return 0;
}

/****************************** 状态查询 ******************************/

/**
 * @brief 获取蜂窝运行状态的稳定日志名称。
 */
const char *cellular_runtime_state_name(cellular_runtime_state_t state)
{
    switch (state)
    {
        case CELLULAR_RUNTIME_STATE_NONE:
            return "NONE";

        case CELLULAR_RUNTIME_STATE_IDLE:
            return "IDLE";

        case CELLULAR_RUNTIME_STATE_WAIT_SIM:
            return "WAIT_SIM";

        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            return "CHECK_SIM";

        case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            return "ENTER_PIN";

        case CELLULAR_RUNTIME_STATE_WAIT_PIN:
            return "WAIT_PIN";

        case CELLULAR_RUNTIME_STATE_WAIT_PUK:
            return "WAIT_PUK";

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            return "WAIT_REGISTRATION";

        case CELLULAR_RUNTIME_STATE_PREPARE_PDP:
            return "PREPARE_PDP";

        case CELLULAR_RUNTIME_STATE_ACTIVATE_PDP:
            return "ACTIVATE_PDP";

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            return "WAIT_PDP";

        case CELLULAR_RUNTIME_STATE_START_NETDEV:
            return "START_NETDEV";

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            return "WAIT_NETDEV";
        case CELLULAR_RUNTIME_STATE_PREPARE_HOST:
            return "PREPARE_HOST";
        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            return "WAIT_HOST";

        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
            return "VERIFY_CONNECTIVITY";

        case CELLULAR_RUNTIME_STATE_ONLINE:
            return "ONLINE";

        case CELLULAR_RUNTIME_STATE_RETRY_WAIT:
            return "RETRY_WAIT";

        default:
            return "INVALID";
    }
}

/**
 * @brief 获取当前状态整体超时和下一次主动处理中的最近绝对期限。
 */
uint64_t cellular_runtime_get_deadline(const cellular_runtime_t *runtime)
{
    if (runtime == NULL)
    {
        return 0U;
    }

    return _cellular_runtime_min_deadline(runtime->state_deadline_ms, runtime->next_action_ms);
}

/**
 * @brief 判断当前运行状态是否已经超过整体等待期限。
 */
bool cellular_runtime_state_timed_out(const cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL || runtime->state_deadline_ms == 0U)
    {
        return false;
    }

    return now_ms >= runtime->state_deadline_ms;
}

/**
 * @brief 判断当前状态安排的下一次主动处理是否已经到期。
 */
bool cellular_runtime_action_due(const cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL || runtime->next_action_ms == 0U)
    {
        return false;
    }

    return now_ms >= runtime->next_action_ms;
}

/**
 * @brief 判断统一重试退避是否已经结束。
 */
bool cellular_runtime_retry_due(const cellular_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime == NULL || runtime->state != CELLULAR_RUNTIME_STATE_RETRY_WAIT)
    {
        return false;
    }

    if (!runtime->session_active || runtime->retry_target_state == CELLULAR_RUNTIME_STATE_NONE)
    {
        return false;
    }

    return cellular_runtime_state_timed_out(runtime, now_ms);
}

/**
 * @brief 判断蜂窝连接状态是否已经进入验证成功的维护阶段。
 */
bool cellular_runtime_online(const cellular_runtime_t *runtime)
{
    return runtime != NULL &&
           runtime->session_active &&
           runtime->state == CELLULAR_RUNTIME_STATE_ONLINE;
}

/**
 * @brief 判断当前是否存在一个已确认SIM插卡会话。
 */
bool cellular_runtime_session_active(const cellular_runtime_t *runtime)
{
    return runtime != NULL && runtime->session_active;
}

/**
 * @brief 判断是否存在最近一次有效运行状态失败记录。
 */
bool cellular_runtime_has_failure(const cellular_runtime_t *runtime)
{
    return runtime != NULL && runtime->failure_valid;
}

/**
 * @brief 判断当前SIM会话是否已经自动尝试过用户PIN。
 */
bool cellular_runtime_pin_attempted(const cellular_runtime_t *runtime)
{
    return runtime != NULL &&
           runtime->session_active &&
           runtime->pin_attempted;
}
