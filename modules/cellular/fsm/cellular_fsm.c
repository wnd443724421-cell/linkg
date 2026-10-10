/**
 * @file cellular_fsm.c
 * @brief LinkG蜂窝连接状态机核心调度及资源恢复实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-10
 */

#include "cellular_fsm.h"

#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "rg255_cmd.h"
#include "rg255_query.h"

#include "cellular_internal.h"
#include "cellular_fsm_internal.h"
#include "cellular_health.h"

/****************************** 状态机常量 ******************************/

#define CELLULAR_FSM_CHECK_SIM_TIMEOUT_MS    15000U  // SIM初始化状态收敛超时
#define CELLULAR_FSM_REGISTRATION_TIMEOUT_MS 120000U // 移动网络注册等待超时
#define CELLULAR_FSM_PDP_TIMEOUT_MS          30000U  // PDP激活确认等待超时
#define CELLULAR_FSM_NETDEV_TIMEOUT_MS       30000U  // USB网络设备连接等待超时
#define CELLULAR_FSM_HOST_TIMEOUT_MS         30000U  // Linux Host网络配置收敛超时
#define CELLULAR_FSM_RETRY_BASE_MS           5000U   // 连接失败首次退避时间
#define CELLULAR_FSM_RETRY_MAX_MS            60000U  // 连接失败最大退避时间

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化蜂窝状态机运行上下文。
 */
int cellular_fsm_init(cellular_fsm_t *fsm, uint64_t now_ms)
{
    if (fsm == NULL)
    {
        return -EINVAL;
    }

    memset(fsm, 0, sizeof(*fsm));
    return cellular_runtime_init(&fsm->runtime, now_ms);
}

/**
 * @brief 重置蜂窝状态机运行上下文。
 */
void cellular_fsm_reset(cellular_fsm_t *fsm, uint64_t now_ms)
{
    if (fsm == NULL)
    {
        return;
    }

    cellular_runtime_reset(&fsm->runtime, now_ms);

    fsm->requested_refresh     = CELLULAR_STATUS_REFRESH_NONE;
    fsm->pdp_action_started    = false;
    fsm->netdev_action_started = false;
}

/****************************** Owner调度 ******************************/

/**
 * @brief 获取状态机最近的Owner处理期限。
 */
uint64_t cellular_fsm_get_deadline(const cellular_fsm_t *fsm)
{
    if (fsm == NULL)
    {
        return 0U;
    }

    return cellular_runtime_get_deadline(&fsm->runtime);
}

/**
 * @brief 取得并清空状态机下一轮事实刷新请求。
 */
cellular_status_refresh_mask_t cellular_fsm_take_requested_refresh(cellular_fsm_t *fsm)
{
    cellular_status_refresh_mask_t requested;

    if (fsm == NULL)
    {
        return CELLULAR_STATUS_REFRESH_NONE;
    }

    requested              = fsm->requested_refresh;
    fsm->requested_refresh = CELLULAR_STATUS_REFRESH_NONE;

    return requested;
}

/****************************** 内部辅助 ******************************/

/**
 * @brief 记录清理过程中出现的首个错误。
 */
static void _cellular_fsm_record_first_error(int *first_error, int error)
{
    if (first_error == NULL)
    {
        return;
    }

    if (*first_error == 0 && error != 0)
    {
        *first_error = error;
    }
}

/**
 * @brief 判断事实元数据是否表示最近一次确认成功。
 */
bool _cellular_fsm_meta_current(const cellular_status_meta_t *meta)
{
    return meta != NULL && meta->confirmed && meta->last_error == 0;
}

/**
 * @brief 判断AT动作错误是否表示控制基础设施已经无法继续运行。
 */
bool _cellular_fsm_action_error_fatal(int error)
{
    switch (error)
    {
        case -EINVAL:
        case -EMSGSIZE:
        case -EDEADLK:
        case -EBADF:
        case -ENODEV:
        case -ECANCELED:
            return true;

        default:
            return false;
    }
}

/****************************** 状态结果辅助 ******************************/

/**
 * @brief 构造保持当前运行状态的处理结果。
 */
cellular_fsm_step_t _cellular_fsm_step_wait(void)
{
    cellular_fsm_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_WAIT;

    return step;
}

/**
 * @brief 构造推进到指定运行状态的处理结果。
 */
cellular_fsm_step_t _cellular_fsm_step_done(cellular_runtime_state_t next_state)
{
    cellular_fsm_step_t step;

    memset(&step, 0, sizeof(step));
    step.result     = CELLULAR_RUNTIME_STEP_DONE;
    step.next_state = next_state;

    return step;
}

/**
 * @brief 构造交由Owner失败策略处理的状态结果。
 */
cellular_fsm_step_t _cellular_fsm_step_failed(int error)
{
    cellular_fsm_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_FAILED;
    if (error < 0)
    {
        step.error = error;
    }
    else
    {
        step.error = -EIO;
    }

    return step;
}

/**
 * @brief 构造要求Owner结束运行的致命状态结果。
 */
cellular_fsm_step_t _cellular_fsm_step_fatal(int error)
{
    cellular_fsm_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_FATAL;
    if (error < 0)
    {
        step.error = error;
    }
    else
    {
        step.error = -EIO;
    }

    return step;
}

/****************************** 运行状态控制 ******************************/

/**
 * @brief 获取指定运行状态的整体等待超时。
 */
static uint64_t _cellular_fsm_state_timeout(cellular_runtime_state_t state)
{
    switch (state)
    {
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            return CELLULAR_FSM_CHECK_SIM_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            return CELLULAR_FSM_REGISTRATION_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            return CELLULAR_FSM_PDP_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            return CELLULAR_FSM_NETDEV_TIMEOUT_MS;

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            return CELLULAR_FSM_HOST_TIMEOUT_MS;

        default:
            return 0U;
    }
}

/**
 * @brief 安排Owner下一轮定向确认指定状态事实。
 */
void _cellular_fsm_request_refresh(cellular_fsm_t *fsm, cellular_status_refresh_mask_t requested)
{
    if (fsm == NULL)
    {
        return;
    }

    fsm->requested_refresh |= requested;
}

/**
 * @brief 为新进入的状态安排立即需要确认的事实。
 */
static void _cellular_fsm_request_state_refresh(cellular_fsm_t *fsm, cellular_runtime_state_t state)
{
    switch (state)
    {
        case CELLULAR_RUNTIME_STATE_WAIT_SIM:
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
        case CELLULAR_RUNTIME_STATE_WAIT_PIN:
        case CELLULAR_RUNTIME_STATE_WAIT_PUK:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_SIM);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_REGISTRATION | CELLULAR_STATUS_REFRESH_RADIO);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP | CELLULAR_STATUS_REFRESH_PDP_ADDRESS);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_NETDEV | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK | CELLULAR_STATUS_REFRESH_HOST);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP_ADDRESS | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK | CELLULAR_STATUS_REFRESH_HOST);
            break;

        case CELLULAR_RUNTIME_STATE_ONLINE:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_SIM | CELLULAR_STATUS_REFRESH_REGISTRATION |
                                               CELLULAR_STATUS_REFRESH_PDP | CELLULAR_STATUS_REFRESH_NETDEV |
                                               CELLULAR_STATUS_REFRESH_HOST);
            break;

        default:
            break;
    }
}

/**
 * @brief 显式进入指定运行状态并建立该状态的时间边界。
 */
int cellular_fsm_enter(cellular_fsm_t *fsm, cellular_runtime_state_t state, uint64_t now_ms)
{
    cellular_runtime_state_t previous;
    uint64_t                 timeout_ms;
    int                      ret;

    if (fsm == NULL)
    {
        return -EINVAL;
    }

    previous   = fsm->runtime.state;
    timeout_ms = _cellular_fsm_state_timeout(state);

    ret = cellular_runtime_enter(&fsm->runtime, state, now_ms, timeout_ms);
    if (ret != 0)
    {
        return ret;
    }

    if (state == CELLULAR_RUNTIME_STATE_ONLINE)
    {
        cellular_runtime_clear_failure(&fsm->runtime, now_ms);
    }

    _cellular_fsm_request_state_refresh(fsm, state);

    if (previous != state)
    {
        CELLULAR_DEBUG("runtime state changed, old=%s, new=%s", cellular_runtime_state_name(previous), cellular_runtime_state_name(state));

        if (state == CELLULAR_RUNTIME_STATE_ONLINE)
        {
            CELLULAR_INFO("data link online");
        }
    }

    return 0;
}

/****************************** 数据会话清理 ******************************/

/**
 * @brief 按Host、QNETDEV、PDP的逆序尽力清理当前数据会话。
 *
 * @note 不结束SIM会话；AT通道不可用时仍清理Host并请求状态刷新。
 */
int _cellular_fsm_cleanup_data_session(cellular_fsm_t *fsm, at_channel_t *channel)
{
    uint8_t pdp_cid;
    bool    pdp_cid_valid;
    int     first_error;
    int     ret;

    if (fsm == NULL)
    {
        return -EINVAL;
    }

    first_error   = 0;
    pdp_cid_valid = cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid);

    _cellular_fsm_cleanup_host_network();

    if ((fsm->netdev_action_started || fsm->pdp_action_started) && !pdp_cid_valid)
    {
        CELLULAR_WARN("data cleanup skipped without selected PDP CID");
        _cellular_fsm_record_first_error(&first_error, -EPROTO);
    }

    if ((fsm->netdev_action_started || fsm->pdp_action_started) && pdp_cid_valid && channel == NULL)
    {
        CELLULAR_WARN("data cleanup skipped Modem commands without AT channel");
        _cellular_fsm_record_first_error(&first_error, -ENODEV);
    }

    if (fsm->netdev_action_started && pdp_cid_valid && channel != NULL)
    {
        ret = rg255_cmd_stop_netdev(channel, pdp_cid);
        if (ret != 0)
        {
            CELLULAR_WARN("stop QNETDEV failed, error=%d", ret);
            _cellular_fsm_record_first_error(&first_error, ret);
        }
    }

    if (fsm->pdp_action_started && pdp_cid_valid && channel != NULL)
    {
        ret = rg255_cmd_set_pdp_active(channel, pdp_cid, false);
        if (ret != 0)
        {
            CELLULAR_WARN("deactivate PDP failed, error=%d", ret);
            _cellular_fsm_record_first_error(&first_error, ret);
        }
    }

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP | CELLULAR_STATUS_REFRESH_PDP_ADDRESS |
                                         CELLULAR_STATUS_REFRESH_NETDEV | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                         CELLULAR_STATUS_REFRESH_HOST);

    return first_error;
}

/****************************** 公共状态步骤 ******************************/

/**
 * @brief 处理IDLE状态并进入SIM等待流程。
 */
static cellular_fsm_step_t _cellular_fsm_state_idle(void)
{
    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_SIM);
}

/**
 * @brief 等待统一连接重试退避期限到达。
 */
static cellular_fsm_step_t _cellular_fsm_state_retry_wait(const cellular_fsm_t *fsm, uint64_t now_ms)
{
    if (!cellular_runtime_retry_due(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_wait();
    }

    return _cellular_fsm_step_done(fsm->runtime.retry_target_state);
}

/**
 * @brief 执行当前运行状态的一步处理。
 */
cellular_fsm_step_t cellular_fsm_run(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, const cellular_status_info_t *info, uint64_t now_ms)
{
    if (fsm == NULL || config == NULL)
    {
        return _cellular_fsm_step_fatal(-EINVAL);
    }

    switch (fsm->runtime.state)
    {
        case CELLULAR_RUNTIME_STATE_IDLE:
            return _cellular_fsm_state_idle();

        case CELLULAR_RUNTIME_STATE_WAIT_SIM:
            return _cellular_fsm_state_wait_sim(fsm);

        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            return _cellular_fsm_state_check_sim(fsm, config, info, now_ms);

        case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            return _cellular_fsm_state_enter_pin(fsm, config, channel, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_PIN:
            return _cellular_fsm_state_wait_pin(info);

        case CELLULAR_RUNTIME_STATE_WAIT_PUK:
            return _cellular_fsm_state_wait_puk(info);

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            return _cellular_fsm_state_wait_registration(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_PREPARE_PDP:
            return _cellular_fsm_state_prepare_pdp(fsm, config, channel, now_ms);

        case CELLULAR_RUNTIME_STATE_ACTIVATE_PDP:
            return _cellular_fsm_state_activate_pdp(fsm, channel, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            return _cellular_fsm_state_wait_pdp(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_START_NETDEV:
            return _cellular_fsm_state_start_netdev(fsm, channel, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            return _cellular_fsm_state_wait_netdev(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_PREPARE_HOST:
            return _cellular_fsm_state_prepare_host(fsm, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            return _cellular_fsm_state_wait_host(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_ONLINE:
            return _cellular_fsm_state_online(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_RETRY_WAIT:
            return _cellular_fsm_state_retry_wait(fsm, now_ms);

        case CELLULAR_RUNTIME_STATE_NONE:
        default:
            return _cellular_fsm_step_fatal(-EPROTO);
    }
}

/****************************** 失败恢复 ******************************/

/**
 * @brief 根据当前SIM会话累计重试次数计算有上限退避时间。
 */
static uint64_t _cellular_fsm_retry_delay(const cellular_fsm_t *fsm)
{
    uint64_t delay_ms;
    uint32_t shift;

    shift = fsm->runtime.retry_count;
    if (shift > 4U)
    {
        shift = 4U;
    }

    delay_ms = CELLULAR_FSM_RETRY_BASE_MS << shift;
    if (delay_ms > CELLULAR_FSM_RETRY_MAX_MS)
    {
        delay_ms = CELLULAR_FSM_RETRY_MAX_MS;
    }

    return delay_ms;
}

/**
 * @brief 检查当前选中CID是否仍可安全操作，不允许关闭其他CID的QNETDEV。
 *
 * @note 查询失败视为状态不明，禁止破坏性恢复。
 */
static int _cellular_fsm_recovery_netdev_identity(cellular_fsm_t *fsm, at_channel_t *channel, uint8_t *pdp_cid, bool *connected)
{
    rg255_netdev_status_t status;
    int                   ret;

    if (channel == NULL || pdp_cid == NULL || connected == NULL)
    {
        return -ENODEV;
    }

    if (!cellular_runtime_get_pdp_cid(&fsm->runtime, pdp_cid))
    {
        return -EPROTO;
    }

    memset(&status, 0, sizeof(status));
    ret = rg255_query_netdev_status(channel, &status);
    if (ret != 0)
    {
        return ret;
    }

    if (status.connected && status.cid != *pdp_cid)
    {
        CELLULAR_WARN("recovery blocked by different QNETDEV CID, current=%u, selected=%u", (unsigned int)status.cid, (unsigned int)*pdp_cid);
        return -EBUSY;
    }

    *connected = status.connected;
    return 0;
}

/**
 * @brief 双栈健康失效后的第二级恢复：只重建选中CID的QNETDEV。
 *
 * @note 禁止在无法确认QNETDEV身份的情况下发送停止命令。
 */
static int _cellular_fsm_recover_netdev(cellular_fsm_t *fsm, at_channel_t *channel)
{
    uint8_t pdp_cid;
    bool    connected;
    int     ret;

    ret = _cellular_fsm_recovery_netdev_identity(fsm, channel, &pdp_cid, &connected);
    if (ret != 0)
    {
        return ret;
    }

    if (connected)
    {
        ret = rg255_cmd_stop_netdev(channel, pdp_cid);
        if (ret != 0)
        {
            CELLULAR_WARN("restart QNETDEV stop command returned error=%d", ret);
            return ret;
        }
    }

    _cellular_fsm_cleanup_host_network();
    fsm->netdev_action_started = false;
    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_NETDEV | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK | CELLULAR_STATUS_REFRESH_HOST);

    CELLULAR_WARN("restarting selected QNETDEV, cid=%u", (unsigned int)pdp_cid);
    return 0;
}

/**
 * @brief 双栈健康失效后的第三级恢复：重建当前选中CID的PDP数据会话。
 *
 * @note 只能由Health确认双地址族多轮不可达后调用，不能修改其他CID。
 */
static int _cellular_fsm_rebuild_pdp(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    uint8_t pdp_cid;
    bool    connected;
    int     ret;

    if (!cellular_health_dual_unreachable())
    {
        return -EAGAIN;
    }

    ret = _cellular_fsm_recovery_netdev_identity(fsm, channel, &pdp_cid, &connected);
    if (ret != 0)
    {
        return ret;
    }

    if (connected)
    {
        ret = rg255_cmd_stop_netdev(channel, pdp_cid);
        if (ret != 0)
        {
            return ret;
        }
    }

    ret = rg255_cmd_set_pdp_active(channel, pdp_cid, false);
    if (ret != 0)
    {
        return ret;
    }

    _cellular_fsm_cleanup_host_network();
    cellular_status_clear_pdp_context();
    cellular_runtime_clear_pdp_cid(&fsm->runtime, now_ms);
    fsm->netdev_action_started = false;
    fsm->pdp_action_started    = false;

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP | CELLULAR_STATUS_REFRESH_PDP_ADDRESS |
                                         CELLULAR_STATUS_REFRESH_NETDEV | CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                         CELLULAR_STATUS_REFRESH_HOST);

    CELLULAR_WARN("controlled PDP rebuild requested, cid=%u", (unsigned int)pdp_cid);
    return 0;
}

/**
 * @brief 根据失败状态执行局部恢复；只有健康模块确认双栈不可达才允许重建PDP。
 */
int cellular_fsm_handle_failure(cellular_fsm_t *fsm, at_channel_t *channel, int error, uint64_t now_ms)
{
    cellular_runtime_state_t failed_state;
    cellular_runtime_state_t retry_target;
    uint64_t                 retry_delay_ms;
    int                      ret;

    if (fsm == NULL)
    {
        return -EINVAL;
    }

    ret = cellular_runtime_record_failure(&fsm->runtime, error, now_ms);
    if (ret != 0)
    {
        return ret;
    }

    failed_state = fsm->runtime.failed_state;
    CELLULAR_WARN("runtime state failed, state=%s, error=%d", cellular_runtime_state_name(failed_state), error);

    switch (failed_state)
    {
        case CELLULAR_RUNTIME_STATE_CHECK_SIM:
            retry_target = CELLULAR_RUNTIME_STATE_CHECK_SIM;
            break;

        case CELLULAR_RUNTIME_STATE_ENTER_PIN:
            return cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_WAIT_PIN, now_ms);

        case CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION:
            retry_target = CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION;
            break;

        case CELLULAR_RUNTIME_STATE_PREPARE_PDP:
        case CELLULAR_RUNTIME_STATE_ACTIVATE_PDP:
        case CELLULAR_RUNTIME_STATE_WAIT_PDP:
            retry_target = CELLULAR_RUNTIME_STATE_PREPARE_PDP;
            break;

        case CELLULAR_RUNTIME_STATE_START_NETDEV:
        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
            retry_target = CELLULAR_RUNTIME_STATE_START_NETDEV;
            break;

        case CELLULAR_RUNTIME_STATE_PREPARE_HOST:
        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            retry_target = CELLULAR_RUNTIME_STATE_PREPARE_HOST;
            break;

        case CELLULAR_RUNTIME_STATE_ONLINE:
            if (error == -ENOLINK)
            {
                if (!cellular_health_dual_unreachable())
                {
                    return cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_ONLINE, now_ms);
                }

                ret = _cellular_fsm_recover_netdev(fsm, channel);
                if (ret != 0)
                {
                    CELLULAR_WARN("QNETDEV recovery deferred, error=%d", ret);
                    return cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_ONLINE, now_ms);
                }

                retry_target = CELLULAR_RUNTIME_STATE_START_NETDEV;
            }
            else if (error == -ENETUNREACH)
            {
                ret = _cellular_fsm_rebuild_pdp(fsm, channel, now_ms);
                if (ret != 0)
                {
                    CELLULAR_WARN("PDP recovery deferred, error=%d", ret);
                    return cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_ONLINE, now_ms);
                }

                retry_target = CELLULAR_RUNTIME_STATE_PREPARE_PDP;
            }
            else if (error == -EBUSY)
            {
                retry_target = CELLULAR_RUNTIME_STATE_START_NETDEV;
            }
            else
            {
                retry_target = CELLULAR_RUNTIME_STATE_PREPARE_HOST;
            }
            break;

        default:
            return -EPROTO;
    }

    retry_delay_ms = _cellular_fsm_retry_delay(fsm);
    ret = cellular_runtime_schedule_retry(&fsm->runtime, retry_target, now_ms, retry_delay_ms);
    if (ret != 0)
    {
        return ret;
    }

    CELLULAR_DEBUG("retry scheduled, target=%s, delay_ms=%llu, count=%u", cellular_runtime_state_name(retry_target), (unsigned long long)retry_delay_ms, (unsigned int)fsm->runtime.retry_count);
    return 0;
}

/****************************** 会话停止 ******************************/

/**
 * @brief 停止当前SIM数据会话并清理状态机运行标志。
 */
int cellular_fsm_stop_session(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    int cleanup_ret;

    if (fsm == NULL)
    {
        return -EINVAL;
    }

    cleanup_ret = 0;

    if (cellular_runtime_session_active(&fsm->runtime))
    {
        cleanup_ret = _cellular_fsm_cleanup_data_session(fsm, channel);
        cellular_runtime_end_session(&fsm->runtime, now_ms);
    }

    cellular_status_clear_pdp_context();
    fsm->pdp_action_started    = false;
    fsm->netdev_action_started = false;

    return cleanup_ret;
}
