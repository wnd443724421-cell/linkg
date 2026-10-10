/**
 * @file cellular_fsm_sim.c
 * @brief LinkG蜂窝SIM插拔及PIN会话状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_fsm_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "rg255_cmd.h"

#include "cellular_internal.h"

/****************************** SIM会话协调 ******************************/

/**
 * @brief 处理SIM拔出，尽力清理数据会话并进入WAIT_SIM。
 *
 * @note 数据清理失败仅记录日志；状态迁移失败必须上报Owner。
 */
static int _cellular_fsm_handle_sim_removed(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    int cleanup_ret;
    int ret;

    if (cellular_runtime_session_active(&fsm->runtime))
    {
        cleanup_ret = _cellular_fsm_cleanup_data_session(fsm, channel);
        if (cleanup_ret != 0)
        {
            CELLULAR_WARN("cleanup after SIM removal returned error=%d", cleanup_ret);
        }

        cellular_runtime_end_session(&fsm->runtime, now_ms);
    }

    cellular_status_clear_pdp_context();
    fsm->pdp_action_started    = false;
    fsm->netdev_action_started = false;

    if (fsm->runtime.state != CELLULAR_RUNTIME_STATE_WAIT_SIM)
    {
        ret = cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_WAIT_SIM, now_ms);
        if (ret != 0)
        {
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 根据最新SIM事实开始或结束物理插卡会话。
 *
 * @return 1表示本轮发生了强制状态转移，0表示未转移，负值表示错误。
 */
int cellular_fsm_sync_sim_session(cellular_fsm_t *fsm, at_channel_t *channel, const cellular_monitor_events_t *events, const cellular_status_info_t *info, uint64_t now_ms)
{
    linkg_cellular_sim_state_t sim_state;
    bool                       physical_removed;
    int                        ret;

    if (fsm == NULL)
    {
        return -EINVAL;
    }

    physical_removed = events != NULL &&
                       (events->mask & CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED) != 0U &&
                       events->sim_presence_valid &&
                       events->sim_presence == CELLULAR_MONITOR_SIM_PRESENCE_REMOVED;

    sim_state = LINKG_CELLULAR_SIM_STATE_UNKNOWN;
    if (info != NULL && _cellular_fsm_meta_current(&info->local.sim_meta))
    {
        sim_state = info->local.sim_state;
    }

    if (physical_removed || sim_state == LINKG_CELLULAR_SIM_STATE_ABSENT)
    {
        bool session_active;
        bool state_changed;

        session_active = cellular_runtime_session_active(&fsm->runtime);
        state_changed  = session_active || fsm->runtime.state != CELLULAR_RUNTIME_STATE_WAIT_SIM;

        ret = _cellular_fsm_handle_sim_removed(fsm, channel, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        if (session_active)
        {
            CELLULAR_INFO("SIM removed, session ended");
        }

        return state_changed ? 1 : 0;
    }

    if (!cellular_runtime_session_active(&fsm->runtime) && sim_state != LINKG_CELLULAR_SIM_STATE_UNKNOWN)
    {
        ret = cellular_runtime_begin_session(&fsm->runtime, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        cellular_status_clear_pdp_context();
        fsm->pdp_action_started    = false;
        fsm->netdev_action_started = false;

        ret = cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_CHECK_SIM, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        CELLULAR_INFO("SIM session started");

        return 1;
    }

    if (cellular_runtime_session_active(&fsm->runtime) && sim_state != LINKG_CELLULAR_SIM_STATE_UNKNOWN && sim_state != LINKG_CELLULAR_SIM_STATE_READY)
    {
        if (fsm->runtime.state == CELLULAR_RUNTIME_STATE_RETRY_WAIT &&
            fsm->runtime.retry_target_state == CELLULAR_RUNTIME_STATE_CHECK_SIM)
        {
            return 0;
        }
        switch (fsm->runtime.state)
        {
            case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            case CELLULAR_RUNTIME_STATE_WAIT_PIN:
            case CELLULAR_RUNTIME_STATE_WAIT_PUK:
                break;

            default:
                ret = cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_CHECK_SIM, now_ms);
                if (ret != 0)
                {
                    return ret;
                }

                return 1;
        }
    }

    return 0;
}

/****************************** SIM状态步骤 ******************************/

/**
 * @brief 处理等待SIM插入状态。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_sim(const cellular_fsm_t *fsm)
{
    if (cellular_runtime_session_active(&fsm->runtime))
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_CHECK_SIM);
    }

    return _cellular_fsm_step_wait();
}

/**
 * @brief 根据CPIN事实决定SIM会话后续流程。
 */
cellular_fsm_step_t _cellular_fsm_state_check_sim(const cellular_fsm_t *fsm, const linkg_cellular_config_t *config, const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info == NULL || !_cellular_fsm_meta_current(&info->local.sim_meta))
    {
        if (cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
        {
            error = -ETIMEDOUT;
            if (info != NULL && info->local.sim_meta.last_error != 0)
            {
                error = info->local.sim_meta.last_error;
            }

            return _cellular_fsm_step_failed(error);
        }

        return _cellular_fsm_step_wait();
    }

    switch (info->local.sim_state)
    {
        case LINKG_CELLULAR_SIM_STATE_READY:
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);

        case LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED:
            if (config->pin[0] == '\0' || cellular_runtime_pin_attempted(&fsm->runtime))
            {
                return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
            }

            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_ENTER_PIN);

        case LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED:
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PUK);

        case LINKG_CELLULAR_SIM_STATE_NOT_READY:
        case LINKG_CELLULAR_SIM_STATE_UNKNOWN:
        default:
            if (cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
            {
                return _cellular_fsm_step_failed(-ETIMEDOUT);
            }

            return _cellular_fsm_step_wait();
    }
}

/**
 * @brief 在当前物理插卡会话中安全执行一次用户PIN输入。
 */
cellular_fsm_step_t _cellular_fsm_state_enter_pin(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms)
{
    int ret;

    if (channel == NULL)
    {
        return _cellular_fsm_step_fatal(-ENODEV);
    }

    if (config->pin[0] == '\0')
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
    }

    ret = cellular_runtime_mark_pin_attempted(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        if (ret == -EALREADY)
        {
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
        }

        return _cellular_fsm_step_fatal(ret);
    }

    ret = cellular_runtime_note_attempt(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    ret = rg255_cmd_enter_pin(channel, config->pin);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        CELLULAR_WARN("enter SIM PIN returned error=%d, action=query-truth", ret);
    }
    else
    {
        CELLULAR_DEBUG("SIM PIN submitted, action=query-truth");
    }

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_SIM);

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_CHECK_SIM);
}

/**
 * @brief 处理等待用户提供正确PIN或更换SIM状态。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_pin(const cellular_status_info_t *info)
{
    if (info == NULL || !_cellular_fsm_meta_current(&info->local.sim_meta))
    {
        return _cellular_fsm_step_wait();
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_READY)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_PUK_REQUIRED)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PUK);
    }

    return _cellular_fsm_step_wait();
}

/**
 * @brief 处理等待用户人工解除SIM PUK状态。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_puk(const cellular_status_info_t *info)
{
    if (info == NULL || !_cellular_fsm_meta_current(&info->local.sim_meta))
    {
        return _cellular_fsm_step_wait();
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_READY)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION);
    }

    if (info->local.sim_state == LINKG_CELLULAR_SIM_STATE_PIN_REQUIRED)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PIN);
    }

    return _cellular_fsm_step_wait();
}
