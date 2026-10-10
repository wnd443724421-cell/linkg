/**
 * @file cellular_fsm_registration.c
 * @brief LinkG蜂窝网络注册状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_fsm_internal.h"

#include <errno.h>
#include <stdint.h>

#include "cellular_internal.h"

/****************************** 网络注册状态步骤 ******************************/

/**
 * @brief 等待移动网络注册成功。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_registration(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info != NULL && _cellular_fsm_meta_current(&info->network.registration_meta))
    {
        if (info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
        {
            CELLULAR_DEBUG("network registration confirmed");
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_PREPARE_PDP);
        }

        if (info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_DENIED)
        {
            return _cellular_fsm_step_failed(-EACCES);
        }
    }

    if (!cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_wait();
    }

    error = -ETIMEDOUT;
    if (info != NULL && info->network.registration_meta.last_error != 0)
    {
        error = info->network.registration_meta.last_error;
    }

    return _cellular_fsm_step_failed(error);
}
