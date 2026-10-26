/**
 * @file switch_policy_verify.c
 * @brief LinkG冗余阶段Wi-Fi积分恢复及Cellular质量确认
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-26
 *
 * 流程：ACK启动 → Wire/Load/Probe采集 → Wi-Fi共享积分 → 双链路比较
 *       → 固化VERIFY_DONE → Operation决定最终单发Plan。
 *
 * 本模块只评估质量，不修改发送计划，也不执行PlanSync。
 */

#include "switch_policy_verify.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

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
 * @brief 按业务/方向消费Wire窗口；首个ACK后窗口只建立边界。
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

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
        {
            sample = direction == LINKG_SWITCH_POLICY_VERIFY_UPLINK ?
                         &loss->uplink_classes[class_index] :
                         &loss->downlink_classes[class_index];

            if (!_linkg_switch_verify_window_valid(sample, started_us, deadline_us, now_us,
                                                    path->last_loss_us[class_index][direction]))
            {
                continue;
            }

            path->last_loss_us[class_index][direction] = sample->updated_us;

            if (!path->loss_primed[class_index][direction])
            {
                path->loss_primed[class_index][direction] = true;
                continue;
            }

            _linkg_switch_verify_accumulate(&path->lost_packets[class_index][direction], sample->lost_packets);
            _linkg_switch_verify_accumulate(&path->sample_packets[class_index][direction], sample->sample_packets);

            if (fresh_lost != NULL && fresh_samples != NULL)
            {
                _linkg_switch_verify_accumulate(&fresh_lost[direction], sample->lost_packets);
                _linkg_switch_verify_accumulate(&fresh_samples[direction], sample->sample_packets);
            }
        }
    }
}

/**
 * @brief 按方向消费Load窗口；发现过一次队列溢出即锁定。
 */
static void _linkg_switch_verify_consume_load(linkg_switch_policy_verify_path_t *path,
                                              const linkg_switch_path_loss_observation_t *loss,
                                              uint64_t started_us,
                                              uint64_t deadline_us,
                                              uint64_t now_us)
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
            continue;
        }

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

    /****************************** Wi-Fi质量已恢复 ******************************/

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
 * @brief 冗余ACK后固定事务身份和三秒截止时刻。
 */
int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime,
                                     uint8_t peer_node_id,
                                     uint32_t peer_generation,
                                     const uint32_t operation_ids[LINKG_TRANSPORT_CLASS_COUNT],
                                     uint32_t class_mask,
                                     linkg_switch_policy_reason_t reason,
                                     const linkg_switch_observation_t *observation,
                                     uint64_t now_us)
{
    uint32_t operation_id;
    uint32_t class_index;
    uint32_t other_index;

    if (runtime == NULL || observation == NULL || operation_ids == NULL ||
        peer_node_id == 0U || peer_node_id == UINT8_MAX ||
        peer_generation == 0U || class_mask == 0U ||
        (class_mask & ~LINKG_SWITCH_POLICY_CLASS_MASK_ALL) != 0U ||
        (reason != LINKG_SWITCH_POLICY_REASON_LOSS && reason != LINKG_SWITCH_POLICY_REASON_RECOVERY) ||
        now_us == 0U || now_us > UINT64_MAX - LINKG_SWITCH_POLICY_VERIFY_DURATION_US)
    {
        return -EINVAL;
    }

    if (runtime->active)
    {
        return -EBUSY;
    }

    if (!observation->valid || observation->peer_node_id != peer_node_id ||
        observation->collected_us == 0U || observation->collected_us > now_us ||
        now_us - observation->collected_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id == 0U || observation->cellular.link_id == 0U)
    {
        return -EINVAL;
    }

    operation_id = 0U;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((class_mask & (1U << class_index)) == 0U)
        {
            if (operation_ids[class_index] != 0U)
            {
                return -EINVAL;
            }

            continue;
        }

        if (operation_ids[class_index] == 0U)
        {
            return -EINVAL;
        }

        for (other_index = 0U; other_index < class_index; other_index++)
        {
            if (operation_ids[class_index] == operation_ids[other_index])
            {
                return -EINVAL;
            }
        }

        if (operation_id == 0U)
        {
            operation_id = operation_ids[class_index];
        }
    }

    memset(runtime, 0, sizeof(*runtime));

    runtime->active           = true;
    runtime->peer_node_id     = peer_node_id;
    runtime->peer_generation  = peer_generation;
    runtime->operation_id     = operation_id;
    memcpy(runtime->operation_ids, operation_ids, sizeof(runtime->operation_ids));
    runtime->class_mask       = class_mask;
    runtime->reason           = reason;
    runtime->wifi_link_id     = observation->wifi.link_id;
    runtime->cellular_link_id = observation->cellular.link_id;
    runtime->started_us       = now_us;
    runtime->deadline_us      = now_us + LINKG_SWITCH_POLICY_VERIFY_DURATION_US;
    runtime->wifi_loss_score  = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;

    return 0;
}

/**
 * @brief 消费观测并在3秒后一次性结算，失败投递可重交付。
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

    if (runtime == NULL || observation == NULL || event == NULL || event_ready == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *event_ready = false;

    if (!runtime->active)
    {
        return 0;
    }

    if (observation->peer_node_id != runtime->peer_node_id || now_us < runtime->started_us)
    {
        return -ESTALE;
    }

    // 已形成结果时重复投递同一事件，不重新采样、比较或改变最终结果。
    if (runtime->result_pending)
    {
        _linkg_switch_verify_fill_event(runtime, event);
        *event_ready = true;
        return 0;
    }

    /****************************** 观测有效性及链路连续性 ******************************/

    observation_fresh = observation->valid &&
                        observation->collected_us > runtime->started_us &&
                        observation->collected_us <= now_us &&
                        now_us - observation->collected_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US;

    if (observation_fresh &&
        (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
         observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
         observation->wifi.link_id != runtime->wifi_link_id ||
         observation->cellular.link_id != runtime->cellular_link_id))
    {
        runtime->path_interrupted = true;
    }

    paths_usable = observation_fresh && !runtime->path_interrupted &&
                   observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
                   observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE;

    /****************************** 双链路新窗口采集 ******************************/

    if (paths_usable)
    {
        memset(fresh_lost, 0, sizeof(fresh_lost));
        memset(fresh_samples, 0, sizeof(fresh_samples));

        _linkg_switch_verify_consume_wire(&runtime->wifi, &observation->wifi.loss,
                                          runtime->class_mask, runtime->started_us, runtime->deadline_us, now_us,
                                          fresh_lost, fresh_samples);

        _linkg_switch_verify_consume_wire(&runtime->cellular, &observation->cellular.loss,
                                          runtime->class_mask, runtime->started_us, runtime->deadline_us, now_us,
                                          NULL, NULL);

        _linkg_switch_verify_update_wifi_score(runtime, fresh_lost, fresh_samples);

        _linkg_switch_verify_consume_load(&runtime->wifi, &observation->wifi.loss,
                                          runtime->started_us, runtime->deadline_us, now_us);
        _linkg_switch_verify_consume_load(&runtime->cellular, &observation->cellular.loss,
                                          runtime->started_us, runtime->deadline_us, now_us);

        _linkg_switch_verify_consume_probe(runtime, observation->cellular.probe, now_us);
    }

    /****************************** 三秒截止形成固定结果 ******************************/

    if (now_us < runtime->deadline_us)
    {
        return 0;
    }

    runtime->result         = _linkg_switch_verify_result(runtime, observation, now_us);
    runtime->finished_us    = now_us;
    runtime->result_pending = true;

    _linkg_switch_verify_fill_event(runtime, event);
    *event_ready = true;

    return 0;
}
