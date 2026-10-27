/**
 * @file switch_policy_verify.c
 * @brief LinkG冗余阶段LOSS无线质量与LOAD队列承载验证
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-26
 *
 * 流程：ACK启动 → 三秒采集 → LOSS按无线积分、LOAD按队列溢出判决
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

/****************************** 一、Wi-Fi单方向丢包判据 ******************************/

/**
 * @brief 满足BAD：至少20包、至少3丢，满足动态丢包门槛。
 */
static bool _linkg_switch_verify_loss_bad(uint64_t samples, uint64_t lost)
{
    if (samples < LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES ||
        lost > samples || lost < 3U || 200ULL * lost < samples)
    {
        return false;
    }

    return lost >= 32768ULL || 4ULL * lost * lost >= samples;
}

/**
 * @brief 满足GOOD：至少20包且丢包率低于动态GOOD门槛。
 */
static bool _linkg_switch_verify_loss_good(uint64_t samples, uint64_t lost)
{
    if (samples < LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES || lost > samples)
    {
        return false;
    }

    if (samples >= 10000ULL)
    {
        return 400ULL * lost <= samples;
    }

    return 100ULL * lost <= samples && 16ULL * lost * lost <= samples;
}

/****************************** 二、Wire及队列样本采集 ******************************/

/**
 * @brief 64位饱和累加；统计始终不允许无符号回绕。
 */
static void _linkg_switch_verify_accumulate(uint64_t *total, uint64_t amount)
{
    if (*total > UINT64_MAX - amount)
    {
        *total = UINT64_MAX;
        return;
    }

    *total += amount;
}

/**
 * @brief 检查Wire或Load窗口是否属于ACK后的未消费合法样本。
 *
 * 只接受(started_us, min(deadline_us, now_us)]内的新窗口。
 * 无效、零包、丢包数异常及重复窗口全部忽略。
 */
static bool _linkg_switch_verify_window_valid(const linkg_switch_loss_observation_t *sample,
                                              uint64_t started_us,
                                              uint64_t deadline_us,
                                              uint64_t now_us,
                                              uint64_t last_updated_us)
{
    return sample->valid &&
           sample->sample_packets != 0U &&
           sample->lost_packets <= sample->sample_packets &&
           sample->updated_us > started_us &&
           sample->updated_us <= deadline_us &&
           sample->updated_us <= now_us &&
           sample->updated_us > last_updated_us;
}

/**
 * @brief 消费指定链路的三业务双方向Wire丢包观测。
 *
 * 仅处理class_mask包含的业务，按UL/DL分别累计有效样本。
 * 每个业务方向独立建立采样边界，首个有效窗口只用于初始化，
 * 后续有效窗口累计到Path运行状态。
 *
 * fresh_lost和fresh_samples同时有效时，额外输出本轮新样本，
 * 供Wi-Fi质量恢复积分使用。
 */
static void _linkg_switch_verify_consume_wire(linkg_switch_policy_verify_path_t *path,
                                              const linkg_switch_path_loss_observation_t *loss,
                                              uint32_t class_mask,
                                              uint64_t started_us,
                                              uint64_t deadline_us,
                                              uint64_t now_us,
                                              uint64_t *fresh_lost,
                                              uint64_t *fresh_samples)
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               class_index;
    uint32_t                               direction;

    /****************************** 一、遍历参与业务 ******************************/

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        // 只消费本次Operation参与的业务。
        if ((class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        /****************************** 二、遍历双向Wire窗口 ******************************/

        for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
        {
            // 根据方向选择对应业务的Wire丢包观测。
            sample = direction == LINKG_SWITCH_POLICY_VERIFY_UPLINK ?
                         &loss->uplink_classes[class_index] :
                         &loss->downlink_classes[class_index];

            /****************************** 三、窗口有效性检查 ******************************/

            // 跳过无效、过期、重复或不属于本次验证时间范围的窗口。
            if (!_linkg_switch_verify_window_valid(sample,
                                                    started_us,
                                                    deadline_us,
                                                    now_us,
                                                    path->last_loss_us[class_index][direction]))
            {
                continue;
            }

            // 记录已消费窗口时间，避免下一轮重复处理。
            path->last_loss_us[class_index][direction] = sample->updated_us;

            /****************************** 四、建立采样边界 ******************************/

            // 每个业务方向首次有效窗口只用于建立边界，不参与质量统计。
            if (!path->loss_primed[class_index][direction])
            {
                path->loss_primed[class_index][direction] = true;
                continue;
            }

            /****************************** 五、累计有效Wire样本 ******************************/

            // 累计当前链路在本次验证期间的丢包数和总样本数。
            _linkg_switch_verify_accumulate(&path->lost_packets[class_index][direction],
                                            sample->lost_packets);

            _linkg_switch_verify_accumulate(&path->sample_packets[class_index][direction],
                                            sample->sample_packets);

            /****************************** 六、输出本轮新样本 ******************************/

            // Wi-Fi需要额外输出本轮UL/DL样本，供恢复积分计算。
            if (fresh_lost != NULL && fresh_samples != NULL)
            {
                _linkg_switch_verify_accumulate(&fresh_lost[direction],
                                                sample->lost_packets);

                _linkg_switch_verify_accumulate(&fresh_samples[direction],
                                                sample->sample_packets);
            }
        }
    }
}

/**
 * @brief 按方向消费Load窗口；LOAD恢复连首个新窗口也参与判决。
 */
static void _linkg_switch_verify_consume_load(linkg_switch_policy_verify_path_t *path,
                                              const linkg_switch_path_loss_observation_t *loss,
                                              uint64_t started_us,
                                              uint64_t deadline_us,
                                              uint64_t now_us,
                                              bool load_recovery)
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               direction;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        sample = direction == LINKG_SWITCH_POLICY_VERIFY_UPLINK ? &loss->uplink_load : &loss->downlink_load;

        if (!_linkg_switch_verify_window_valid(sample, started_us, deadline_us, now_us,
                                                path->last_load_us[direction]))
        {
            continue;
        }

        path->last_load_us[direction] = sample->updated_us;

        if (!path->load_primed[direction])
        {
            path->load_primed[direction] = true;

            // LOAD恢复不能漏掉冗余启动后第一个有溢出的窗口。
            if (!load_recovery)
            {
                continue;
            }
        }

        path->load_observed[direction] = true;

        if (sample->lost_packets != 0U)
        {
            path->queue_overflow = true;
        }
    }
}

/****************************** 三、Wi-Fi质量恢复积分 ******************************/

/**
 * @brief 沿用共享异常积分，起始5分；新窗口BAD+1、GOOD-1。
 */
static void _linkg_switch_verify_update_wifi_score(linkg_switch_policy_verify_runtime_t *runtime,
                                                    const uint64_t fresh_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT],
                                                    const uint64_t fresh_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT])
{
    uint32_t direction;
    int32_t  delta;

    // 上下行仍采用共享积分：BAD +1、GOOD -1、单轮总减分上限为1。
    delta = 0;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        if (_linkg_switch_verify_loss_bad(fresh_samples[direction], fresh_lost[direction]))
        {
            delta++;
        }
        else if (_linkg_switch_verify_loss_good(fresh_samples[direction], fresh_lost[direction]))
        {
            delta--;
        }
    }

    if (delta > 0)
    {
        uint32_t next;

        next = (uint32_t)runtime->wifi_loss_score + (uint32_t)delta;
        runtime->wifi_loss_score = next >= LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE ?
                                      LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE : (uint8_t)next;
    }
    else if (delta < 0 && runtime->wifi_loss_score > 0U)
    {
        runtime->wifi_loss_score--;
    }
}

/****************************** 四、Cellular主动可达检测 ******************************/

/**
 * @brief 读取本次3秒内新Cellular Probe；失败或RTT>=250ms即锁定。
 */
static void _linkg_switch_verify_consume_probe(linkg_switch_policy_verify_runtime_t *runtime,
                                               const linkg_switch_probe_observation_t probe[LINKG_TRANSPORT_CLASS_COUNT],
                                               uint64_t now_us)
{
    const linkg_switch_probe_observation_t *sample;
    uint32_t                                class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        sample = &probe[class_index];

        if (!sample->valid || sample->updated_us <= runtime->started_us ||
            sample->updated_us > runtime->deadline_us || sample->updated_us > now_us ||
            sample->updated_us <= runtime->cellular_last_probe_us[class_index])
        {
            continue;
        }

        runtime->cellular_last_probe_us[class_index] = sample->updated_us;

        if (!sample->reachable || sample->rtt_us == 0U ||
            sample->rtt_us >= LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US)
        {
            // 本次会话内任一有效Probe失败即锁定候选失败，后续成功不撤销。
            runtime->cellular_probe_failed = true;
            continue;
        }

        runtime->cellular_probe_seen_mask |= 1U << class_index;
        runtime->cellular_last_good_probe_us[class_index] = sample->updated_us;

        if (sample->rtt_us > runtime->cellular_max_rtt_us)
        {
            runtime->cellular_max_rtt_us = sample->rtt_us;
        }
    }
}

/**
 * @brief 参与业务均需新鲜成功Probe且全程没有有效失败。
 */
static bool _linkg_switch_verify_cellular_probe_ready(const linkg_switch_policy_verify_runtime_t *runtime)
{
    uint64_t last_good_us;
    uint32_t class_index;

    if (runtime->cellular_probe_failed ||
        (runtime->cellular_probe_seen_mask & runtime->class_mask) != runtime->class_mask)
    {
        return false;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        last_good_us = runtime->cellular_last_good_probe_us[class_index];

        if (last_good_us <= runtime->started_us ||
            last_good_us > runtime->deadline_us ||
            runtime->deadline_us - last_good_us > LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US)
        {
            return false;
        }
    }

    return true;
}

/****************************** 五、相同业务和方向的Wire质量比较 ******************************/

/**
 * @brief 对相同业务/方向用min(Nwifi,Ncell)加权；需至少100配对样本。
 */
static bool _linkg_switch_verify_cellular_wire_better(const linkg_switch_policy_verify_runtime_t *runtime)
{
    uint64_t wifi_samples;
    uint64_t cellular_samples;
    uint64_t paired_samples;
    uint64_t wifi_n;
    uint64_t cellular_n;
    uint64_t paired_n;
    uint32_t class_index;
    uint32_t direction;
    double   wifi_lost_equivalent;
    double   cellular_lost_equivalent;

    wifi_samples            = 0U;
    cellular_samples        = 0U;
    paired_samples          = 0U;
    wifi_lost_equivalent    = 0.0;
    cellular_lost_equivalent = 0.0;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
        {
            wifi_n     = runtime->wifi.sample_packets[class_index][direction];
            cellular_n = runtime->cellular.sample_packets[class_index][direction];

            if (wifi_n == 0U && cellular_n == 0U)
            {
                continue;
            }

            if (wifi_n == 0U || cellular_n == 0U ||
                runtime->wifi.lost_packets[class_index][direction] > wifi_n ||
                runtime->cellular.lost_packets[class_index][direction] > cellular_n)
            {
                return false;
            }

            // 每个(业务,方向)取双链路较小样本数作为比较权重；
            // 这不是包级序号一一配对，而是窗口统计的归一化比较。
            paired_n = wifi_n < cellular_n ? wifi_n : cellular_n;

            _linkg_switch_verify_accumulate(&wifi_samples, wifi_n);
            _linkg_switch_verify_accumulate(&cellular_samples, cellular_n);
            _linkg_switch_verify_accumulate(&paired_samples, paired_n);

            wifi_lost_equivalent += (double)runtime->wifi.lost_packets[class_index][direction] *
                                    (double)paired_n / (double)wifi_n;
            cellular_lost_equivalent += (double)runtime->cellular.lost_packets[class_index][direction] *
                                        (double)paired_n / (double)cellular_n;
        }
    }

    if (wifi_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES ||
        cellular_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES ||
        paired_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES)
    {
        return false;
    }

    // 严格更低才确认Cellular更优；相等或证据不足均不触发质量迁移。
    return cellular_lost_equivalent < wifi_lost_equivalent;
}

/****************************** 六、验证终态与事件交付 ******************************/

/**
 * @brief 按Path、Wi-Fi积分、原因和Cellular质量确认结果。
 */
static linkg_switch_policy_verify_result_t _linkg_switch_verify_result(const linkg_switch_policy_verify_runtime_t *runtime,
                                                                       const linkg_switch_observation_t *observation,
                                                                       uint64_t now_us)
{
    if (runtime->path_interrupted || !observation->valid ||
        observation->collected_us == 0U || observation->collected_us > now_us ||
        now_us - observation->collected_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id != runtime->wifi_link_id ||
        observation->cellular.link_id != runtime->cellular_link_id)
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    /****************************** LOAD队列恢复验证 ******************************/

    if (runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY &&
        runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
    {
        // 冗余期间发现任何一个方向的Wi-Fi队列溢出，继续使用Cellular。
        if (runtime->wifi.queue_overflow)
        {
            return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
        }

        // 至少持续采集到ACK后的有效Wi-Fi Path流量窗口；空闲的零速率窗口同样有效。
        if (!runtime->wifi_traffic_seen ||
            !observation->wifi.traffic.valid ||
            observation->wifi.traffic.updated_us <= runtime->started_us ||
            observation->wifi.traffic.updated_us > now_us ||
            now_us - observation->wifi.traffic.updated_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
        {
            return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
        }

        // 实际有发送或接收时，需要对应方向的新队列样本，不能把缺失统计视为零溢出。
        if ((runtime->wifi_tx_active && !runtime->wifi.load_observed[LINKG_SWITCH_POLICY_VERIFY_UPLINK]) ||
            (runtime->wifi_rx_active && !runtime->wifi.load_observed[LINKG_SWITCH_POLICY_VERIFY_DOWNLINK]))
        {
            return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
        }

        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    /****************************** LOSS无线质量恢复 ******************************/

    // 必须严格低于恢复门槛；积分等于3时不视为已恢复。
    if (runtime->wifi_loss_score < LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE)
    {
        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    /****************************** 恢复事务保持Cellular ******************************/

    // 恢复尝试期间Wi-Fi没有达到恢复门槛，继续使用原Cellular主链路。
    if (runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
    }

    /****************************** LOSS迁移确认 ******************************/

    // Wi-Fi异常积分必须大于3，且Cellular具备Probe、Wire证据并无队列溢出。
    // 任何条件不足均返回INCONCLUSIVE，交给Operation执行保守回退。
    if (runtime->wifi_loss_score <= LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE ||
        runtime->cellular.queue_overflow ||
        !_linkg_switch_verify_cellular_probe_ready(runtime) ||
        !_linkg_switch_verify_cellular_wire_better(runtime))
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
}

/**
 * @brief 将已固定结果封装为VERIFY_DONE；允许重复交付。
 */
static void _linkg_switch_verify_fill_event(const linkg_switch_policy_verify_runtime_t *runtime,
                                             linkg_switch_policy_event_t *event)
{
    memset(event, 0, sizeof(*event));

    event->type                             = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
    event->peer_node_id                     = runtime->peer_node_id;
    event->peer_generation                  = runtime->peer_generation;
    event->occurred_us                      = runtime->finished_us;
    event->payload.verify_done.operation_id = runtime->operation_id;
    event->payload.verify_done.result       = runtime->result;
}

/****************************** 七、验证会话生命周期 ******************************/

/**
 * @brief 由Operation取消/消费结束后释放当前验证会话。
 */
void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime)
{
    if (runtime != NULL)
    {
        memset(runtime, 0, sizeof(*runtime));
    }
}

/**
 * @brief 冗余ACK确认后启动质量验证会话。
 *
 * 固定本次Operation身份、参与业务、链路实例及验证截止时间。
 * 本函数不判断业务活跃性，也不执行Wire质量比较。
 */
int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime,
                                     uint8_t peer_node_id,
                                     uint32_t peer_generation,
                                     const uint32_t operation_ids[LINKG_TRANSPORT_CLASS_COUNT],
                                     uint32_t class_mask,
                                     linkg_switch_policy_reason_t reason,
                                     linkg_switch_policy_migration_reason_t recovery_reason,
                                     const linkg_switch_observation_t *observation,
                                     uint64_t now_us)
{
    uint32_t operation_id;
    uint32_t class_index;
    uint32_t other_index;

    /****************************** 一、基础参数检查 ******************************/

    if (runtime == NULL || observation == NULL || operation_ids == NULL ||
        peer_node_id == 0U || peer_node_id == UINT8_MAX ||
        peer_generation == 0U ||
        class_mask == 0U ||
        (class_mask & ~LINKG_SWITCH_POLICY_CLASS_MASK_ALL) != 0U ||
        now_us == 0U ||
        now_us > UINT64_MAX - LINKG_SWITCH_POLICY_VERIFY_DURATION_US)
    {
        return -EINVAL;
    }

    /****************************** 二、事务类型检查 ******************************/

    // 仅支持LOSS验证及RECOVERY恢复验证。
    if (reason != LINKG_SWITCH_POLICY_REASON_LOSS &&
        reason != LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        return -EINVAL;
    }

    // LOSS不是恢复事务，不允许携带历史迁移原因。
    if (reason == LINKG_SWITCH_POLICY_REASON_LOSS &&
        recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_NONE)
    {
        return -EINVAL;
    }

    // RECOVERY必须明确原始迁移原因。
    if (reason == LINKG_SWITCH_POLICY_REASON_RECOVERY &&
        recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_LOSS &&
        recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_LOAD)
    {
        return -EINVAL;
    }

    /****************************** 三、运行状态检查 ******************************/

    // 单个Peer同时只允许存在一个活动验证会话。
    if (runtime->active)
    {
        return -EBUSY;
    }

    /****************************** 四、链路观测检查 ******************************/

    // 必须使用当前Peer的新鲜有效观测。
    if (!observation->valid ||
        observation->peer_node_id != peer_node_id ||
        observation->collected_us == 0U ||
        observation->collected_us > now_us ||
        now_us - observation->collected_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
    {
        return -EINVAL;
    }

    // 启动验证前必须确认两条链路均可用。
    if (observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id == LINKG_LINK_ID_INVALID ||
        observation->cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    /****************************** 五、Operation身份检查 ******************************/

    operation_id = 0U;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        // 未参与本次事务的业务不得携带Operation ID。
        if ((class_mask & (1U << class_index)) == 0U)
        {
            if (operation_ids[class_index] != 0U)
            {
                return -EINVAL;
            }

            continue;
        }

        // 参与本次事务的业务必须具有有效Operation ID。
        if (operation_ids[class_index] == 0U)
        {
            return -EINVAL;
        }

        // 不同业务的Operation ID必须互不重复。
        for (other_index = 0U; other_index < class_index; other_index++)
        {
            if (operation_ids[class_index] == operation_ids[other_index])
            {
                return -EINVAL;
            }
        }

        // 使用首个参与业务的Operation ID作为组级事件身份。
        if (operation_id == 0U)
        {
            operation_id = operation_ids[class_index];
        }
    }

    /****************************** 六、验证会话初始化 ******************************/

    // 全部检查通过后重置运行状态，避免继承历史采样。
    memset(runtime, 0, sizeof(*runtime));

    // 事务身份。
    runtime->active          = true;
    runtime->peer_node_id    = peer_node_id;
    runtime->peer_generation = peer_generation;
    runtime->operation_id    = operation_id;

    memcpy(runtime->operation_ids, operation_ids, sizeof(runtime->operation_ids));

    // 验证范围与原始原因。
    runtime->class_mask      = class_mask;
    runtime->reason          = reason;
    runtime->recovery_reason = recovery_reason;

    // 固定本次验证使用的链路实例。
    runtime->wifi_link_id     = observation->wifi.link_id;
    runtime->cellular_link_id = observation->cellular.link_id;

    // 冗余ACK确认后开始固定三秒验证。
    runtime->started_us  = now_us;
    runtime->deadline_us = now_us + LINKG_SWITCH_POLICY_VERIFY_DURATION_US;

    // LOSS及LOSS恢复验证均从异常积分5开始。
    runtime->wifi_loss_score = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;

    return 0;
}

/**
 * @brief 消费双链路质量观测，并在验证截止后形成固定结果。
 *
 * 验证期间持续采集Wi-Fi和Cellular质量样本。
 * 到达三秒截止时间后只结算一次，结果由Operation消费。
 * 结果尚未消费时允许重复交付同一个VERIFY_DONE事件。
 */
int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime,
                                       const linkg_switch_observation_t *observation,
                                       uint64_t now_us,
                                       linkg_switch_policy_event_t *event,
                                       bool *event_ready)
{
    uint64_t fresh_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    uint64_t fresh_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    bool     observation_fresh;
    bool     paths_usable;
    bool     load_recovery;

    /****************************** 一、基础参数检查 ******************************/

    if (runtime == NULL || observation == NULL ||
        event == NULL || event_ready == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *event_ready = false;

    /****************************** 二、验证会话及事件状态 ******************************/

    // 当前没有活动验证会话，无需处理。
    if (!runtime->active)
    {
        return 0;
    }

    // 拒绝其他Peer的观测及早于验证启动时间的调用。
    if (observation->peer_node_id != runtime->peer_node_id ||
        now_us < runtime->started_us)
    {
        return -ESTALE;
    }

    // 已形成固定结果时直接重复交付，不重新采样或结算。
    if (runtime->result_pending)
    {
        _linkg_switch_verify_fill_event(runtime, event);
        *event_ready = true;
        return 0;
    }

    /****************************** 三、观测有效性及链路连续性 ******************************/

    // 仅接受验证启动后生成、且未超过有效期的观测。
    observation_fresh = observation->valid &&
                        observation->collected_us > runtime->started_us &&
                        observation->collected_us <= now_us &&
                        now_us - observation->collected_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US;

    // 有效观测确认链路不可用或Link实例变化时，永久标记本次验证中断。
    if (observation_fresh &&
        (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
         observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
         observation->wifi.link_id != runtime->wifi_link_id ||
         observation->cellular.link_id != runtime->cellular_link_id))
    {
        runtime->path_interrupted = true;
    }

    // 只有观测新鲜、链路连续且双链路可用时，才允许采集质量样本。
    paths_usable = observation_fresh &&
                   !runtime->path_interrupted &&
                   observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
                   observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE;

    /****************************** 四、双链路质量样本采集 ******************************/

    // LOAD原因的恢复验证使用独立的队列承载判据。
    load_recovery = runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY &&
                    runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD;

    if (paths_usable)
    {
        /****************************** 4.1 Wire丢包样本 ******************************/

        memset(fresh_lost, 0, sizeof(fresh_lost));
        memset(fresh_samples, 0, sizeof(fresh_samples));

        // Wi-Fi：累计参与业务的上下行Wire样本，并输出本轮积分输入。
        _linkg_switch_verify_consume_wire(&runtime->wifi,
                                          &observation->wifi.loss,
                                          runtime->class_mask,
                                          runtime->started_us,
                                          runtime->deadline_us,
                                          now_us,
                                          fresh_lost,
                                          fresh_samples);

        // Cellular：累计参与业务的上下行Wire样本，用于双链路质量比较。
        _linkg_switch_verify_consume_wire(&runtime->cellular,
                                          &observation->cellular.loss,
                                          runtime->class_mask,
                                          runtime->started_us,
                                          runtime->deadline_us,
                                          now_us,
                                          NULL,
                                          NULL);

        /****************************** 4.2 Wi-Fi质量恢复积分 ******************************/

        // LOSS验证及LOSS恢复验证持续更新Wi-Fi积分。
        if (!load_recovery)
        {
            _linkg_switch_verify_update_wifi_score(runtime, fresh_lost, fresh_samples);
        }

        /****************************** 4.3 双链路队列溢出样本 ******************************/

        // Wi-Fi：LOAD恢复验证允许首个有效Load窗口参与判决。
        _linkg_switch_verify_consume_load(&runtime->wifi,
                                          &observation->wifi.loss,
                                          runtime->started_us,
                                          runtime->deadline_us,
                                          now_us,
                                          load_recovery);

        // Cellular：沿用常规Load窗口采集规则。
        _linkg_switch_verify_consume_load(&runtime->cellular,
                                          &observation->cellular.loss,
                                          runtime->started_us,
                                          runtime->deadline_us,
                                          now_us,
                                          false);

        /****************************** 4.4 LOAD恢复业务活动 ******************************/

        // LOAD恢复期间记录Wi-Fi真实收发方向，供截止时检查队列样本。
        if (load_recovery &&
            observation->wifi.traffic.valid &&
            observation->wifi.traffic.updated_us > runtime->started_us &&
            observation->wifi.traffic.updated_us <= runtime->deadline_us &&
            observation->wifi.traffic.updated_us <= now_us)
        {
            runtime->wifi_traffic_seen = true;
            runtime->wifi_tx_active |= observation->wifi.traffic.tx_pps != 0U;
            runtime->wifi_rx_active |= observation->wifi.traffic.rx_pps != 0U;
        }

        /****************************** 4.5 Cellular主动质量探测 ******************************/

        // LOSS相关验证采集Cellular Probe，LOAD恢复不使用该判据。
        if (!load_recovery)
        {
            _linkg_switch_verify_consume_probe(runtime, observation->cellular.probe, now_us);
        }
    }

    /****************************** 五、验证截止及事件交付 ******************************/

    // 三秒验证尚未到期，继续等待后续观测。
    if (now_us < runtime->deadline_us)
    {
        return 0;
    }

    /****************************** 5.1 固化验证结果 ******************************/

    // 截止时仅计算一次，后续调用不再改变质量判决。
    runtime->result         = _linkg_switch_verify_result(runtime, observation, now_us);
    runtime->finished_us    = now_us;
    runtime->result_pending = true;

    /****************************** 5.2 交付VERIFY_DONE事件 ******************************/

    _linkg_switch_verify_fill_event(runtime, event);
    *event_ready = true;

    return 0;
}
