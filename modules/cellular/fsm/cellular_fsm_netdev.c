/**
 * @file cellular_fsm_netdev.c
 * @brief LinkG蜂窝QNETDEV连接状态处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_fsm_internal.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "rg255_cmd.h"
#include "rg255_query.h"

#include "cellular_internal.h"

/****************************** NETDEV状态步骤 ******************************/

/**
 * @brief 确认RG255 QNETDEV绑定状态并按需请求建立目标CID连接。
 *
 * @note 已连接目标CID时直接复用；连接其他CID时不执行覆盖或断开操作。
 */
cellular_fsm_step_t _cellular_fsm_state_start_netdev(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    rg255_netdev_status_t status;
    uint8_t               pdp_cid;
    int                   ret;

    if (fsm == NULL)
    {
        return _cellular_fsm_step_fatal(-EINVAL);
    }

    if (channel == NULL)
    {
        return _cellular_fsm_step_fatal(-ENODEV);
    }

    ret = cellular_runtime_note_attempt(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    if (!cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid))
    {
        return _cellular_fsm_step_fatal(-EPROTO);
    }

    memset(&status, 0, sizeof(status));
    status.type = RG255_NETDEV_TYPE_UNKNOWN;

    ret = rg255_query_netdev_status(channel, &status);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        CELLULAR_WARN("query QNETDEV before start failed, error=%d", ret);
        return _cellular_fsm_step_failed(ret);
    }

    if (status.connected)
    {
        if (status.cid != pdp_cid)
        {
            CELLULAR_WARN("QNETDEV CID conflict, connected=%u, selected=%u", (unsigned int)status.cid, (unsigned int)pdp_cid);
            return _cellular_fsm_step_failed(-EBUSY);
        }

        CELLULAR_DEBUG("QNETDEV already connected, cid=%u", (unsigned int)pdp_cid);
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_NETDEV);
    }

    fsm->netdev_action_started = true;

    ret = rg255_cmd_start_netdev(channel, pdp_cid);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        CELLULAR_WARN("start QNETDEV returned error=%d, action=query-truth", ret);
    }
    else
    {
        CELLULAR_DEBUG("QNETDEV start requested, cid=%u", (unsigned int)pdp_cid);
    }

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_NETDEV);
}

/**
 * @brief 等待Status确认USB网络设备已经连接到当前所选CID。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_netdev(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    uint8_t pdp_cid;
    int     error;

    if (fsm == NULL)
    {
        return _cellular_fsm_step_fatal(-EINVAL);
    }

    if (!cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid))
    {
        return _cellular_fsm_step_fatal(-EPROTO);
    }

    if (info != NULL && _cellular_fsm_meta_current(&info->netdev.state.meta) && info->netdev.state.connected && info->netdev.state.cid == pdp_cid)
    {
        CELLULAR_DEBUG("QNETDEV connection confirmed, cid=%u", (unsigned int)pdp_cid);
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_PREPARE_HOST);
    }

    if (!cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_wait();
    }

    error = -ETIMEDOUT;
    if (info != NULL && info->netdev.state.meta.last_error != 0)
    {
        error = info->netdev.state.meta.last_error;
    }

    return _cellular_fsm_step_failed(error);
}
