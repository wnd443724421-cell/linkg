/**
 * @file switch_policy_verify.c
 * @brief LinkG冗余阶段LOSS迁出及RECOVERY + LOSS回切质量验证
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-26
 *
 * 流程：ACK启动 → 三秒采集 → LOSS及RECOVERY按不同压力积分判决
 *       → 固化VERIFY_DONE → Operation决定最终单发Plan。
 *
 * 本模块只评估质量，不修改发送计划，也不执行PlanSync。
 */

#include "switch_policy_verify.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"



/**
 * @brief 将已固定的验证结果填充为VERIFY_DONE事件。
 *
 * 所有结果字段均来自已锁存的runtime，重复交付不重新计算。
 */
static void _linkg_switch_verify_fill_event(const linkg_switch_policy_verify_runtime_t *runtime, linkg_switch_policy_event_t *event)
{
    memset(event, 0, sizeof(*event));

    event->type                                     = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
    event->peer_node_id                             = runtime->peer_node_id;
    event->peer_generation                          = runtime->peer_generation;
    event->occurred_us                              = runtime->finished_us;
    event->payload.verify_done.operation_id         = runtime->operation_id;
    event->payload.verify_done.result               = runtime->result;
    event->payload.verify_done.wifi_target_mask     = runtime->wifi_target_mask;
    event->payload.verify_done.cellular_target_mask = runtime->cellular_target_mask;
}




/**
 * @brief 固定本次验证结果并生成VERIFY_DONE事件。
 *
 * WIFI和CELLULAR自动生成整组目标掩码。
 * MIXED使用截止判决预先生成的分类目标掩码。
 * INCONCLUSIVE清空目标掩码，由Operation执行安全回退。
 *
 * 结果只允许固化一次，后续通过result_pending重复交付。
 */
static void _linkg_switch_verify_finish(linkg_switch_policy_verify_runtime_t *runtime, linkg_switch_policy_verify_result_t result, uint64_t now_us, linkg_switch_policy_verify_process_output_t *output)
{
    /****************************** 一、验证结果检查 ******************************/

    if (runtime->result_pending)
    {
        _linkg_switch_verify_fill_event(runtime, &output->event);
        output->event_ready = true;
        return;
    }

    /****************************** 二、生成业务目标掩码 ******************************/

    switch (result)
    {
        case LINKG_SWITCH_POLICY_VERIFY_WIFI:
        {
            // 本次参与验证的业务全部选择Wi-Fi。
            runtime->wifi_target_mask     = runtime->class_mask;
            runtime->cellular_target_mask = 0U;
            break;
        }

        case LINKG_SWITCH_POLICY_VERIFY_CELLULAR:
        {
            // 本次参与验证的业务全部选择Cellular。
            runtime->wifi_target_mask     = 0U;
            runtime->cellular_target_mask = runtime->class_mask;
            break;
        }

        case LINKG_SWITCH_POLICY_VERIFY_MIXED:
        {
            // 分类结果必须非空、互不重叠，且完整覆盖本次参与业务。
            if (runtime->wifi_target_mask == 0U ||
                runtime->cellular_target_mask == 0U ||
                (runtime->wifi_target_mask & runtime->cellular_target_mask) != 0U ||
                (runtime->wifi_target_mask | runtime->cellular_target_mask) != runtime->class_mask)
            {
                result = LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
            }
            break;
        }

        case LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE:
        {
            // 不确定结果不携带链路目标。
            break;
        }

        default:
        {
            // NONE及非法枚举值均不能作为有效完成结果。
            result = LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
            break;
        }
    }

    /****************************** 三、不确定结果处理 ******************************/

    if (result == LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE)
    {
        runtime->wifi_target_mask     = 0U;
        runtime->cellular_target_mask = 0U;
    }

    /****************************** 四、固定验证结果 ******************************/

    runtime->result         = result;
    runtime->finished_us    = now_us;
    runtime->result_pending = true;

    /****************************** 五、生成VERIFY_DONE事件 ******************************/

    _linkg_switch_verify_fill_event(runtime, &output->event);
    output->event_ready = true;
}


/**
 * @brief 清理当前验证会话及全部运行状态。
 *
 * 仅清理Verify运行时数据，不修改Operation事务或发送计划。
 * 由Operation在事务取消、结束或明确放弃验证时调用。
 */
void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime)
{
    if (runtime == NULL)
    {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
}

/**
 * @brief 冗余PlanSync ACK确认后启动一次质量验证会话。
 *
 * 校验事务身份、验证原因及链路观测，固定本次验证的
 * Peer、Operation、Link实例和三秒时间窗口。
 * 不执行质量判决，不修改发送计划。
 */
int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_begin_params_t *params)
{
    const linkg_switch_observation_t *observation;
    uint32_t                          operation_id;
    uint32_t                          class_index;
    uint32_t                          other_index;

    /****************************** 基础参数检查 ******************************/

    if (runtime == NULL || params == NULL || params->observation == NULL)
    {
        return -EINVAL;
    }

    if (params->peer_node_id == 0U ||
        params->peer_node_id == UINT8_MAX ||
        params->peer_generation == 0U ||
        params->class_mask == 0U ||
        (params->class_mask & ~LINKG_SWITCH_POLICY_CLASS_MASK_ALL) != 0U ||
        params->now_us == 0U ||
        params->now_us > UINT64_MAX - LINKG_SWITCH_POLICY_VERIFY_DURATION_US)
    {
        return -EINVAL;
    }

    /****************************** 验证原因检查 ******************************/

    // 仅允许LOSS进入验证，或Cellular主链路发起RECOVERY验证。
    if (params->reason != LINKG_SWITCH_POLICY_REASON_LOSS &&
        params->reason != LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        return -EINVAL;
    }

    // LOSS不是恢复事务，不允许携带历史迁移原因。
    if (params->reason == LINKG_SWITCH_POLICY_REASON_LOSS &&
        params->recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_NONE)
    {
        return -EINVAL;
    }

    // RECOVERY仅支持原始LOSS或LOAD迁移原因。
    if (params->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY &&
        params->recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_LOSS &&
        params->recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_LOAD)
    {
        return -EINVAL;
    }

    /****************************** 会话状态检查 ******************************/

    // 不允许覆盖活动验证或尚未消费的固定结果。
    if (runtime->active || runtime->result_pending)
    {
        return -EBUSY;
    }

    /****************************** 链路观测检查 ******************************/

    observation = params->observation;

    if (!observation->valid ||
        observation->peer_node_id != params->peer_node_id ||
        observation->collected_us == 0U ||
        observation->collected_us > params->now_us ||
        params->now_us - observation->collected_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
    {
        return -EINVAL;
    }

    // 冗余验证启动时必须存在两条可用物理链路。
    if (observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id == LINKG_LINK_ID_INVALID ||
        observation->cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    /****************************** Operation身份检查 ******************************/

    operation_id = LINKG_SWITCH_POLICY_OPERATION_ID_INVALID;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        // 未参与业务不允许携带Operation ID。
        if ((params->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            if (params->operation_ids[class_index] != LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
            {
                return -EINVAL;
            }

            continue;
        }

        // 参与业务必须具有有效且唯一的Operation ID。
        if (params->operation_ids[class_index] == LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
        {
            return -EINVAL;
        }

        for (other_index = 0U; other_index < class_index; other_index++)
        {
            if (params->operation_ids[class_index] == params->operation_ids[other_index])
            {
                return -EINVAL;
            }
        }

        // 首个参与业务的Operation ID作为组级事件身份。
        if (operation_id == LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
        {
            operation_id = params->operation_ids[class_index];
        }
    }

    /****************************** 初始化验证会话 ******************************/

    // 全部检查通过后清空上一次会话状态。
    memset(runtime, 0, sizeof(*runtime));

    // 固定Peer及Operation事务身份。
    runtime->peer_node_id    = params->peer_node_id;
    runtime->peer_generation = params->peer_generation;
    runtime->operation_id    = operation_id;

    memcpy(runtime->operation_ids, params->operation_ids, sizeof(runtime->operation_ids));

    // 保存本次验证范围和进入原因。
    runtime->class_mask      = params->class_mask;
    runtime->reason          = params->reason;
    runtime->recovery_reason = params->recovery_reason;

    // 固定本次验证对应的物理Link实例。
    runtime->wifi_link_id     = observation->wifi.link_id;
    runtime->cellular_link_id = observation->cellular.link_id;

    // 从冗余ACK确认时刻开始固定三秒验证。
    runtime->started_us  = params->now_us;
    runtime->deadline_us = params->now_us + LINKG_SWITCH_POLICY_VERIFY_DURATION_US;

    // LOSS及LOSS原因的恢复验证从异常积分开始。
    if (params->reason == LINKG_SWITCH_POLICY_REASON_LOSS ||
        params->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS)
    {
        runtime->wifi_loss_score = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;
    }
    else if (params->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
    {
        runtime->wifi_load_score = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;
    }

    runtime->result = LINKG_SWITCH_POLICY_VERIFY_NONE;

    // 所有会话信息初始化完成后才允许process消费。
    runtime->active = true;

    return 0;
}

/****************************** LOSS验证采样 ******************************/

/**
 * @brief 判断单项质量窗口是否属于本次ACK后的有效新样本。
 *
 * 首个有效窗口只建立时间边界，不直接计入三秒验证累计。
 */
static bool _linkg_switch_verify_loss_sample_valid(const linkg_switch_loss_observation_t *sample, const linkg_switch_policy_verify_runtime_t *runtime, uint64_t observation_us, uint64_t now_us)
{
    return sample != NULL && sample->valid &&
           sample->sample_packets != 0U &&
           sample->lost_packets <= sample->sample_packets &&
           sample->updated_us > runtime->started_us &&
           sample->updated_us <= runtime->deadline_us &&
           sample->updated_us <= observation_us &&
           sample->updated_us <= now_us &&
           now_us - sample->updated_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US;
}

/**
 * @brief 64位累计计数饱和相加，防止长时间运行后回绕。
 */
static uint64_t _linkg_switch_verify_add_u64(uint64_t a, uint64_t b)
{
    return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

/**
 * @brief 累加本次参与业务的独立Wire窗口；每个方向和业务只消费一次。
 */
static void _linkg_switch_verify_collect_wire(linkg_switch_policy_verify_runtime_t *runtime, linkg_switch_policy_verify_path_t *path, const linkg_switch_loss_observation_t samples[LINKG_TRANSPORT_CLASS_COUNT], uint32_t direction, uint64_t observation_us, uint64_t now_us)
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        sample = &samples[index];
        if (!_linkg_switch_verify_loss_sample_valid(sample, runtime, observation_us, now_us) ||
            sample->updated_us <= path->last_loss_us[index][direction])
        {
            continue;
        }

        path->last_loss_us[index][direction] = sample->updated_us;
        if (!path->loss_primed[index][direction])
        {
            path->loss_primed[index][direction] = true;
            continue;
        }

        path->lost_packets[index][direction] =
            _linkg_switch_verify_add_u64(path->lost_packets[index][direction], sample->lost_packets);
        path->sample_packets[index][direction] =
            _linkg_switch_verify_add_u64(path->sample_packets[index][direction], sample->sample_packets);
    }
}

/**
 * @brief 累加指定物理Path的三业务TX队列溢出；保留各业务方向归属。
 *
 * 只要新窗口存在确认溢出，即锁存对应类别的压力证据；不触发事务抢占。
 */
static void _linkg_switch_verify_collect_load(linkg_switch_policy_verify_runtime_t *runtime, linkg_switch_policy_verify_path_t *path, const linkg_switch_loss_observation_t samples[LINKG_TRANSPORT_CLASS_COUNT], uint32_t direction, uint64_t observation_us, uint64_t now_us)
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        sample = &samples[index];
        if (!_linkg_switch_verify_loss_sample_valid(sample, runtime, observation_us, now_us) ||
            sample->updated_us <= path->last_load_us[index][direction])
        {
            continue;
        }

        path->last_load_us[index][direction] = sample->updated_us;
        if (!path->load_primed[index][direction])
        {
            path->load_primed[index][direction] = true;
            continue;
        }

        path->load_observed[index][direction]         = true;
        path->load_valid_this_round[index][direction] = true;
        path->load_lost_packets[index][direction]     = _linkg_switch_verify_add_u64(path->load_lost_packets[index][direction], sample->lost_packets);
        path->load_sample_packets[index][direction]   = _linkg_switch_verify_add_u64(path->load_sample_packets[index][direction], sample->sample_packets);

        if (sample->lost_packets != 0U)
        {
            path->queue_overflow                   = true;
            path->overflow_class_mask             |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
            path->overflow_class_mask_this_round  |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
        }
    }
}

/**
 * @brief 记录Wi-Fi整链路Load窗口，用于识别所有业务共有的TX溢出压力。
 *
 * 不把全局溢出分配给某一类业务；首次有效窗口仅建立ACK后采样边界。
 */
static void _linkg_switch_verify_collect_global_load(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_loss_observation_t *sample, uint32_t direction, uint64_t observation_us, uint64_t now_us)
{
    linkg_switch_policy_verify_load_window_t *window;

    if (!_linkg_switch_verify_loss_sample_valid(sample, runtime, observation_us, now_us))
    {
        return;
    }

    window = &runtime->wifi_global_load[direction];
    if (sample->updated_us <= window->last_updated_us)
    {
        return;
    }

    window->last_updated_us = sample->updated_us;
    if (!window->primed)
    {
        window->primed = true;
        return;
    }

    window->observed         = true;
    window->valid_this_round = true;
    if (sample->lost_packets != 0U)
    {
        window->overflow_this_round = true;
        window->overflow_seen       = true;
    }
}

/**
 * @brief 记录Wi-Fi上下行HCC原始队列压力；不把设备级HCC误记为某业务OVF。
 */
static void _linkg_switch_verify_collect_wifi_hcc(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_wifi_hcc_observation_t *hcc, uint32_t direction, uint64_t observation_us, uint64_t now_us)
{
    linkg_switch_policy_verify_hcc_window_t *window;

    if (!hcc->valid ||
        hcc->updated_us <= runtime->started_us ||
        hcc->updated_us > runtime->deadline_us ||
        hcc->updated_us > observation_us ||
        hcc->updated_us > now_us ||
        now_us - hcc->updated_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
    {
        return;
    }

    window = &runtime->wifi_hcc[direction];
    if (hcc->updated_us <= window->last_updated_us)
    {
        return;
    }

    window->last_updated_us  = hcc->updated_us;
    window->valid_this_round = true;
    if (!window->initialized)
    {
        window->last_off_count = hcc->flowctrl_off_count;
        window->initialized    = true;
    }
    else if (hcc->flowctrl_off_count >= window->last_off_count)
    {
        window->off_delta_this_round = hcc->flowctrl_off_count - window->last_off_count;
        window->off_events = _linkg_switch_verify_add_u64(
            window->off_events, window->off_delta_this_round);
    }

    // 计数回退说明驱动统计曾重建；仅更新基线，不产生伪新增事件。
    window->last_off_count = hcc->flowctrl_off_count;
    if (!hcc->tx_allowed)
    {
        window->tx_blocked_seen       = true;
        window->tx_blocked_this_round = true;
    }

    if (hcc->vo_queue_length > window->max_vo_queue_length)
    {
        window->max_vo_queue_length = hcc->vo_queue_length;
    }
    if (hcc->vi_queue_length > window->max_vi_queue_length)
    {
        window->max_vi_queue_length = hcc->vi_queue_length;
    }
    if (hcc->be_queue_length > window->max_be_queue_length)
    {
        window->max_be_queue_length = hcc->be_queue_length;
    }
}

/**
 * @brief 从Wi-Fi全局Wire窗口生成一个方向的恢复评分增量。
 *
 * BAD采用QUALITY相同的0.5%与50%/sqrt(N)门槛；GOOD仅使用零丢包窗口。
 * 小窗口累积到20包后再评价，避免把少量Probe误当成可靠恢复证据。
 */
static int _linkg_switch_verify_wifi_score_direction(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_loss_observation_t *sample, uint32_t direction, uint64_t observation_us, uint64_t now_us, bool score_enabled)
{
    linkg_switch_policy_verify_score_window_t *window;
    uint64_t                                   samples;
    uint64_t                                   lost;
    bool                                       bad;

    window = &runtime->wifi_score[direction];

    // 负载压力优先或负载情况未知时，丢弃未完成批次，防止下轮混入旧窗口。
    if (!score_enabled)
    {
        window->sample_packets = 0U;
        window->lost_packets   = 0U;
    }

    if (!_linkg_switch_verify_loss_sample_valid(sample, runtime, observation_us, now_us) ||
        sample->updated_us <= window->last_updated_us)
    {
        return 0;
    }

    // 即使不评分，也必须消费本轮Wire源时间，避免下一轮重复累加。
    window->last_updated_us = sample->updated_us;
    if (!window->primed)
    {
        window->primed = true;
        return 0;
    }

    if (!score_enabled)
    {
        return 0;
    }

    window->sample_packets = _linkg_switch_verify_add_u64(window->sample_packets, sample->sample_packets);
    window->lost_packets   = _linkg_switch_verify_add_u64(window->lost_packets, sample->lost_packets);

    if (window->sample_packets < LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES)
    {
        return 0;
    }

    samples = window->sample_packets;
    lost    = window->lost_packets;
    window->sample_packets = 0U;
    window->lost_packets   = 0U;

    // 防止溢出或非法观测造成错误评分。
    if (lost > samples)
    {
        return 0;
    }

    bad = lost >= 3U &&
          lost >= samples / 200ULL + (samples % 200ULL != 0U ? 1ULL : 0ULL) &&
          (lost >= 2147483648ULL || 4ULL * lost * lost >= samples);

    if (bad)
    {
        return 1;
    }

    return lost == 0U ? -1 : 0;
}

/**
 * @brief 记录参与业务的Cellular周期Probe质量；三业务独立去重。
 */
static void _linkg_switch_verify_collect_cellular_probe(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_probe_observation_t probes[LINKG_TRANSPORT_CLASS_COUNT], uint64_t observation_us, uint64_t now_us)
{
    const linkg_switch_probe_observation_t *probe;
    uint32_t                                index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(index)) == 0U)
        {
            continue;
        }

        probe = &probes[index];
        if (!probe->valid ||
            probe->updated_us <= runtime->started_us ||
            probe->updated_us > runtime->deadline_us ||
            probe->updated_us > observation_us ||
            probe->updated_us > now_us ||
            now_us - probe->updated_us > LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US ||
            probe->updated_us <= runtime->cellular_last_probe_us[index])
        {
            continue;
        }

        runtime->cellular_last_probe_us[index] = probe->updated_us;
        if (!probe->reachable)
        {
            runtime->cellular_probe_unreachable_mask |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
            continue;
        }

        runtime->cellular_last_good_probe_us[index] = probe->updated_us;
        runtime->cellular_rtt_sum_us[index] =
            _linkg_switch_verify_add_u64(runtime->cellular_rtt_sum_us[index], probe->rtt_us);

        if (runtime->cellular_rtt_count[index] != UINT32_MAX)
        {
            runtime->cellular_rtt_count[index]++;
        }
        if (probe->rtt_us > runtime->cellular_max_rtt_us[index])
        {
            runtime->cellular_max_rtt_us[index] = probe->rtt_us;
        }
    }
}

/**
 * @brief 清除当前轮临时负载状态，不影响三秒验证累计统计。
 */
static void _linkg_switch_verify_reset_round(linkg_switch_policy_verify_runtime_t *runtime)
{
    uint32_t class_index;
    uint32_t direction;

    runtime->wifi_pressure_this_round                 = false;
    runtime->wifi_pressure_known_this_round           = false;
    runtime->wifi.overflow_class_mask_this_round      = 0U;
    runtime->cellular.overflow_class_mask_this_round  = 0U;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        runtime->wifi_hcc[direction].valid_this_round             = false;
        runtime->wifi_hcc[direction].off_delta_this_round         = 0U;
        runtime->wifi_hcc[direction].tx_blocked_this_round        = false;
        runtime->wifi_global_load[direction].valid_this_round    = false;
        runtime->wifi_global_load[direction].overflow_this_round = false;

        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            runtime->wifi.load_valid_this_round[class_index][direction]     = false;
            runtime->cellular.load_valid_this_round[class_index][direction] = false;
        }
    }
}

/**
 * @brief 判断历史有效HCC窗口是否足够新，可用于本轮无线评分的负载排除。
 */
static bool _linkg_switch_verify_hcc_recent(const linkg_switch_policy_verify_hcc_window_t *window, uint64_t now_us)
{
    return window->initialized && window->last_updated_us != 0U &&
           now_us >= window->last_updated_us &&
           now_us - window->last_updated_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US;
}

/**
 * @brief 判断一方向是否有最近的有效Wi-Fi Load样本，防止把缺失统计当作无拥塞。
 */
static bool _linkg_switch_verify_wifi_load_recent(const linkg_switch_policy_verify_runtime_t *runtime, uint32_t direction, uint64_t now_us)
{
    const linkg_switch_policy_verify_load_window_t *global;
    uint32_t                                       class_index;
    uint64_t                                       updated_us;

    global = &runtime->wifi_global_load[direction];
    updated_us = global->last_updated_us;
    if (global->observed && global->valid_this_round)
    {
        return true;
    }

    // 已建立过基线且来源近期仍有更新，视作有Load覆盖；不使用历史OVF判断本轮压力。
    if (global->observed && updated_us != 0U && now_us >= updated_us &&
        now_us - updated_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
    {
        return true;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            continue;
        }

        updated_us = runtime->wifi.last_load_us[class_index][direction];
        if (runtime->wifi.load_observed[class_index][direction] &&
            updated_us != 0U && now_us >= updated_us &&
            now_us - updated_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 收集RECOVERY + LOSS期间参与业务的Wi-Fi Probe最新可达性。
 *
 * Probe按业务独立去重。最新失败可以覆盖先前成功状态，最新成功也可以
 * 恢复对应业务的可达标记；三秒截止时再判断成功结果的新鲜度。
 */
static void _linkg_switch_verify_collect_wifi_probe(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_probe_observation_t probes[LINKG_TRANSPORT_CLASS_COUNT], uint64_t observation_us, uint64_t now_us)
{
    const linkg_switch_probe_observation_t *probe;
    uint32_t                                class_index;
    uint32_t                                bit;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        bit = LINKG_SWITCH_POLICY_CLASS_BIT(class_index);
        if ((runtime->class_mask & bit) == 0U)
        {
            continue;
        }

        probe = &probes[class_index];
        if (!probe->valid ||
            probe->updated_us <= runtime->started_us ||
            probe->updated_us > runtime->deadline_us ||
            probe->updated_us > observation_us ||
            probe->updated_us > now_us ||
            now_us - probe->updated_us > LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US ||
            probe->updated_us <= runtime->wifi_last_probe_us[class_index])
        {
            continue;
        }

        runtime->wifi_last_probe_us[class_index] = probe->updated_us;
        runtime->wifi_probe_seen_mask          |= bit;

        if (probe->reachable)
        {
            runtime->wifi_last_good_probe_us[class_index] = probe->updated_us;
            runtime->wifi_probe_reachable_mask            |= bit;
            runtime->wifi_rtt_sum_us[class_index] = _linkg_switch_verify_add_u64(runtime->wifi_rtt_sum_us[class_index], probe->rtt_us);

            if (runtime->wifi_rtt_count[class_index] != UINT32_MAX)
            {
                runtime->wifi_rtt_count[class_index]++;
            }
        }
        else
        {
            runtime->wifi_probe_reachable_mask &= ~bit;
        }
    }
}

/**
 * @brief LOSS及RECOVERY + LOSS验证阶段的双链路采样与Wi-Fi恢复积分。
 *
 * 两种验证共用HCC、Load、Wire及Probe采样。
 * LOSS：负载压力减分，无线BAD加分、GOOD减分。
 * RECOVERY：负载压力加分，无线BAD加分、GOOD减分。
 * 本函数不执行截止判决、不修改发送计划。
 */
static void _linkg_switch_verify_collect_loss_common(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_observation_t *observation, uint64_t now_us, bool recovery)
{
    const linkg_switch_loss_observation_t *wifi_wire[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    bool                                   wifi_pressure;
    bool                                   hcc_known;
    bool                                   load_known;
    bool                                   score_enabled;
    int                                    uplink_delta;
    int                                    downlink_delta;
    uint32_t                               bad_count;
    uint32_t                               direction;
    uint32_t                               next_score;

    /****************************** 一、采样轮次检查 ******************************/

    // 同一份Observation禁止重复消费或重复更新积分。
    if (observation->collected_us <= runtime->last_processed_observation_us)
    {
        return;
    }

    runtime->last_processed_observation_us = observation->collected_us;
    _linkg_switch_verify_reset_round(runtime);

    /****************************** 二、Wi-Fi HCC及双链路Load采集 ******************************/

    // 优先采集Wi-Fi上下行HCC流控状态。
    _linkg_switch_verify_collect_wifi_hcc(runtime, &observation->wifi.uplink_hcc, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_wifi_hcc(runtime, &observation->wifi.downlink_hcc, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    // 采集Wi-Fi整链路上下行队列溢出。
    _linkg_switch_verify_collect_global_load(runtime, &observation->wifi.loss.uplink_load, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_global_load(runtime, &observation->wifi.loss.downlink_load, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    // 采集Wi-Fi和Cellular三业务上下行Load。
    _linkg_switch_verify_collect_load(runtime, &runtime->wifi, observation->wifi.loss.uplink_load_classes, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_load(runtime, &runtime->wifi, observation->wifi.loss.downlink_load_classes, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_load(runtime, &runtime->cellular, observation->cellular.loss.uplink_load_classes, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_load(runtime, &runtime->cellular, observation->cellular.loss.downlink_load_classes, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    /****************************** 三、双链路分类Wire采集 ******************************/

    // 无论本轮Wi-Fi是否存在负载压力，都累计双链路分类Wire供截止时比较。
    _linkg_switch_verify_collect_wire(runtime, &runtime->wifi, observation->wifi.loss.uplink_classes, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_wire(runtime, &runtime->wifi, observation->wifi.loss.downlink_classes, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_wire(runtime, &runtime->cellular, observation->cellular.loss.uplink_classes, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_wire(runtime, &runtime->cellular, observation->cellular.loss.downlink_classes, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    /****************************** 四、Wi-Fi负载压力检查 ******************************/

    // 本轮发生整链路或分类队列溢出，优先按负载压力处理。
    wifi_pressure = runtime->wifi.overflow_class_mask_this_round != 0U ||
                    runtime->wifi_global_load[LINKG_SWITCH_POLICY_VERIFY_UPLINK].overflow_this_round ||
                    runtime->wifi_global_load[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK].overflow_this_round;

    // 合并Wi-Fi上下行HCC新增流控事件及当前TX阻塞状态。
    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        wifi_pressure = wifi_pressure ||
                        runtime->wifi_hcc[direction].off_delta_this_round != 0U ||
                        runtime->wifi_hcc[direction].tx_blocked_this_round;
    }

    runtime->wifi_pressure_this_round = wifi_pressure;

    /****************************** 五、Wi-Fi负载观测有效性 ******************************/

    // HCC上下行必须存在近期有效观测，才能确认流控状态。
    hcc_known = _linkg_switch_verify_hcc_recent(&runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_UPLINK], now_us) &&
                _linkg_switch_verify_hcc_recent(&runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK], now_us);

    load_known = true;

    // 整链路Wire分别用于上行和下行恢复积分。
    wifi_wire[LINKG_SWITCH_POLICY_VERIFY_UPLINK]   = &observation->wifi.loss.uplink;
    wifi_wire[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK] = &observation->wifi.loss.downlink;

    // 有效Wire方向必须具备近期Load观测，避免错误归因为无线丢包。
    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        if (_linkg_switch_verify_loss_sample_valid(wifi_wire[direction], runtime, observation->collected_us, now_us) &&
            !_linkg_switch_verify_wifi_load_recent(runtime, direction, now_us))
        {
            load_known = false;
        }
    }

    runtime->wifi_pressure_known_this_round = hcc_known && load_known;
    score_enabled                           = !wifi_pressure && runtime->wifi_pressure_known_this_round;

    /****************************** 六、Wi-Fi整链路恢复积分 ******************************/

    // 负载压力存在或观测不足时，只消费Wire窗口，不执行BAD/GOOD评分。
    uplink_delta   = _linkg_switch_verify_wifi_score_direction(runtime, wifi_wire[LINKG_SWITCH_POLICY_VERIFY_UPLINK], LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us, score_enabled);
    downlink_delta = _linkg_switch_verify_wifi_score_direction(runtime, wifi_wire[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK], LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us, score_enabled);

    if (wifi_pressure)
    {
        if (recovery)
        {
            // RECOVERY：Wi-Fi仍有流控或队列溢出，最多加1分。
            if (runtime->wifi_loss_score < LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE)
            {
                runtime->wifi_loss_score++;
            }
        }
        else if (runtime->wifi_loss_score > 0U)
        {
            // LOSS：负载压力优先归因，本轮最多减1分。
            runtime->wifi_loss_score--;
        }
    }
    else if (score_enabled)
    {
        // 无负载压力时，Wire BAD优先于GOOD，上下行BAD最多加2分。
        bad_count = (uplink_delta > 0 ? 1U : 0U) +
                    (downlink_delta > 0 ? 1U : 0U);

        if (bad_count != 0U)
        {
            next_score = (uint32_t)runtime->wifi_loss_score + bad_count;

            runtime->wifi_loss_score = (uint8_t)(next_score > LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE ?
                                                  LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE : next_score);
        }
        else if ((uplink_delta < 0 || downlink_delta < 0) &&
                 runtime->wifi_loss_score > 0U)
        {
            // 没有BAD且至少一个方向GOOD，同一轮最多减1分。
            runtime->wifi_loss_score--;
        }
    }

    /****************************** 七、双链路Probe采集 ******************************/

    // 持续记录Cellular分类Probe，供三秒截止后的双链路质量比较。
    _linkg_switch_verify_collect_cellular_probe(runtime, observation->cellular.probe, observation->collected_us, now_us);

    // LOSS及RECOVERY都需要比较两个Path的平均Probe RTT。
    _linkg_switch_verify_collect_wifi_probe(runtime, observation->wifi.probe, observation->collected_us, now_us);
}

/**
 * @brief LOSS迁出验证期间采集双链路质量；负载压力减分。
 */
static void _linkg_switch_verify_collect_loss(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_observation_t *observation, uint64_t now_us)
{
    _linkg_switch_verify_collect_loss_common(runtime, observation, now_us, false);
}

/**
 * @brief RECOVERY + LOSS回切验证期间采集双链路质量；负载压力加分。
 *
 * 积分为整链路组级数据，不为每类业务单独维护积分；分类样本单独累计。
 */
static void _linkg_switch_verify_collect_recovery_loss(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_observation_t *observation, uint64_t now_us)
{
    _linkg_switch_verify_collect_loss_common(runtime, observation, now_us, true);
}

/**
 * @brief RECOVERY + LOAD验证期间采集Wi-Fi流控事件、队列OVF及实际发送负载。
 *
 * 只使用HCC新增FLOWCTRL_OFF和TX队列OVF判断负载BAD，不使用HCC队列水位。
 * BAD每轮最多加1分；只有新鲜双方向HCC及新增Wi-Fi业务流量证据均有效，
 * 且本轮无压力时才减1分。未知观测不计分。
 *
 * 本函数只做周期采样，不生成VERIFY_DONE或修改Plan。
 */
static void _linkg_switch_verify_collect_recovery_load(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_observation_t *observation, uint64_t now_us)
{
    const linkg_switch_traffic_observation_t *traffic;
    bool                                      wifi_pressure;
    bool                                      hcc_ready;
    bool                                      traffic_active;
    uint32_t                                  direction;

    /****************************** 一、采样轮次去重 ******************************/

    if (observation->collected_us <= runtime->last_processed_observation_us)
    {
        return;
    }

    runtime->last_processed_observation_us = observation->collected_us;
    _linkg_switch_verify_reset_round(runtime);

    /****************************** 二、Wi-Fi HCC及队列溢出采集 ******************************/

    _linkg_switch_verify_collect_wifi_hcc(runtime, &observation->wifi.uplink_hcc, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_wifi_hcc(runtime, &observation->wifi.downlink_hcc, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    _linkg_switch_verify_collect_global_load(runtime, &observation->wifi.loss.uplink_load, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_global_load(runtime, &observation->wifi.loss.downlink_load, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    _linkg_switch_verify_collect_load(runtime, &runtime->wifi, observation->wifi.loss.uplink_load_classes, LINKG_SWITCH_POLICY_VERIFY_UPLINK, observation->collected_us, now_us);
    _linkg_switch_verify_collect_load(runtime, &runtime->wifi, observation->wifi.loss.downlink_load_classes, LINKG_SWITCH_POLICY_VERIFY_DOWNLINK, observation->collected_us, now_us);

    /****************************** 三、新增流控与负载溢出判定 ******************************/

    wifi_pressure = runtime->wifi.overflow_class_mask_this_round != 0U ||
                    runtime->wifi_global_load[LINKG_SWITCH_POLICY_VERIFY_UPLINK].overflow_this_round ||
                    runtime->wifi_global_load[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK].overflow_this_round;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        // 只统计新增FLOWCTRL_OFF，不将持续tx_allowed=false重复计为新事件。
        wifi_pressure = wifi_pressure || runtime->wifi_hcc[direction].off_delta_this_round != 0U;
    }

    runtime->wifi_pressure_this_round = wifi_pressure;

    /****************************** 四、Wi-Fi实际负载采样 ******************************/

    traffic        = &observation->wifi.traffic;
    traffic_active = false;

    // Wi-Fi Path总收发速率仅用于防止空载时通过；不等价于指定业务USER_DATA活跃。
    if (traffic->valid &&
        traffic->updated_us > runtime->started_us &&
        traffic->updated_us <= runtime->deadline_us &&
        traffic->updated_us <= observation->collected_us &&
        traffic->updated_us <= now_us &&
        now_us - traffic->updated_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US &&
        traffic->updated_us > runtime->wifi_load_last_traffic_us)
    {
        runtime->wifi_load_last_traffic_us = traffic->updated_us;
        traffic_active = traffic->tx_bps != 0U || traffic->rx_bps != 0U;

        if (traffic_active)
        {
            runtime->wifi_traffic_seen = true;
            runtime->wifi_tx_active |= traffic->tx_bps != 0U;
            runtime->wifi_rx_active |= traffic->rx_bps != 0U;
        }
    }

    /****************************** 五、Wi-Fi负载恢复积分 ******************************/

    if (wifi_pressure)
    {
        // 本轮出现OFF或OVF，最多加1分，不区分同轮新增事件数量。
        if (runtime->wifi_load_score < LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE)
        {
            runtime->wifi_load_score++;
        }

        return;
    }

    // 必须本轮两方向均取得新HCC样本，不能用旧OFF计数反复确认良好窗口。
    hcc_ready = runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_UPLINK].valid_this_round &&
                runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK].valid_this_round &&
                runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_UPLINK].initialized &&
                runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK].initialized;

    // 首个双向完整HCC窗口只确认计数基线，不将可能跨ACK的首轮观测视为GOOD。
    if (!runtime->wifi_load_hcc_primed)
    {
        runtime->wifi_load_hcc_primed = hcc_ready;
        return;
    }

    // 没有新HCC或没有新流量活动时不减分，防止空闲Wi-Fi虚假恢复。
    if (hcc_ready && traffic_active && runtime->wifi_load_score > 0U)
    {
        runtime->wifi_load_score--;
    }
}

/**
 * @brief RECOVERY + LOAD三秒结束后只根据Wi-Fi负载恢复积分决定整组回切。
 *
 * FLOWCTRL_OFF允许出现，只要最终积分严格小于3即可恢复；没有有效业务
 * 流量观测或HCC过期时不将空载状态视为承载恢复。
 */
static linkg_switch_policy_verify_result_t _linkg_switch_verify_decide_recovery_load(const linkg_switch_policy_verify_runtime_t *runtime, uint64_t now_us)
{
    if (runtime->wifi_load_score < LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE &&
        runtime->wifi_traffic_seen &&
        _linkg_switch_verify_hcc_recent(&runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_UPLINK], now_us) &&
        _linkg_switch_verify_hcc_recent(&runtime->wifi_hcc[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK], now_us))
    {
        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
}

/****************************** LOSS分类截止判决 ******************************/

/**
 * @brief 检查某业务的最近一次Probe是否仍可达，并读取验证期平均RTT。
 *
 * 最新失败会覆盖历史成功资格；平均值由验证期所有有效成功Probe组成。
 */
static bool _linkg_switch_verify_probe_average(const linkg_switch_policy_verify_runtime_t *runtime, uint32_t class_index, bool wifi, uint64_t now_us, uint64_t *average_rtt_us)
{
    uint64_t last_probe_us;
    uint64_t last_good_us;
    uint64_t rtt_sum_us;
    uint32_t rtt_count;

    if (wifi)
    {
        last_probe_us = runtime->wifi_last_probe_us[class_index];
        last_good_us  = runtime->wifi_last_good_probe_us[class_index];
        rtt_sum_us    = runtime->wifi_rtt_sum_us[class_index];
        rtt_count     = runtime->wifi_rtt_count[class_index];

        if ((runtime->wifi_probe_reachable_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            return false;
        }
    }
    else
    {
        last_probe_us = runtime->cellular_last_probe_us[class_index];
        last_good_us  = runtime->cellular_last_good_probe_us[class_index];
        rtt_sum_us    = runtime->cellular_rtt_sum_us[class_index];
        rtt_count     = runtime->cellular_rtt_count[class_index];
    }

    if (last_good_us == 0U || last_good_us != last_probe_us ||
        last_good_us > now_us ||
        now_us - last_good_us > LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US ||
        rtt_count == 0U)
    {
        return false;
    }

    *average_rtt_us = rtt_sum_us / rtt_count;
    return true;
}

/**
 * @brief 计算一个Path指定业务的上下行综合丢包率（0~1）。
 *
 * 综合损失=1-(1-Load丢包率)*(1-Wire丢包率)。
 * 全部发送尝试均因Queue Overflow丢弃时允许没有Wire样本。
 * 其他方向要求Wire/Load均有样本，最终取两方向较差值。
 * 不要求精准端到端对齐，但不能把缺失样本视为零丢包。
 */
static bool _linkg_switch_verify_class_loss_rate(const linkg_switch_policy_verify_path_t *path, uint32_t class_index, double *loss_rate)
{
    uint64_t wire_samples;
    uint64_t wire_lost;
    uint64_t load_samples;
    uint64_t load_lost;
    uint64_t total_wire_samples;
    uint64_t total_load_samples;
    double   wire_rate;
    double   load_rate;
    double   combined;
    double   worst;
    bool     observed;
    uint32_t direction;

    total_wire_samples = 0U;
    total_load_samples = 0U;
    worst              = 0.0;
    observed           = false;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        // Load累计确认此方向全部发送尝试均已OVF时，Wire无包可观察。
        // 只要Load样本达到最低门槛，即可确认该方向综合丢包率100%。
        load_samples = path->load_sample_packets[class_index][direction];
        load_lost    = path->load_lost_packets[class_index][direction];
        if (load_samples >= LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES &&
            load_lost == load_samples)
        {
            *loss_rate = 1.0;
            return true;
        }

        wire_samples = path->sample_packets[class_index][direction];
        wire_lost    = path->lost_packets[class_index][direction];
        load_samples = path->load_sample_packets[class_index][direction];
        load_lost    = path->load_lost_packets[class_index][direction];

        if (wire_samples == 0U && load_samples == 0U)
        {
            continue;
        }

        if (wire_samples == 0U || load_samples == 0U ||
            wire_lost > wire_samples || load_lost > load_samples)
        {
            return false;
        }

        total_wire_samples = _linkg_switch_verify_add_u64(total_wire_samples, wire_samples);
        total_load_samples = _linkg_switch_verify_add_u64(total_load_samples, load_samples);

        wire_rate = (double)wire_lost / (double)wire_samples;
        load_rate = (double)load_lost / (double)load_samples;
        combined  = load_rate + wire_rate - load_rate * wire_rate;

        if (!observed || combined > worst)
        {
            worst = combined;
        }

        observed = true;
    }

    if (!observed || total_wire_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES ||
        total_load_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES)
    {
        return false;
    }

    *loss_rate = worst;
    return true;
}

/**
 * @brief 逐业务判定Wi-Fi或Cellular的可用质量目标。
 *
 * LOSS：保留Wi-Fi不需要Probe；仅选择Cellular时要求其平均RTT<250ms。
 * RECOVERY + LOSS：恢复Wi-Fi必须平均Probe RTT严格低于Cellular；
 * 维持Cellular只要求其平均Probe RTT<250ms。
 * 积分<3时不要求分类Wire比较。
 */
static linkg_switch_policy_verify_result_t _linkg_switch_verify_decide_active_class(linkg_switch_policy_verify_runtime_t *runtime, uint32_t class_index, uint64_t now_us)
{
    uint64_t wifi_rtt_us;
    uint64_t cellular_rtt_us;
    double   wifi_loss_rate;
    double   cellular_loss_rate;
    bool     recovery;
    bool     wifi_probe_ok;
    bool     cellular_probe_ok;
    bool     wifi_allowed;
    bool     cellular_allowed;
    bool     wifi_quality_known;
    bool     cellular_quality_known;

    recovery        = runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY;
    wifi_rtt_us     = 0U;
    cellular_rtt_us = 0U;

    // LOSS迁出无需Wi-Fi Probe：正在承载的Wi-Fi本身是保守目标。
    cellular_probe_ok = _linkg_switch_verify_probe_average(runtime, class_index, false, now_us, &cellular_rtt_us);
    cellular_allowed  = cellular_probe_ok && cellular_rtt_us < LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US;
    wifi_allowed      = true;

    if (recovery)
    {
        // 仅RECOVERY回切要求Wi-Fi平均Probe RTT严格小于Cellular。
        wifi_probe_ok = _linkg_switch_verify_probe_average(runtime, class_index, true, now_us, &wifi_rtt_us);
        wifi_allowed  = wifi_probe_ok && cellular_probe_ok && wifi_rtt_us < cellular_rtt_us;
    }

    if (runtime->wifi_loss_score < LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE)
    {
        if (wifi_allowed)
        {
            return LINKG_SWITCH_POLICY_VERIFY_WIFI;
        }

        return cellular_allowed ? LINKG_SWITCH_POLICY_VERIFY_CELLULAR : LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    wifi_quality_known = _linkg_switch_verify_class_loss_rate(&runtime->wifi, class_index, &wifi_loss_rate);
    cellular_quality_known = _linkg_switch_verify_class_loss_rate(&runtime->cellular, class_index, &cellular_loss_rate);

    if (!wifi_quality_known || !cellular_quality_known)
    {
        // LOSS样本不足不迁出；RECOVERY没有足够Wi-Fi恢复证据时维持Cellular。
        if (!recovery)
        {
            return LINKG_SWITCH_POLICY_VERIFY_WIFI;
        }

        return cellular_allowed ? LINKG_SWITCH_POLICY_VERIFY_CELLULAR : LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    if (cellular_loss_rate < wifi_loss_rate && cellular_allowed)
    {
        return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
    }

    if (wifi_allowed)
    {
        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    return cellular_allowed ? LINKG_SWITCH_POLICY_VERIFY_CELLULAR : LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
}

/**
 * @brief 按REALTIME、VIDEO、DATA对本次参与的业务形成完整分类结果。
 *
 * 已确认空闲的业务直接选择Wi-Fi，不比较丢包或Probe，也不阻止后续。
 * 高优先级一旦选择Cellular，低优先级参与业务无条件跟随。
 * 未知活跃性不能当空闲，分类证据不充分时返回INCONCLUSIVE。
 */
static linkg_switch_policy_verify_result_t _linkg_switch_verify_decide_loss_classes(linkg_switch_policy_verify_runtime_t *runtime, uint64_t now_us)
{
    linkg_switch_policy_verify_result_t choice;
    uint32_t                             class_index;
    uint32_t                             bit;
    bool                                 force_cellular;
    bool                                 recovery;

    runtime->wifi_target_mask        = 0U;
    runtime->cellular_target_mask    = 0U;
    runtime->comparable_class_mask   = 0U;
    runtime->cellular_preferred_mask = 0U;

    // 不允许将Transport统计读取失败解释为全部业务空闲。
    if (!runtime->class_activity_valid)
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    recovery       = runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY;
    force_cellular = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        bit = LINKG_SWITCH_POLICY_CLASS_BIT(class_index);
        if ((runtime->class_mask & bit) == 0U)
        {
            continue;
        }

        if (force_cellular)
        {
            runtime->cellular_target_mask |= bit;
            continue;
        }

        // 确认无USER_DATA的高优先级业务不阻挡低优先级恢复。
        if ((runtime->active_class_mask & bit) == 0U)
        {
            runtime->wifi_target_mask |= bit;
            continue;
        }

        choice = _linkg_switch_verify_decide_active_class(runtime, class_index, now_us);

        if (choice == LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE)
        {
            // 不确定只回退当前类别，而非抛弃整组已知分类结果。
            // LOSS保留原Wi-Fi；RECOVERY保留原Cellular。
            choice = recovery ? LINKG_SWITCH_POLICY_VERIFY_CELLULAR : LINKG_SWITCH_POLICY_VERIFY_WIFI;
        }
        else
        {
            // 表示此类已产生明确可用的判决，不代表必然进行过Wire比较。
            runtime->comparable_class_mask |= bit;
        }

        if (choice == LINKG_SWITCH_POLICY_VERIFY_CELLULAR)
        {
            runtime->cellular_target_mask |= bit;
            // 包括保守维持Cellular，后续参与业务须遵循QoS级联。
            force_cellular = true;
        }
        else
        {
            runtime->wifi_target_mask |= bit;
        }
    }

    if (runtime->wifi_target_mask == runtime->class_mask)
    {
        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    if (runtime->cellular_target_mask == runtime->class_mask)
    {
        return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
    }

    return LINKG_SWITCH_POLICY_VERIFY_MIXED;
}

/**
 * @brief 周期处理双链路质量验证并交付验证结果。
 *
 * 验证期间持续采集链路观测，三秒到期后按验证原因执行质量判决。
 * 最终结果只计算一次，并持续交付VERIFY_DONE直至Operation消费。
 */
int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_process_params_t *params, linkg_switch_policy_verify_process_output_t *output)
{
    const linkg_switch_observation_t *observation;
    bool                              observation_fresh;
    bool                              paths_usable;
    bool                              wifi_link_changed;
    bool                              cellular_link_changed;


    /****************************** 一、参数与会话检查 ******************************/

    // 检查输入参数。
    if (runtime == NULL || params == NULL || output == NULL ||
        params->observation == NULL || params->now_us == 0U)
    {
        return -EINVAL;
    }

    // 清空本轮事件输出，避免保留上次调用的结果。
    memset(output, 0, sizeof(*output));

    // 当前没有活动验证会话，无需处理。
    if (!runtime->active)
    {
        return 0;
    }

    // 拒绝其他Peer的观测或早于验证启动时间的调用。
    if (params->observation->peer_node_id != runtime->peer_node_id ||
        params->now_us < runtime->started_us)
    {
        return -ESTALE;
    }

    // 已固定结果直接重复交付，不重新采样或计算。
    if (runtime->result_pending)
    {
        _linkg_switch_verify_fill_event(runtime, &output->event);
        output->event_ready = true;
        return 0;
    }


    /****************************** 二、观测与链路检查 ******************************/

    observation = params->observation;

    // 仅允许验证启动后、截止时间内生成的新鲜观测参与处理。
    observation_fresh = observation->valid &&
                        observation->collected_us > runtime->started_us &&
                        observation->collected_us <= runtime->deadline_us &&
                        observation->collected_us <= params->now_us &&
                        params->now_us - observation->collected_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US;

    paths_usable = false;

    if (observation_fresh)
    {
        /****************************** Link实例连续性 ******************************/

        // 仅将已确认的新Link实例视为变化，UNKNOWN状态下的无效ID不直接判定断链。
        wifi_link_changed     = observation->wifi.link_id != LINKG_LINK_ID_INVALID && observation->wifi.link_id != runtime->wifi_link_id;
        cellular_link_changed = observation->cellular.link_id != LINKG_LINK_ID_INVALID && observation->cellular.link_id != runtime->cellular_link_id;

        if (wifi_link_changed || cellular_link_changed)
        {
            // 原始Link实例已经改变，旧验证样本不能继续用于本次质量判断。
            runtime->path_interrupted = true;

            _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE, params->now_us, output);
            return 0;
        }

        /****************************** 明确链路故障 ******************************/

        if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE || observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
        {
            runtime->path_interrupted = true;

            // Wi-Fi已明确不可用，Cellular仍可用，提前选择Cellular。
            if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE &&
                observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE)
            {
                _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_CELLULAR, params->now_us, output);
                return 0;
            }

            // Cellular已明确不可用，Wi-Fi仍可用，提前选择Wi-Fi。
            if (observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE &&
                observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE)
            {
                _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_WIFI, params->now_us, output);
                return 0;
            }

            // 双链路均明确不可用，当前没有安全的单发目标。
            if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE &&
                observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
            {
                _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE, params->now_us, output);
                return 0;
            }
        }

        /****************************** 采样条件 ******************************/

        // 仅在两条链路均可用、实例未变化且没有历史中断时允许采样。
        paths_usable = !runtime->path_interrupted &&
                    observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
                    observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE &&
                    observation->wifi.link_id == runtime->wifi_link_id &&
                    observation->cellular.link_id == runtime->cellular_link_id;
    }


    /****************************** 三、验证数据采集 ******************************/

    if (paths_usable)
    {
        // 三秒内真实USER_DATA曾活跃的业务需参与质量比较。
        if (params->activity_valid)
        {
            runtime->class_activity_valid = true;
            runtime->active_class_mask   |= params->active_class_mask & runtime->class_mask;
        }

        if (runtime->reason == LINKG_SWITCH_POLICY_REASON_LOSS)
        {
            // 同一轮记录双链路三业务Wire/Load、Wi-Fi HCC及Cellular Probe。
            // 队列溢出仅锁存LOAD压力，不在Verify内创建或抢占Operation。
            _linkg_switch_verify_collect_loss(runtime, observation, params->now_us);
        }
        else if (runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS)
        {
            // 复用LOSS三秒采样：恢复阶段HCC流控和OVF加分，Wire BAD加分、GOOD减分。
            // 各业务Wire/Load、双链路Probe保持独立累计；不在此处执行回切判决。
            _linkg_switch_verify_collect_recovery_loss(runtime, observation, params->now_us);
        }
        else if (runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
        {
            // 仅检查新增FLOWCTRL_OFF、实际队列OVF及有效Wi-Fi业务负载。
            // 不使用HCC队列水位，不进行双链路Wire质量比较。
            _linkg_switch_verify_collect_recovery_load(runtime, observation, params->now_us);
        }
    }

    /****************************** 四、截止时间与质量判决 ******************************/

    if (params->now_us < runtime->deadline_us)
    {
        return 0;
    }

    // 三秒到期，基于已经累计的有效样本执行一次判决。
    if (runtime->reason == LINKG_SWITCH_POLICY_REASON_LOSS)
    {
        // Wi-Fi积分恢复后仍需满足活跃业务Wi-Fi Probe优于Cellular的约束。
        _linkg_switch_verify_finish(runtime, _linkg_switch_verify_decide_loss_classes(runtime, params->now_us), params->now_us, output);
        return 0;
    }
    else if (runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS)
    {
        // 与LOSS使用同一分类比较，仅保守回退方向不同。
        _linkg_switch_verify_finish(runtime, _linkg_switch_verify_decide_loss_classes(runtime, params->now_us), params->now_us, output);
        return 0;
    }
    else if (runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
    {
        // RECOVERY + LOAD只输出整组WIFI或CELLULAR结果。
        _linkg_switch_verify_finish(runtime, _linkg_switch_verify_decide_recovery_load(runtime, params->now_us), params->now_us, output);
        return 0;
    }

    /****************************** 五、固定结果与事件交付 ******************************/

    // 未识别验证原因不能静默留在冗余状态。
    _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE, params->now_us, output);
    return 0;
}

