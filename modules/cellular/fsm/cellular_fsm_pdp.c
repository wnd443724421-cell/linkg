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

#define CELLULAR_FSM_PDP_UNSUPPORTED_CID 8U // RG255当前不支持的PDP上下文ID

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

/****************************** PDP上下文选择 ******************************/

/**
 * @brief 根据目标APN选择最小匹配双栈CID，或选择安全的空闲CID。
 *
 * @note 不修改已经占用的其他CID；CID 8不可选择或新建。
 */
static int _cellular_fsm_select_pdp_context(const rg255_pdp_config_t *configs, size_t count, const char *effective_apn, uint8_t *selected_cid, bool *create)
{
    bool    occupied[RG255_PDP_CONTEXT_ID_MAX + 1U];
    uint8_t matched_cid;
    uint8_t cid;
    size_t  index;

    if (configs == NULL || effective_apn == NULL || effective_apn[0] == '\0' || selected_cid == NULL || create == NULL || count > RG255_PDP_CONTEXT_MAX)
    {
        return -EINVAL;
    }

    memset(occupied, 0, sizeof(occupied));
    matched_cid = 0U;
    *selected_cid = 0U;
    *create       = false;

    for (index = 0U; index < count; index++)
    {
        cid = configs[index].cid;
        if (cid < RG255_PDP_CONTEXT_ID_MIN || cid > RG255_PDP_CONTEXT_ID_MAX || occupied[cid])
        {
            return -EBADMSG;
        }

        occupied[cid] = true;

        if (cid == CELLULAR_FSM_PDP_UNSUPPORTED_CID || configs[index].pdp_type != RG255_PDP_TYPE_IPV4V6)
        {
            continue;
        }

        if (_cellular_fsm_apn_equal(configs[index].apn, effective_apn) && (matched_cid == 0U || cid < matched_cid))
        {
            matched_cid = cid;
        }
    }

    if (matched_cid != 0U)
    {
        *selected_cid = matched_cid;
        return 0;
    }

    for (cid = RG255_PDP_CONTEXT_ID_MIN; cid <= RG255_PDP_CONTEXT_ID_MAX; cid++)
    {
        if (cid != CELLULAR_FSM_PDP_UNSUPPORTED_CID && !occupied[cid])
        {
            *selected_cid = cid;
            *create       = true;
            return 0;
        }
    }

    return -ENOSPC;
}

/**
 * @brief 回读确认新创建CID的APN和双栈类型均符合目标配置。
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
 * @brief 识别SIM运营商、确定APN并选择或创建双栈PDP上下文。
 */
cellular_fsm_step_t _cellular_fsm_state_prepare_pdp(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms)
{
    rg255_pdp_config_t configs[RG255_PDP_CONTEXT_MAX];
    const char        *effective_apn;
    const char        *operator_name;
    size_t             count;
    uint8_t            selected_cid;
    uint8_t            previous_cid;
    bool               create;
    int                ret;

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

    selected_cid = 0U;
    create       = false;
    ret = _cellular_fsm_select_pdp_context(configs, count, effective_apn, &selected_cid, &create);
    if (ret != 0)
    {
        if (ret == -ENOSPC)
        {
            CELLULAR_WARN("no safe PDP CID available, operator=%s, apn=%s", operator_name, effective_apn);
        }

        return _cellular_fsm_step_failed(ret);
    }

    if ((fsm->pdp_action_started || fsm->netdev_action_started) &&
        cellular_runtime_get_pdp_cid(&fsm->runtime, &previous_cid) && previous_cid != selected_cid)
    {
        CELLULAR_WARN("PDP CID change requires prior session cleanup, old=%u, new=%u", (unsigned int)previous_cid, (unsigned int)selected_cid);
        return _cellular_fsm_step_failed(-EBUSY);
    }

    if (create)
    {
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

    CELLULAR_INFO("PDP context selected, operator=%s, cid=%u, apn=%s, source=%s", operator_name, (unsigned int)selected_cid, effective_apn, create ? "created" : "existing");

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
