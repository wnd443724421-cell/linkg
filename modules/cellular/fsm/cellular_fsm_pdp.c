/**
 * @file cellular_fsm_pdp.c
 * @brief LinkG蜂窝PDP上下文选择与激活状态处理实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-10-10
 */

#include "cellular_fsm_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>

#include "rg255_cmd.h"
#include "rg255_query.h"

#include "cellular_internal.h"

/****************************** 模块常量 ******************************/

#define CELLULAR_FSM_PDP_DATA_CID 1U // RG255AACNAAR02A04M16固件用户数据固定CID
#define CELLULAR_FSM_PDP_IMS_CID  8U // 保护运营商IMS上下文

/****************************** 运营商APN配置 ******************************/

typedef struct
{
    const char        *name;              // 运营商名称
    const char *const *imsi_prefixes;     // SIM归属运营商IMSI前缀表
    size_t             imsi_prefix_count; // IMSI前缀数量
    const char *const *apns;              // 允许使用的数据APN，第一个是默认APN
    size_t             apn_count;         // 允许使用的APN数量
} cellular_fsm_pdp_operator_t;

static const char *const g_cellular_fsm_mobile_apns[] =
{
    "cmnet" // 中国移动默认数据APN
};

static const char *const g_cellular_fsm_unicom_apns[] =
{
    "3gnet" // 中国联通默认数据APN
};

static const char *const g_cellular_fsm_telecom_apns[] =
{
    "ctnet" // 中国电信默认数据APN
};

static const char *const g_cellular_fsm_broadnet_apns[] =
{
    "cbnet" // 中国广电默认数据APN
};

static const char *const g_cellular_fsm_mobile_imsi_prefixes[] =
{
    "46000", "46002", "46004", "46007", "46008", "46013"
};

static const char *const g_cellular_fsm_unicom_imsi_prefixes[] =
{
    "46001", "46006", "46009", "46030"
};

static const char *const g_cellular_fsm_telecom_imsi_prefixes[] =
{
    "46003", "46005", "46011", "46012"
};

static const char *const g_cellular_fsm_broadnet_imsi_prefixes[] =
{
    "46015"
};

static const cellular_fsm_pdp_operator_t g_cellular_fsm_pdp_operators[] =
{
    {
        .name              = "mobile",
        .imsi_prefixes     = g_cellular_fsm_mobile_imsi_prefixes,
        .imsi_prefix_count = sizeof(g_cellular_fsm_mobile_imsi_prefixes) / sizeof(g_cellular_fsm_mobile_imsi_prefixes[0]),
        .apns              = g_cellular_fsm_mobile_apns,
        .apn_count         = sizeof(g_cellular_fsm_mobile_apns) / sizeof(g_cellular_fsm_mobile_apns[0])
    },
    {
        .name              = "unicom",
        .imsi_prefixes     = g_cellular_fsm_unicom_imsi_prefixes,
        .imsi_prefix_count = sizeof(g_cellular_fsm_unicom_imsi_prefixes) / sizeof(g_cellular_fsm_unicom_imsi_prefixes[0]),
        .apns              = g_cellular_fsm_unicom_apns,
        .apn_count         = sizeof(g_cellular_fsm_unicom_apns) / sizeof(g_cellular_fsm_unicom_apns[0])
    },
    {
        .name              = "telecom",
        .imsi_prefixes     = g_cellular_fsm_telecom_imsi_prefixes,
        .imsi_prefix_count = sizeof(g_cellular_fsm_telecom_imsi_prefixes) / sizeof(g_cellular_fsm_telecom_imsi_prefixes[0]),
        .apns              = g_cellular_fsm_telecom_apns,
        .apn_count         = sizeof(g_cellular_fsm_telecom_apns) / sizeof(g_cellular_fsm_telecom_apns[0])
    },
    {
        .name              = "broadnet",
        .imsi_prefixes     = g_cellular_fsm_broadnet_imsi_prefixes,
        .imsi_prefix_count = sizeof(g_cellular_fsm_broadnet_imsi_prefixes) / sizeof(g_cellular_fsm_broadnet_imsi_prefixes[0]),
        .apns              = g_cellular_fsm_broadnet_apns,
        .apn_count         = sizeof(g_cellular_fsm_broadnet_apns) / sizeof(g_cellular_fsm_broadnet_apns[0])
    }
};

/****************************** APN选择 ******************************/

/**
 * @brief 判断两个APN是否大小写无关地完全一致。
 */
static bool _cellular_fsm_apn_equal(const char *left, const char *right)
{
    return left != NULL && right != NULL && strcasecmp(left, right) == 0;
}

/**
 * @brief 判断IMSI是否属于指定运营商前缀。
 */
static bool _cellular_fsm_imsi_matches_operator(const char *imsi, const cellular_fsm_pdp_operator_t *operator_info)
{
    size_t index;
    size_t prefix_length;

    if (imsi == NULL || operator_info == NULL)
    {
        return false;
    }

    for (index = 0U; index < operator_info->imsi_prefix_count; index++)
    {
        prefix_length = strlen(operator_info->imsi_prefixes[index]);
        if (strncmp(imsi, operator_info->imsi_prefixes[index], prefix_length) == 0)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 依据SIM归属运营商选择明确允许的目标APN。
 *
 * @note configured_apn不在该运营商白名单时回退默认APN；无法识别运营商时不猜测。
 */
static int _cellular_fsm_select_effective_apn(at_channel_t *channel, const char *configured_apn, const char **effective_apn, const char **operator_name)
{
    char imsi[RG255_IMSI_BUFFER_SIZE];
    const cellular_fsm_pdp_operator_t *operator_info;
    size_t operator_index;
    size_t apn_index;
    int    ret;

    if (channel == NULL || configured_apn == NULL || effective_apn == NULL || operator_name == NULL)
    {
        return -EINVAL;
    }

    *effective_apn = NULL;
    *operator_name = NULL;

    memset(imsi, 0, sizeof(imsi));
    ret = rg255_cmd_get_imsi(channel, imsi, (int)sizeof(imsi));
    if (ret != 0)
    {
        return ret;
    }

    if (strlen(imsi) < 6U)
    {
        return -EBADMSG;
    }

    for (operator_index = 0U; operator_index < sizeof(g_cellular_fsm_pdp_operators) / sizeof(g_cellular_fsm_pdp_operators[0]); operator_index++)
    {
        operator_info = &g_cellular_fsm_pdp_operators[operator_index];
        if (!_cellular_fsm_imsi_matches_operator(imsi, operator_info))
        {
            continue;
        }

        *operator_name = operator_info->name;
        *effective_apn = operator_info->apns[0];

        if (configured_apn[0] == '\0')
        {
            return 0;
        }

        for (apn_index = 0U; apn_index < operator_info->apn_count; apn_index++)
        {
            if (_cellular_fsm_apn_equal(configured_apn, operator_info->apns[apn_index]))
            {
                *effective_apn = operator_info->apns[apn_index];
                return 0;
            }
        }

        CELLULAR_WARN("configured APN not allowed for SIM operator, operator=%s, apn=%s, fallback=%s", operator_info->name, configured_apn, operator_info->apns[0]);
        return 0;
    }

    return -EOPNOTSUPP;
}

/****************************** PDP上下文检查 ******************************/

/**
 * @brief 检查固定用户数据CID是否符合目标APN和双栈类型。
 *
 * @note 只检查CID 1；不选择、创建或修改其他CID。
 */
static int _cellular_fsm_check_pdp_context(const rg255_pdp_config_t *configs, size_t count, const char *effective_apn, bool *configure, bool *exists)
{
    size_t index;

    if (configs == NULL || effective_apn == NULL || effective_apn[0] == '\0' || configure == NULL || exists == NULL || count > RG255_PDP_CONTEXT_MAX)
    {
        return -EINVAL;
    }

    *configure = true;
    *exists    = false;

    for (index = 0U; index < count; index++)
    {
        if (configs[index].cid != CELLULAR_FSM_PDP_DATA_CID)
        {
            continue;
        }

        if (*exists)
        {
            return -EBADMSG;
        }

        *exists    = true;
        *configure = configs[index].pdp_type != RG255_PDP_TYPE_IPV4V6 || !_cellular_fsm_apn_equal(configs[index].apn, effective_apn);
    }

    return 0;
}

/**
 * @brief 判断APN是否属于LinkG已支持的普通用户数据APN。
 */
static bool _cellular_fsm_pdp_managed_apn(const char *apn)
{
    const cellular_fsm_pdp_operator_t *operator_info;
    size_t operator_index;
    size_t apn_index;

    if (apn == NULL || apn[0] == '\0')
    {
        return false;
    }

    for (operator_index = 0U; operator_index < sizeof(g_cellular_fsm_pdp_operators) / sizeof(g_cellular_fsm_pdp_operators[0]); operator_index++)
    {
        operator_info = &g_cellular_fsm_pdp_operators[operator_index];
        for (apn_index = 0U; apn_index < operator_info->apn_count; apn_index++)
        {
            if (_cellular_fsm_apn_equal(apn, operator_info->apns[apn_index]))
            {
                return true;
            }
        }
    }

    return false;
}

/**
 * @brief 确认指定CID对应已知用户数据配置，避免接管IMS或未知用途的上下文。
 *
 * @note 旧CID允许IP、IPV6或IPV4V6，但必须属于已知数据APN；未知PDP类型仍禁止接管。
 */
static int _cellular_fsm_pdp_check_managed_context(const rg255_pdp_config_t *configs, size_t count, uint8_t cid)
{
    const rg255_pdp_config_t *context;
    size_t index;

    if (configs == NULL || cid < RG255_PDP_CONTEXT_ID_MIN || cid > RG255_PDP_CONTEXT_ID_MAX || count > RG255_PDP_CONTEXT_MAX)
    {
        return -EINVAL;
    }

    if (cid == CELLULAR_FSM_PDP_IMS_CID)
    {
        return -EPERM;
    }

    context = NULL;
    for (index = 0U; index < count; index++)
    {
        if (configs[index].cid != cid)
        {
            continue;
        }

        if (context != NULL)
        {
            return -EBADMSG;
        }

        context = &configs[index];
    }

    if (context == NULL)
    {
        return -ENOENT;
    }

    if (cid == CELLULAR_FSM_PDP_DATA_CID && context->apn[0] == '\0')
    {
        return 0;
    }

    if (!_cellular_fsm_pdp_managed_apn(context->apn) ||
        (cid != CELLULAR_FSM_PDP_DATA_CID && context->pdp_type == RG255_PDP_TYPE_UNKNOWN))
    {
        return -EPERM;
    }

    return 0;
}

/**
 * @brief 查询并按需去激活指定PDP上下文，命令执行后必须回读确认。
 */
static int _cellular_fsm_pdp_deactivate_if_needed(at_channel_t *channel, uint8_t cid)
{
    bool active;
    int  action_ret;
    int  ret;

    active = false;
    ret = rg255_query_pdp_active(channel, cid, &active);
    if (ret == -ENOENT)
    {
        return 0;
    }

    if (ret != 0 || !active)
    {
        return ret;
    }

    action_ret = rg255_cmd_set_pdp_active(channel, cid, false);

    active = false;
    ret = rg255_query_pdp_active(channel, cid, &active);
    if (ret == -ENOENT || (ret == 0 && !active))
    {
        CELLULAR_INFO("PDP context deactivated, cid=%u", (unsigned int)cid);
        return 0;
    }

    if (ret != 0)
    {
        return ret;
    }

    return action_ret != 0 ? action_ret : -EAGAIN;
}

/**
 * @brief 根据当前QNETDEV事实迁移旧用户数据CID，或为重配CID 1断开旧连接。
 *
 * @note 只处理已知数据APN；每次AT操作后回读，未收敛交由FSM退避重试。
 */
static int _cellular_fsm_pdp_prepare_netdev(at_channel_t *channel, const rg255_pdp_config_t *configs, size_t count, bool configure)
{
    rg255_netdev_status_t netdev;
    uint8_t               old_cid;
    bool                  old_managed;
    bool                  cleanup_host;
    int                   action_ret;
    int                   ret;

    memset(&netdev, 0, sizeof(netdev));
    netdev.type = RG255_NETDEV_TYPE_UNKNOWN;
    ret = rg255_query_netdev_status(channel, &netdev);
    if (ret != 0)
    {
        return ret;
    }

    if (netdev.type == RG255_NETDEV_TYPE_DISCONNECT && netdev.connected)
    {
        return -EAGAIN;
    }

    old_cid      = netdev.cid;
    old_managed  = false;
    cleanup_host = false;
    if (old_cid != CELLULAR_FSM_PDP_DATA_CID && old_cid >= RG255_PDP_CONTEXT_ID_MIN && old_cid <= RG255_PDP_CONTEXT_ID_MAX)
    {
        ret = _cellular_fsm_pdp_check_managed_context(configs, count, old_cid);
        if (ret == 0)
        {
            old_managed = true;
        }
        else if (netdev.type != RG255_NETDEV_TYPE_DISCONNECT)
        {
            CELLULAR_WARN("QNETDEV migration blocked, cid=%u, error=%d", (unsigned int)old_cid, ret);
            return -EBUSY;
        }
    }

    if (netdev.type != RG255_NETDEV_TYPE_DISCONNECT &&
        (old_cid != CELLULAR_FSM_PDP_DATA_CID || configure))
    {
        if (old_cid != CELLULAR_FSM_PDP_DATA_CID && !old_managed)
        {
            return -EBUSY;
        }

        action_ret = rg255_cmd_stop_netdev(channel, old_cid);

        memset(&netdev, 0, sizeof(netdev));
        netdev.type = RG255_NETDEV_TYPE_UNKNOWN;
        ret = rg255_query_netdev_status(channel, &netdev);
        if (ret != 0)
        {
            return ret;
        }

        if (netdev.type != RG255_NETDEV_TYPE_DISCONNECT || netdev.connected)
        {
            return action_ret != 0 ? action_ret : -EAGAIN;
        }

        cleanup_host = true;
        CELLULAR_INFO("QNETDEV disconnected for fixed CID 1, previous_cid=%u", (unsigned int)old_cid);
    }

    // QNETDEV已处于断开状态但仍能确认旧数据CID时，也要处理Host残留。
    if (old_managed)
    {
        cleanup_host = true;
    }

    if (cleanup_host)
    {
        // 只在旧QNETDEV确认断开后清理Host，避免影响CID 1的正常数据会话。
        _cellular_fsm_cleanup_host_network();
        CELLULAR_INFO("Host cleanup requested for fixed CID 1, previous_cid=%u", (unsigned int)old_cid);
    }

    if (old_managed)
    {
        ret = _cellular_fsm_pdp_deactivate_if_needed(channel, old_cid);
        if (ret != 0)
        {
            CELLULAR_WARN("old data PDP deactivate not confirmed, cid=%u, error=%d", (unsigned int)old_cid, ret);
            return ret;
        }
    }

    return 0;
}

/**
 * @brief 回读确认CID配置的APN和双栈类型均符合目标配置。
 */
static int _cellular_fsm_confirm_pdp_context(at_channel_t *channel, rg255_pdp_config_t *configs, uint8_t cid, const char *effective_apn)
{
    size_t count;
    size_t index;
    int    ret;

    count = 0U;
    ret = rg255_query_pdp_configs(channel, configs, RG255_PDP_CONTEXT_MAX, &count);
    if (ret != 0)
    {
        return ret;
    }

    for (index = 0U; index < count; index++)
    {
        if (configs[index].cid == cid)
        {
            if (configs[index].pdp_type != RG255_PDP_TYPE_IPV4V6 || !_cellular_fsm_apn_equal(configs[index].apn, effective_apn))
            {
                return -EPROTO;
            }

            return 0;
        }
    }

    return -EAGAIN;
}

/****************************** PDP状态步骤 ******************************/

/**
 * @brief 识别SIM运营商并按需配置固定CID 1双栈PDP上下文。
 */
cellular_fsm_step_t _cellular_fsm_state_prepare_pdp(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms)
{
    rg255_pdp_config_t configs[RG255_PDP_CONTEXT_MAX];
    const char        *effective_apn;
    const char           *operator_name;
    const char           *source;
    size_t                count;
    uint8_t               selected_cid;
    uint8_t               previous_cid;
    bool                  configure;
    bool                  context_exists;
    int                   ret;

    if (fsm == NULL || config == NULL)
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

    effective_apn = NULL;
    operator_name = NULL;
    ret = _cellular_fsm_select_effective_apn(channel, config->apn, &effective_apn, &operator_name);
    if (ret != 0)
    {
        if (ret == -EOPNOTSUPP)
        {
            CELLULAR_WARN("SIM operator unsupported by APN policy");
        }

        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        return _cellular_fsm_step_failed(ret);
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

    selected_cid   = CELLULAR_FSM_PDP_DATA_CID;
    configure      = false;
    context_exists = false;
    ret = _cellular_fsm_check_pdp_context(configs, count, effective_apn, &configure, &context_exists);
    if (ret != 0)
    {
        return _cellular_fsm_step_failed(ret);
    }

    if ((fsm->pdp_action_started || fsm->netdev_action_started) &&
        cellular_runtime_get_pdp_cid(&fsm->runtime, &previous_cid) && previous_cid != selected_cid)
    {
        CELLULAR_WARN("PDP CID change requires prior session cleanup, old=%u, new=%u", (unsigned int)previous_cid, (unsigned int)selected_cid);
        return _cellular_fsm_step_failed(-EBUSY);
    }

    // 占用旧CID时先迁移QNETDEV，和CID 1是否需要重新配置无关。
    if (configure && context_exists)
    {
        ret = _cellular_fsm_pdp_check_managed_context(configs, count, selected_cid);
        if (ret != 0)
        {
            CELLULAR_WARN("PDP CID 1 configuration blocked by unmanaged context, error=%d", ret);
            return _cellular_fsm_step_failed(-EBUSY);
        }
    }

    ret = _cellular_fsm_pdp_prepare_netdev(channel, configs, count, configure);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        return _cellular_fsm_step_failed(ret);
    }

    if (configure)
    {
        ret = _cellular_fsm_pdp_deactivate_if_needed(channel, selected_cid);
        if (ret != 0)
        {
            if (_cellular_fsm_action_error_fatal(ret))
            {
                return _cellular_fsm_step_fatal(ret);
            }

            return _cellular_fsm_step_failed(ret);
        }

        ret = rg255_cmd_set_pdp_context(channel, selected_cid, effective_apn);
        if (ret != 0)
        {
            if (_cellular_fsm_action_error_fatal(ret))
            {
                return _cellular_fsm_step_fatal(ret);
            }

            return _cellular_fsm_step_failed(ret);
        }

        ret = _cellular_fsm_confirm_pdp_context(channel, configs, selected_cid, effective_apn);
        if (ret != 0)
        {
            CELLULAR_WARN("PDP context readback failed, cid=%u, error=%d", (unsigned int)selected_cid, ret);
            return _cellular_fsm_step_failed(ret);
        }
    }

    ret = cellular_runtime_set_pdp_cid(&fsm->runtime, selected_cid, now_ms);
    if (ret != 0)
    {
        return _cellular_fsm_step_fatal(ret);
    }

    ret = cellular_status_set_pdp_context(selected_cid, effective_apn);
    if (ret != 0)
    {
        cellular_runtime_clear_pdp_cid(&fsm->runtime, now_ms);
        return _cellular_fsm_step_fatal(ret);
    }

    source = configure ? (context_exists ? "updated" : "created") : "existing";
    CELLULAR_INFO("PDP context selected, operator=%s, cid=%u, apn=%s, source=%s", operator_name, (unsigned int)selected_cid, effective_apn, source);

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_ACTIVATE_PDP);
}

/**
 * @brief 已激活则复用，否则发起PDP激活后交由Status确认。
 */
cellular_fsm_step_t _cellular_fsm_state_activate_pdp(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms)
{
    uint8_t pdp_cid;
    bool    active;
    int     ret;

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

    active = false;
    ret = rg255_query_pdp_active(channel, pdp_cid, &active);
    if (ret != 0 && ret != -ENOENT)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        return _cellular_fsm_step_failed(ret);
    }

    if (active)
    {
        CELLULAR_DEBUG("PDP already active, cid=%u", (unsigned int)pdp_cid);
        return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PDP);
    }

    fsm->pdp_action_started = true;
    ret = rg255_cmd_set_pdp_active(channel, pdp_cid, true);
    if (ret != 0)
    {
        if (_cellular_fsm_action_error_fatal(ret))
        {
            return _cellular_fsm_step_fatal(ret);
        }

        CELLULAR_WARN("activate PDP returned error=%d, action=query-truth", ret);
    }
    else
    {
        CELLULAR_DEBUG("PDP activation requested, cid=%u", (unsigned int)pdp_cid);
    }

    return _cellular_fsm_step_done(CELLULAR_RUNTIME_STATE_WAIT_PDP);
}

/**
 * @brief 等待Status确认所选CID的PDP已激活。
 */
cellular_fsm_step_t _cellular_fsm_state_wait_pdp(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms)
{
    uint8_t pdp_cid;
    int     error;

    if (fsm == NULL || !cellular_runtime_get_pdp_cid(&fsm->runtime, &pdp_cid))
    {
        return _cellular_fsm_step_fatal(-EPROTO);
    }

    if (info != NULL && info->pdp.context_valid && info->pdp.cid == pdp_cid &&
        _cellular_fsm_meta_current(&info->pdp.active_meta) && info->pdp.active)
    {
        CELLULAR_DEBUG("PDP activation confirmed, cid=%u", (unsigned int)pdp_cid);
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
