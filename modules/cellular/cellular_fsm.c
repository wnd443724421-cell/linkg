/**
 * @file cellular_fsm.c
 * @brief LinkG蜂窝网络连接状态机实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-02
 */

#include "cellular_fsm.h"

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "linkg_log.h"
#include "linkg_network_ops.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"
#include "linkg_time.h"

#include "rg255_cmd.h"
#include "rg255_query.h"

/****************************** 状态机常量 ******************************/

#define CELLULAR_FSM_VERIFY_TARGET                 "www.baidu.com" // V1公网连通性验证目标
#define CELLULAR_FSM_CHECK_SIM_TIMEOUT_MS          15000U          // SIM初始化状态收敛超时
#define CELLULAR_FSM_REGISTRATION_TIMEOUT_MS       120000U         // 移动网络注册等待超时
#define CELLULAR_FSM_PDP_TIMEOUT_MS                30000U          // PDP激活确认等待超时
#define CELLULAR_FSM_NETDEV_TIMEOUT_MS             30000U          // USB网络设备连接等待超时
#define CELLULAR_FSM_HOST_TIMEOUT_MS               30000U          // Linux Host网络配置收敛超时
#define CELLULAR_FSM_VERIFY_TIMEOUT_MS             30000U          // 公网连通性验证整体超时
#define CELLULAR_FSM_VERIFY_RETRY_MS               2000U           // 单次公网探测失败后的重试间隔
#define CELLULAR_FSM_VERIFY_NEXT_FAMILY_DELAY_MS   1U              // IPv4成功后进入IPv6验证的调度间隔
#define CELLULAR_FSM_VERIFY_FAILURE_LIMIT          3U              // 单个地址族连续公网探测失败阈值
#define CELLULAR_FSM_ONLINE_VERIFY_INTERVAL_MS     30000U          // 在线状态公网复核周期
#define CELLULAR_FSM_RETRY_BASE_MS                 5000U           // 连接失败首次退避时间
#define CELLULAR_FSM_RETRY_MAX_MS                  60000U          // 连接失败最大退避时间

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
    fsm->requested_refresh      = CELLULAR_STATUS_REFRESH_NONE;
    fsm->pdp_action_started     = false;
    fsm->netdev_action_started  = false;
    fsm->verify_ipv4_done       = false;
    fsm->verify_ipv6_done       = false;
    fsm->verify_failure_count   = 0U;
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
static bool _cellular_fsm_meta_current(const cellular_status_meta_t *meta)
{
    return meta != NULL && meta->confirmed && meta->last_error == 0;
}

/**
 * @brief 判断AT动作错误是否表示控制基础设施已经无法继续运行。
 */
static bool _cellular_fsm_action_error_fatal(int error)
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

/**
 * @brief 判断两个APN是否大小写无关地完全一致。
 */
static bool _cellular_fsm_apn_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcasecmp(left, right) == 0;
}

/**
 * @brief 判断PDP配置是否属于必须保护的IMS上下文。
 */
static bool _cellular_fsm_pdp_is_ims(const rg255_pdp_config_t *config)
{
    return config != NULL && _cellular_fsm_apn_equal(config->apn, "ims");
}

/**
 * @brief 从PDP事实表中选择现有数据上下文或安全的空闲CID。
 *
 * 明确配置APN时优先复用同APN双栈上下文，否则返回第一个未占用CID用于创建。
 * 未配置APN时只允许复用唯一的非IMS双栈上下文。
 */
static int _cellular_fsm_select_pdp_context(const rg255_pdp_config_t *configs,
                                              size_t count,
                                              const char *required_apn,
                                              uint8_t *selected_cid,
                                              const char **selected_apn,
                                              bool *create)
{
    bool occupied[RG255_PDP_CONTEXT_ID_MAX + 1U];
    const rg255_pdp_config_t *candidate;
    size_t candidate_count;
    size_t index;
    uint8_t cid;

    if (configs == NULL || required_apn == NULL || selected_cid == NULL ||
        selected_apn == NULL || create == NULL || count > RG255_PDP_CONTEXT_MAX)
    {
        return -EINVAL;
    }

    memset(occupied, 0, sizeof(occupied));
    candidate       = NULL;
    candidate_count = 0U;

    for (index = 0U; index < count; index++)
    {
        cid = configs[index].cid;
        if (cid < RG255_PDP_CONTEXT_ID_MIN || cid > RG255_PDP_CONTEXT_ID_MAX)
        {
            return -EBADMSG;
        }

        occupied[cid] = true;

        if (_cellular_fsm_pdp_is_ims(&configs[index]) ||
            configs[index].pdp_type != RG255_PDP_TYPE_IPV4V6)
        {
            continue;
        }

        if (required_apn[0] != '\0')
        {
            if (_cellular_fsm_apn_equal(configs[index].apn, required_apn))
            {
                *selected_cid = cid;
                *selected_apn = configs[index].apn;
                *create       = false;
                return 0;
            }

            continue;
        }

        if (configs[index].apn[0] == '\0')
        {
            continue;
        }

        candidate = &configs[index];
        candidate_count++;
    }

    if (required_apn[0] == '\0')
    {
        if (candidate_count == 0U)
        {
            return -ENOENT;
        }

        if (candidate_count != 1U)
        {
            return -ENOTUNIQ;
        }

        *selected_cid = candidate->cid;
        *selected_apn = candidate->apn;
        *create       = false;
        return 0;
    }

    if (_cellular_fsm_apn_equal(required_apn, "ims"))
    {
        return -EPERM;
    }

    for (cid = RG255_PDP_CONTEXT_ID_MIN; cid <= RG255_PDP_CONTEXT_ID_MAX; cid++)
    {
        if (!occupied[cid])
        {
            *selected_cid = cid;
            *selected_apn = required_apn;
            *create       = true;
            return 0;
        }
    }

    return -ENOSPC;
}

/**
 * @brief 清空当前公网验证轮次状态。
 */
static void _cellular_fsm_reset_verify(cellular_fsm_t *fsm)
{
    fsm->verify_ipv4_done     = false;
    fsm->verify_ipv6_done     = false;
    fsm->verify_failure_count = 0U;
}

/****************************** 状态结果辅助 ******************************/

/**
 * @brief 构造保持当前运行状态的处理结果。
 */
static cellular_fsm_step_t _cellular_fsm_step_wait(void)
{
    cellular_fsm_step_t step;

    memset(&step, 0, sizeof(step));
    step.result = CELLULAR_RUNTIME_STEP_WAIT;

    return step;
}

/**
 * @brief 构造推进到指定运行状态的处理结果。
 */
static cellular_fsm_step_t _cellular_fsm_step_done(cellular_runtime_state_t next_state)
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
static cellular_fsm_step_t _cellular_fsm_step_failed(int error)
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
static cellular_fsm_step_t _cellular_fsm_step_fatal(int error)
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

        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
            return CELLULAR_FSM_VERIFY_TIMEOUT_MS;

        default:
            return 0U;
    }
}

/**
 * @brief 安排Owner下一轮定向确认指定状态事实。
 */
static void _cellular_fsm_request_refresh(cellular_fsm_t *fsm, cellular_status_refresh_mask_t requested)
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
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_NETDEV |
                                       CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                       CELLULAR_STATUS_REFRESH_HOST);
            break;

        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP_ADDRESS |
                                       CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                       CELLULAR_STATUS_REFRESH_HOST);
            break;

        case CELLULAR_RUNTIME_STATE_ONLINE:
            _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_SIM |
                                       CELLULAR_STATUS_REFRESH_REGISTRATION |
                                       CELLULAR_STATUS_REFRESH_PDP |
                                       CELLULAR_STATUS_REFRESH_NETDEV |
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

    if (state == CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY)
    {
        _cellular_fsm_reset_verify(fsm);
    }

    if (state == CELLULAR_RUNTIME_STATE_ONLINE)
    {
        cellular_runtime_clear_failure(&fsm->runtime, now_ms);
        ret = cellular_runtime_schedule_action(&fsm->runtime, now_ms, CELLULAR_FSM_ONLINE_VERIFY_INTERVAL_MS);
        if (ret != 0)
        {
            return ret;
        }
    }

    _cellular_fsm_request_state_refresh(fsm, state);

    if (previous != state)
    {
        LINKG_LOG_INFO("CELLULAR: runtime state changed, old=%s, new=%s", cellular_runtime_state_name(previous), cellular_runtime_state_name(state));
    }

    return 0;
}

/****************************** 数据会话清理 ******************************/

/**
 * @brief 清理Linux蜂窝接口上一轮数据会话残留的网络状态。
 */
static void _cellular_fsm_cleanup_host_network(void)
{
    if (!linkg_network_interface_exists(LINKG_RESOURCE_INTERFACE_CELLULAR))
    {
        return;
    }

    // 先关闭接口，阻止旧地址和路由继续被使用。
    (void)linkg_network_interface_set_up(
        LINKG_RESOURCE_INTERFACE_CELLULAR,
        false);

    // 清除上一轮DHCP IPv4地址。
    linkg_os_run_ignore("ip",
                        "-4",
                        "addr",
                        "flush",
                        "dev",
                        LINKG_RESOURCE_INTERFACE_CELLULAR,
                        "scope",
                        "global",
                        NULL);

    // 清除上一轮RA/SLAAC产生的Global IPv6，保留fe80::链路本地地址。
    linkg_os_run_ignore("ip",
                        "-6",
                        "addr",
                        "flush",
                        "dev",
                        LINKG_RESOURCE_INTERFACE_CELLULAR,
                        "scope",
                        "global",
                        NULL);

    // 清除上一轮动态路由。
    linkg_os_run_ignore("ip",
                        "-4",
                        "route",
                        "flush",
                        "dev",
                        LINKG_RESOURCE_INTERFACE_CELLULAR,
                        NULL);

    linkg_os_run_ignore("ip",
                        "-6",
                        "route",
                        "flush",
                        "dev",
                        LINKG_RESOURCE_INTERFACE_CELLULAR,
                        NULL);
}

/**
 * @brief 按QNETDEV到PDP的逆序尽力停止当前数据会话。
 *
 * @note 本函数不结束SIM插卡会话；连接失败重试时保留SIM会话和PIN保护。
 */
static int _cellular_fsm_cleanup_data_session(cellular_fsm_t *fsm, at_channel_t *channel)
{
    uint8_t       pdp_cid;
    bool          pdp_cid_valid;
    int           first_error;
    int           ret;

    if (fsm == NULL)
    {
        return -EINVAL;
    }

    if (channel == NULL)
    {
        return -ENODEV;
    }

    first_error = 0;
    pdp_cid_valid = cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid);

    _cellular_fsm_cleanup_host_network();

    if ((fsm->netdev_action_started || fsm->pdp_action_started) && !pdp_cid_valid)
    {
        LINKG_LOG_WARN("CELLULAR: data cleanup skipped without selected PDP CID");
        _cellular_fsm_record_first_error(&first_error, -EPROTO);
    }

    if (fsm->netdev_action_started && pdp_cid_valid)
    {
        ret = rg255_cmd_stop_netdev(channel, pdp_cid);
        if (ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: stop QNETDEV failed, error=%d", ret);
            _cellular_fsm_record_first_error(&first_error, ret);
        }
    }

    if (fsm->pdp_action_started && pdp_cid_valid)
    {
        ret = rg255_cmd_set_pdp_active(channel, pdp_cid, false);
        if (ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: deactivate PDP failed, error=%d", ret);
            _cellular_fsm_record_first_error(&first_error, ret);
        }
    }

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_PDP |
                                       CELLULAR_STATUS_REFRESH_PDP_ADDRESS |
                                       CELLULAR_STATUS_REFRESH_NETDEV |
                                       CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK |
                                       CELLULAR_STATUS_REFRESH_HOST);

    _cellular_fsm_reset_verify(fsm);

    return first_error;
}

/**
 * @brief 处理SIM拔出并回到等待新插卡状态。
 */
static int _cellular_fsm_handle_sim_removed(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    int cleanup_ret;
    int ret;

    cleanup_ret = 0;

    if (cellular_runtime_session_active(&fsm->runtime))
    {
        cleanup_ret = _cellular_fsm_cleanup_data_session(fsm, channel);
        cellular_runtime_end_session(&fsm->runtime, now_ms);
    }

    cellular_status_clear_pdp_cid();
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

    return cleanup_ret;
}

/****************************** Host事实校验 ******************************/

/**
 * @brief 判断IPv6地址是否属于指定网络前缀。
 */
static bool _cellular_fsm_ipv6_in_prefix(const struct in6_addr *address, const struct in6_addr *prefix, uint8_t prefix_length)
{
    uint8_t mask;
    size_t  full_bytes;
    uint8_t remaining_bits;

    if (address == NULL || prefix == NULL || prefix_length == 0U || prefix_length > 128U)
    {
        return false;
    }

    full_bytes     = prefix_length / 8U;
    remaining_bits = prefix_length % 8U;

    if (full_bytes > 0U && memcmp(address->s6_addr, prefix->s6_addr, full_bytes) != 0)
    {
        return false;
    }

    if (remaining_bits == 0U)
    {
        return true;
    }

    mask = (uint8_t)(0xffU << (8U - remaining_bits));

    return (address->s6_addr[full_bytes] & mask) ==
           (prefix->s6_addr[full_bytes] & mask);
}

/**
 * @brief 判断Linux Host网络事实是否完整匹配RG255期望配置。
 */
static bool _cellular_fsm_host_ready(const cellular_status_info_t *info)
{
    const cellular_status_host_info_t          *host;
    const cellular_status_expected_ipv4_info_t *expected_ipv4;
    const cellular_status_expected_ipv6_info_t *expected_ipv6;

    if (info == NULL)
    {
        return false;
    }

    host          = &info->host;
    expected_ipv4 = &info->netdev.expected_ipv4;
    expected_ipv6 = &info->netdev.expected_ipv6;

    if (!_cellular_fsm_meta_current(&info->pdp.address_meta) ||
        !info->pdp.global_ipv6_valid)
    {
        return false;
    }

    if (!_cellular_fsm_meta_current(&expected_ipv4->meta) || !expected_ipv4->valid ||
        !_cellular_fsm_meta_current(&expected_ipv6->meta) || !expected_ipv6->valid)
    {
        return false;
    }

    if (!_cellular_fsm_meta_current(&host->interface_meta) || !host->interface_present ||
        !_cellular_fsm_meta_current(&host->interface_up_meta) || !host->interface_up)
    {
        return false;
    }

    if (!_cellular_fsm_meta_current(&host->ipv4_meta) || !host->ipv4_valid ||
        !_cellular_fsm_meta_current(&host->ipv4_netmask_meta) || !host->ipv4_netmask_valid ||
        !_cellular_fsm_meta_current(&host->ipv4_route_meta) || !host->ipv4_gateway_valid)
    {
        return false;
    }

    if (memcmp(&host->ipv4, &expected_ipv4->address, sizeof(host->ipv4)) != 0 ||
        memcmp(&host->ipv4_netmask, &expected_ipv4->netmask, sizeof(host->ipv4_netmask)) != 0 ||
        memcmp(&host->ipv4_gateway, &expected_ipv4->gateway, sizeof(host->ipv4_gateway)) != 0)
    {
        return false;
    }

    if (!_cellular_fsm_meta_current(&host->ipv6_meta) || !host->global_ipv6_valid ||
        !_cellular_fsm_meta_current(&host->ipv6_route_meta) || !host->ipv6_gateway_valid)
    {
        return false;
    }

    if (!_cellular_fsm_ipv6_in_prefix(&host->global_ipv6,
                                        &expected_ipv6->prefix,
                                        expected_ipv6->prefix_length))
    {
        return false;
    }

    return memcmp(&host->ipv6_gateway,
                  &expected_ipv6->gateway,
                  sizeof(host->ipv6_gateway)) == 0;
}

/**
 * @brief 判断在线状态是否已经被最新成功事实明确否定。
 */
static bool _cellular_fsm_online_invalid(const cellular_fsm_t *fsm, const cellular_status_info_t *info)
{
    uint8_t pdp_cid;

    if (info == NULL)
    {
        return true;
    }

    if (_cellular_fsm_meta_current(&info->local.sim_meta) &&
        info->local.sim_state != LINKG_CELLULAR_SIM_STATE_READY)
    {
        return true;
    }

    if (_cellular_fsm_meta_current(&info->network.registration_meta) &&
        info->network.registration != LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
    {
        return true;
    }

    if (_cellular_fsm_meta_current(&info->pdp.active_meta) && !info->pdp.active)
    {
        return true;
    }

    if (_cellular_fsm_meta_current(&info->netdev.state.meta) &&
        !info->netdev.state.connected)
    {
        return true;
    }

    if (_cellular_fsm_meta_current(&info->netdev.state.meta) &&
        info->netdev.state.connected &&
        (!cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid) ||
         info->netdev.state.cid != pdp_cid))
    {
        return true;
    }

    if (_cellular_fsm_meta_current(&info->host.interface_meta) &&
        _cellular_fsm_meta_current(&info->host.interface_up_meta) &&
        _cellular_fsm_meta_current(&info->host.ipv4_meta) &&
        _cellular_fsm_meta_current(&info->host.ipv4_netmask_meta) &&
        _cellular_fsm_meta_current(&info->host.ipv6_meta) &&
        _cellular_fsm_meta_current(&info->host.ipv4_route_meta) &&
        _cellular_fsm_meta_current(&info->host.ipv6_route_meta) &&
        _cellular_fsm_meta_current(&info->netdev.expected_ipv4.meta) &&
        _cellular_fsm_meta_current(&info->netdev.expected_ipv6.meta) &&
        !_cellular_fsm_host_ready(info))
    {
        return true;
    }

    return false;
}

/****************************** SIM会话协调 ******************************/

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
        ret = _cellular_fsm_handle_sim_removed(fsm, channel, now_ms);
        if (ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: cleanup after SIM removal returned error=%d", ret);
        }

        return 1;
    }

    if (!cellular_runtime_session_active(&fsm->runtime) &&
        sim_state != LINKG_CELLULAR_SIM_STATE_UNKNOWN)
    {
        ret = cellular_runtime_begin_session(&fsm->runtime, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        cellular_status_clear_pdp_cid();
        fsm->pdp_action_started    = false;
        fsm->netdev_action_started = false;

        ret = cellular_fsm_enter(fsm, CELLULAR_RUNTIME_STATE_CHECK_SIM, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        return 1;
    }

    if (cellular_runtime_session_active(&fsm->runtime) &&
        sim_state != LINKG_CELLULAR_SIM_STATE_UNKNOWN &&
        sim_state != LINKG_CELLULAR_SIM_STATE_READY)
    {
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

/****************************** 状态步骤 ******************************/

/**
 * @brief 处理IDLE状态并进入SIM等待流程。
 */
static cellular_fsm_step_t _cellular_fsm_state_idle(void)
{
    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_SIM);
}

/**
 * @brief 处理等待SIM插入状态。
 */
static cellular_fsm_step_t _cellular_fsm_state_wait_sim(const cellular_fsm_t *fsm)
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
static cellular_fsm_step_t _cellular_fsm_state_check_sim(const cellular_fsm_t *fsm, const linkg_cellular_config_t *config, const cellular_status_info_t *info, uint64_t now_ms)
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

        case LINKG_CELLULAR_SIM_STATE_ABSENT:
            return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_SIM);

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
static cellular_fsm_step_t _cellular_fsm_state_enter_pin(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms)
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

        LINKG_LOG_WARN("CELLULAR: enter SIM PIN returned error=%d, action=query-truth", ret);
    }

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_SIM);

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_CHECK_SIM);
}

/**
 * @brief 处理等待用户提供正确PIN或更换SIM状态。
 */
static cellular_fsm_step_t _cellular_fsm_state_wait_pin(const cellular_status_info_t *info)
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
static cellular_fsm_step_t _cellular_fsm_state_wait_puk(const cellular_status_info_t *info)
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

/**
 * @brief 等待移动网络注册成功。
 */
static cellular_fsm_step_t _cellular_fsm_state_wait_registration(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info != NULL && _cellular_fsm_meta_current(&info->network.registration_meta))
    {
        if (info->network.registration == LINKG_CELLULAR_REGISTRATION_STATE_REGISTERED)
        {
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

/**
 * @brief 查询全部PDP配置并由Owner选择或创建数据上下文。
 */
static cellular_fsm_step_t _cellular_fsm_state_prepare_pdp(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms)
{
    rg255_pdp_config_t configs[RG255_PDP_CONTEXT_MAX];
    const char        *selected_apn;
    size_t             count;
    uint8_t            selected_cid;
    bool               create;
    int                ret;

    if (channel == NULL)
    {
        return _cellular_fsm_step_fatal(-ENODEV);
    }

    ret = cellular_runtime_note_attempt(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    memset(configs, 0, sizeof(configs));
    count = 0U;
    ret = rg255_query_pdp_configs(channel, configs, RG255_PDP_CONTEXT_MAX, &count);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        return _cellular_fsm_step_failed(ret);
    }

    selected_apn = NULL;
    selected_cid = 0U;
    create       = false;

    ret = _cellular_fsm_select_pdp_context(configs,
                                             count,
                                             config->apn,
                                             &selected_cid,
                                             &selected_apn,
                                             &create);
    if (ret != 0)
    {
        if (ret == -ENOSPC)
        {
            LINKG_LOG_WARN("CELLULAR: no free PDP context for configured APN");
        }

        return _cellular_fsm_step_failed(ret);
    }

    if (create)
    {
        ret = rg255_cmd_set_pdp_context(channel, selected_cid, selected_apn);
        if (ret != 0)
        {
            if (_cellular_fsm_action_error_fatal(ret))
            {
                return _cellular_fsm_step_fatal(ret);
            }

            return _cellular_fsm_step_failed(ret);
        }
    }

    ret = cellular_runtime_set_pdp_cid(&fsm->runtime, selected_cid, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    ret = cellular_status_set_pdp_cid(selected_cid);
    if (ret != 0)
    {
        cellular_runtime_clear_pdp_cid(&fsm->runtime, now_ms);
        return _cellular_fsm_step_fatal(ret);
    }

    LINKG_LOG_INFO("CELLULAR: PDP context selected, cid=%u, apn=%s, source=%s",
                   (unsigned int)selected_cid,
                   selected_apn,
                   create ? "created" : "existing");

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_ACTIVATE_PDP);
}

/**
 * @brief 请求激活默认PDP上下文并转入事实确认状态。
 */
static cellular_fsm_step_t _cellular_fsm_state_activate_pdp(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    uint8_t pdp_cid;
    int     ret;

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

    fsm->pdp_action_started = true;

    ret = rg255_cmd_set_pdp_active(channel, pdp_cid, true);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        LINKG_LOG_WARN("CELLULAR: activate PDP returned error=%d, action=query-truth", ret);
    }

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PDP);
}

/**
 * @brief 等待Status确认默认PDP上下文已经激活。
 */
static cellular_fsm_step_t _cellular_fsm_state_wait_pdp(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    int error;

    if (info != NULL && _cellular_fsm_meta_current(&info->pdp.active_meta) && info->pdp.active)
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_START_NETDEV);
    }

    if (!cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_wait();
    }

    error = -ETIMEDOUT;
    if (info != NULL && info->pdp.active_meta.last_error != 0)
    {
        error = info->pdp.active_meta.last_error;
    }

    return _cellular_fsm_step_failed(error);
}

/**
 * @brief 请求启动RG255 USB网络设备连接并转入事实确认状态。
 */
static cellular_fsm_step_t _cellular_fsm_state_start_netdev(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    uint8_t pdp_cid;
    int     ret;

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

    fsm->netdev_action_started = true;

    ret = rg255_cmd_start_netdev(channel, pdp_cid);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        LINKG_LOG_WARN("CELLULAR: start QNETDEV returned error=%d, action=query-truth", ret);
    }

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_NETDEV);
}

/**
 * @brief 等待Status确认USB网络设备已经连接。
 */
static cellular_fsm_step_t _cellular_fsm_state_wait_netdev(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    uint8_t pdp_cid;
    int error;

    if (!cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid))
    {
        return _cellular_fsm_step_fatal(-EPROTO);
    }

    if (info != NULL && _cellular_fsm_meta_current(&info->netdev.state.meta) &&
        info->netdev.state.connected && info->netdev.state.cid == pdp_cid)
    {
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

/**
 * @brief 准备Linux蜂窝网络接口并获取IPv4网络配置。
 *
 * @note LinkG开启转发后usb0必须使用accept_ra=2，确保IPv6 RA/SLAAC继续工作。
 */
static cellular_fsm_step_t _cellular_fsm_state_prepare_host(cellular_fsm_t *fsm, uint64_t now_ms)
{
    int ret;

    ret = cellular_runtime_note_attempt(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    ret = linkg_network_interface_wait(LINKG_RESOURCE_INTERFACE_CELLULAR, 3000U);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    ret = linkg_network_interface_ipv6_accept_ra_set(LINKG_RESOURCE_INTERFACE_CELLULAR, LINKG_NETWORK_IPV6_ACCEPT_RA_FORCE);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    ret = linkg_network_interface_set_up(
        LINKG_RESOURCE_INTERFACE_CELLULAR,
        true);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    ret = linkg_os_run("udhcpc",
                       "-f",
                       "-n",
                       "-q",
                       "-t",
                       "5",
                       "-i",
                       LINKG_RESOURCE_INTERFACE_CELLULAR,
                       NULL);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    _cellular_fsm_request_refresh(fsm, CELLULAR_STATUS_REFRESH_HOST |
                                       CELLULAR_STATUS_REFRESH_EXPECTED_NETWORK);

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_HOST);
}

/**
 * @brief 等待Linux Host网络配置完整匹配RG255期望事实。
 */
static cellular_fsm_step_t _cellular_fsm_state_wait_host(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    if (_cellular_fsm_host_ready(info))
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY);
    }

    if (cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_failed(-ETIMEDOUT);
    }

    return _cellular_fsm_step_wait();
}

/**
 * @brief 通过指定地址族和usb0接口执行一次公网连通性探测。
 */
static int _cellular_fsm_probe_connectivity(bool ipv6)
{
    if (ipv6)
    {
        return linkg_os_run("ping6",
                            "-I",
                            LINKG_RESOURCE_INTERFACE_CELLULAR,
                            "-c",
                            "1",
                            "-W",
                            "2",
                            CELLULAR_FSM_VERIFY_TARGET,
                            NULL);
    }

    return linkg_os_run("ping",
                        "-4",
                        "-I",
                        LINKG_RESOURCE_INTERFACE_CELLULAR,
                        "-c",
                        "1",
                        "-W",
                        "2",
                        CELLULAR_FSM_VERIFY_TARGET,
                        NULL);
}

/**
 * @brief 顺序验证IPv4和IPv6真实公网连通性。
 */
static cellular_fsm_step_t _cellular_fsm_state_verify(cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    bool ipv6;
    int  ret;

    if (!_cellular_fsm_host_ready(info))
    {
        return _cellular_fsm_step_failed(-ENETDOWN);
    }

    if (fsm->runtime.next_action_ms != 0U &&
        !cellular_runtime_action_due(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_wait();
    }

    cellular_runtime_clear_action(&fsm->runtime, now_ms);

    ipv6 = fsm->verify_ipv4_done;

    ret = cellular_runtime_note_attempt(&fsm->runtime, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    ret = _cellular_fsm_probe_connectivity(ipv6);
    if (ret == 0)
    {
        fsm->verify_failure_count = 0U;

        if (!ipv6)
        {
            fsm->verify_ipv4_done = true;

            ret = cellular_runtime_schedule_action(&fsm->runtime,
                                                   now_ms,
                                                   CELLULAR_FSM_VERIFY_NEXT_FAMILY_DELAY_MS);
            if (ret != 0)
            {
                return _cellular_fsm_step_fatal(ret);
            }

            return _cellular_fsm_step_wait();
        }

        fsm->verify_ipv6_done = true;

        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_ONLINE);
    }

    fsm->verify_failure_count++;

    if (fsm->verify_failure_count >= CELLULAR_FSM_VERIFY_FAILURE_LIMIT ||
        cellular_runtime_state_timed_out(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_failed(-ENETUNREACH);
    }

    ret = cellular_runtime_schedule_action(&fsm->runtime,
                                           now_ms,
                                           CELLULAR_FSM_VERIFY_RETRY_MS);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    return _cellular_fsm_step_wait();
}

/**
 * @brief 维护已经验证上线的蜂窝数据链状态。
 */
static cellular_fsm_step_t _cellular_fsm_state_online(cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    int ret;

    if (_cellular_fsm_online_invalid(fsm, info))
    {
        return _cellular_fsm_step_failed(-ENETDOWN);
    }

    if (fsm->runtime.next_action_ms == 0U)
    {
        ret = cellular_runtime_schedule_action(&fsm->runtime,
                                               now_ms,
                                               CELLULAR_FSM_ONLINE_VERIFY_INTERVAL_MS);
        if (ret != 0)
        {
            return _cellular_fsm_step_fatal(ret);
        }

        return _cellular_fsm_step_wait();
    }

    if (cellular_runtime_action_due(&fsm->runtime, now_ms))
    {
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY);
    }

    return _cellular_fsm_step_wait();
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

        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
            return _cellular_fsm_state_verify(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_ONLINE:
            return _cellular_fsm_state_online(fsm, info, now_ms);

        case CELLULAR_RUNTIME_STATE_RETRY_WAIT:
            return _cellular_fsm_state_retry_wait(fsm, now_ms);

        case CELLULAR_RUNTIME_STATE_NONE:
        default:
            return _cellular_fsm_step_fatal(-EPROTO);
    }
}

/****************************** 失败处理 ******************************/

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
 * @brief 记录当前状态失败并执行对应V1统一恢复策略。
 */
int cellular_fsm_handle_failure(cellular_fsm_t *fsm, at_channel_t *channel, int error, uint64_t now_ms)
{
    cellular_runtime_state_t failed_state;
    cellular_runtime_state_t retry_target;
    uint64_t                 retry_delay_ms;
    int                      cleanup_ret;
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

    LINKG_LOG_WARN("CELLULAR: runtime state failed, state=%s, error=%d",
                   cellular_runtime_state_name(failed_state),
                   error);

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
        case CELLULAR_RUNTIME_STATE_START_NETDEV:
        case CELLULAR_RUNTIME_STATE_WAIT_NETDEV:
        case CELLULAR_RUNTIME_STATE_PREPARE_HOST:
        case CELLULAR_RUNTIME_STATE_WAIT_HOST:
        case CELLULAR_RUNTIME_STATE_VERIFY_CONNECTIVITY:
        case CELLULAR_RUNTIME_STATE_ONLINE:
            retry_target = CELLULAR_RUNTIME_STATE_WAIT_REGISTRATION;
            break;

        default:
            return -EPROTO;
    }

    if (failed_state != CELLULAR_RUNTIME_STATE_CHECK_SIM)
    {
        cleanup_ret = _cellular_fsm_cleanup_data_session(fsm, channel);
        if (cleanup_ret != 0)
        {
            LINKG_LOG_WARN("CELLULAR: data cleanup during retry returned error=%d", cleanup_ret);
        }
    }

    retry_delay_ms = _cellular_fsm_retry_delay(fsm);

    ret = cellular_runtime_schedule_retry(&fsm->runtime,
                                          retry_target,
                                          now_ms,
                                          retry_delay_ms);
    if (ret != 0)
    {
        return ret;
    }

    LINKG_LOG_INFO("CELLULAR: retry scheduled, target=%s, delay_ms=%llu, count=%u",
                   cellular_runtime_state_name(retry_target),
                   (unsigned long long)retry_delay_ms,
                   (unsigned int)fsm->runtime.retry_count);

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
        if (channel == NULL)
        {
            cleanup_ret = -ENODEV;
        }
        else
        {
            cleanup_ret = _cellular_fsm_cleanup_data_session(fsm, channel);
        }

        cellular_runtime_end_session(&fsm->runtime, now_ms);
    }

    cellular_status_clear_pdp_cid();
    fsm->pdp_action_started    = false;
    fsm->netdev_action_started = false;
    _cellular_fsm_reset_verify(fsm);

    return cleanup_ret;
}
