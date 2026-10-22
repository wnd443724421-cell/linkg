/**
 * @file switch_policy_verify.c
 * @brief LinkG冗余阶段Wi-Fi恢复积分及双链路质量验证实现
 */

#include "switch_policy_verify.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

/****************************** 基础辅助 ******************************/

static void _linkg_switch_verify_accumulate(uint64_t *total, uint64_t increment)
{
    if (*total > UINT64_MAX - increment)
    {
        *total = UINT64_MAX;
        return;
    }

    *total += increment;
}

static bool _linkg_switch_verify_fresh(uint64_t updated_us, uint64_t now_us, uint64_t max_age_us)
{
    return updated_us != 0U && updated_us <= now_us && now_us - updated_us <= max_age_us;
}

/**
 * @brief 与Loss检测器保持一致的BAD判据。
 *
 * N>=20、L>=3、loss>=max(0.5%,50%/sqrt(N))。
 * 使用整数运算，较大的L先截断平方判断，避免乘法溢出。
 */
static bool _linkg_switch_verify_bad(uint64_t samples, uint64_t lost)
{
    if (samples < LINKG_SWITCH_POLICY_VERIFY_LOSS_MIN_SAMPLES ||
        lost < 3U || lost > samples || 200ULL * lost < samples)
    {
        return false;
    }

    return lost >= 32768ULL || 4ULL * lost * lost >= samples;
}

/**
 * @brief 与Loss检测器保持一致的GOOD判据。
 *
 * N<10000要求loss<=1%且16*L*L<=N；N>=10000要求loss<=0.25%。
 */
static bool _linkg_switch_verify_good(uint64_t samples, uint64_t lost)
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

/****************************** Wire窗口采集 ******************************/

/**
 * @brief 仅累计ACK后新产生的各业务、各方向独立Wire窗口。
 *
 * 丢弃首个新窗口，避免其统计区间横跨冗余ACK时刻。
 * 同一updated_us只消费一次，且不能消费截止时间以后的数据。
 */
static void _linkg_switch_verify_collect_wire(linkg_switch_policy_verify_path_t *path,
                                              const linkg_switch_path_loss_observation_t *source,
                                              uint32_t class_mask,
                                              uint64_t started_us,
                                              uint64_t deadline_us,
                                              uint64_t now_us,
                                              uint64_t new_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT],
                                              uint64_t new_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT])
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               class_index;
    uint32_t                               direction;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            continue;
        }

        for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
        {
            sample = direction == LINKG_SWITCH_POLICY_VERIFY_UPLINK ?
                         &source->uplink_classes[class_index] :
                         &source->downlink_classes[class_index];

            if (!sample->valid || sample->sample_packets == 0U ||
                sample->lost_packets > sample->sample_packets ||
                sample->updated_us <= started_us ||
                sample->updated_us > deadline_us || sample->updated_us > now_us ||
                sample->updated_us <= path->last_loss_us[class_index][direction])
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
            _linkg_switch_verify_accumulate(&new_lost[direction], sample->lost_packets);
            _linkg_switch_verify_accumulate(&new_samples[direction], sample->sample_packets);
        }
    }
}

/**
 * @brief 整链路队列溢出仅作为候选链路保护，不参与业务Wire丢包合计。
 */
static void _linkg_switch_verify_collect_load(linkg_switch_policy_verify_path_t *path,
                                              const linkg_switch_path_loss_observation_t *source,
                                              uint64_t started_us,
                                              uint64_t deadline_us,
                                              uint64_t now_us)
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               direction;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        sample = direction == LINKG_SWITCH_POLICY_VERIFY_UPLINK ?
                     &source->uplink_load : &source->downlink_load;

        if (!sample->valid || sample->sample_packets == 0U ||
            sample->lost_packets > sample->sample_packets ||
            sample->updated_us <= started_us ||
            sample->updated_us > deadline_us || sample->updated_us > now_us ||
            sample->updated_us <= path->last_load_us[direction])
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

/****************************** Cellular Probe采集 ******************************/

/**
 * @brief 只检查Cellular RTT是否满足迁移保护要求，不对Wi-Fi RTT加权。
 *
 * 每业务独立时间戳去重。明确不可达或RTT>=250ms，永久否决本轮候选。
 * 未收到新Probe不等于成功；截止时另行验证最近成功Probe年龄。
 */
static void _linkg_switch_verify_collect_cellular_probe(linkg_switch_policy_verify_runtime_t *runtime,
                                                        const linkg_switch_probe_observation_t probes[LINKG_TRANSPORT_CLASS_COUNT],
                                                        uint64_t now_us)
{
    const linkg_switch_probe_observation_t *sample;
    uint32_t                                class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            continue;
        }

        sample = &probes[class_index];

        if (sample->updated_us <= runtime->started_us ||
            sample->updated_us > runtime->deadline_us ||
            sample->updated_us > now_us ||
            sample->updated_us <= runtime->cellular_last_probe_us[class_index])
        {
            continue;
        }

        // 最近一次已处理时间必须同时覆盖不可达/无效结果，防止重复处理。
        runtime->cellular_last_probe_us[class_index] = sample->updated_us;

        if (!sample->valid)
        {
            continue;
        }

        if (!sample->reachable || sample->rtt_us == 0U ||
            sample->rtt_us >= LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US)
        {
            runtime->cellular_probe_failed = true;
            continue;
        }

        runtime->cellular_probe_seen_mask |= LINKG_SWITCH_POLICY_CLASS_BIT(class_index);
        runtime->cellular_last_good_probe_us[class_index] = sample->updated_us;

        if (sample->rtt_us > runtime->cellular_max_rtt_us)
        {
            runtime->cellular_max_rtt_us = sample->rtt_us;
        }
    }
}

/****************************** Wi-Fi恢复积分 ******************************/

/**
 * @brief 单轮UL/DL分别判决，合并积分；两个GOOD每轮最多减1分。
 */
static void _linkg_switch_verify_update_score(linkg_switch_policy_verify_runtime_t *runtime,
                                             const uint64_t fresh_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT],
                                             const uint64_t fresh_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT])
{
    uint32_t direction;
    int32_t  delta;

    delta = 0;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        if (_linkg_switch_verify_bad(fresh_samples[direction], fresh_lost[direction]))
        {
            delta++;
        }
        else if (_linkg_switch_verify_good(fresh_samples[direction], fresh_lost[direction]))
        {
            delta--;
        }
    }

    if (delta > 0)
    {
        uint32_t next_score = (uint32_t)runtime->wifi_loss_score + (uint32_t)delta;

        runtime->wifi_loss_score = (uint8_t)(next_score > LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE ?
                                                LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE : next_score);
    }
    else if (delta < 0 && runtime->wifi_loss_score > 0U)
    {
        runtime->wifi_loss_score--;
    }
}

/****************************** 丢包率比较 ******************************/

/**
 * @brief 汇总同一业务和方向均有样本的两链路Wire交付率。
 *
 * 双链路同一业务/方向必须都存在样本，或都没有样本；单边缺失不能比较。
 * 不设置业务优先级权重；以参与业务的实际报文数量自然加权。
 */
static bool _linkg_switch_verify_cellular_better(const linkg_switch_policy_verify_runtime_t *runtime)
{
    uint64_t wifi_lost;
    uint64_t wifi_samples;
    uint64_t cellular_lost;
    uint64_t cellular_samples;
    uint64_t wifi_n;
    uint64_t cellular_n;
    uint32_t class_index;
    uint32_t direction;

    wifi_lost        = 0U;
    wifi_samples     = 0U;
    cellular_lost    = 0U;
    cellular_samples = 0U;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
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

            if (wifi_n == 0U || cellular_n == 0U)
            {
                return false;
            }

            _linkg_switch_verify_accumulate(&wifi_samples, wifi_n);
            _linkg_switch_verify_accumulate(&cellular_samples, cellular_n);
            _linkg_switch_verify_accumulate(&wifi_lost, runtime->wifi.lost_packets[class_index][direction]);
            _linkg_switch_verify_accumulate(&cellular_lost, runtime->cellular.lost_packets[class_index][direction]);
        }
    }

    if (wifi_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES ||
        cellular_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES)
    {
        return false;
    }

    // 避免uint64_t交叉相乘溢出；仅进行一次双精度比率比较。
    return (double)cellular_lost / (double)cellular_samples <
           (double)wifi_lost / (double)wifi_samples;
}

/****************************** 最终判决 ******************************/

static linkg_switch_policy_verify_result_t _linkg_switch_verify_decide(const linkg_switch_policy_verify_runtime_t *runtime,
                                                                       const linkg_switch_observation_t *observation,
                                                                       uint64_t now_us)
{
    uint32_t class_index;

    if (runtime->path_interrupted ||
        !observation->valid || observation->peer_node_id != runtime->peer_node_id ||
        !_linkg_switch_verify_fresh(observation->collected_us, now_us,
                                    LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US) ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id != runtime->wifi_link_id ||
        observation->cellular.link_id != runtime->cellular_link_id)
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    if (runtime->wifi_loss_score < LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE)
    {
        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    if (runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
    }

    if (runtime->cellular_probe_failed ||
        runtime->cellular.queue_overflow ||
        runtime->cellular_max_rtt_us == 0U ||
        (runtime->cellular_probe_seen_mask & runtime->class_mask) != runtime->class_mask)
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            continue;
        }

        // 使用固定截止时刻检查Probe年龄，避免Worker迟到导致误拒绝。
        if (!_linkg_switch_verify_fresh(runtime->cellular_last_good_probe_us[class_index],
                                        runtime->deadline_us,
                                        LINKG_SWITCH_POLICY_VERIFY_PROBE_MAX_AGE_US))
        {
            return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
        }
    }

    if (!_linkg_switch_verify_cellular_better(runtime))
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
}

/****************************** 生命周期 ******************************/

void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime)
{
    if (runtime != NULL)
    {
        memset(runtime, 0, sizeof(*runtime));
    }
}

int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime,
                                     uint8_t peer_node_id,
                                     uint32_t peer_generation,
                                     const uint32_t operation_ids[LINKG_TRANSPORT_CLASS_COUNT],
                                     uint32_t class_mask,
                                     linkg_switch_policy_reason_t reason,
                                     const linkg_switch_observation_t *observation,
                                     uint64_t now_us)
{
    uint32_t class_index;
    uint32_t other_index;
    uint32_t representative_id;
    uint32_t bit;

    if (runtime == NULL || operation_ids == NULL || observation == NULL ||
        peer_node_id == 0U || peer_generation == 0U || now_us == 0U ||
        now_us > UINT64_MAX - LINKG_SWITCH_POLICY_VERIFY_DURATION_US ||
        class_mask == 0U || (class_mask & ~LINKG_SWITCH_POLICY_CLASS_MASK_ALL) != 0U ||
        (reason != LINKG_SWITCH_POLICY_REASON_LOSS && reason != LINKG_SWITCH_POLICY_REASON_RECOVERY) ||
        !observation->valid || observation->peer_node_id != peer_node_id ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE)
    {
        return -EINVAL;
    }

    if (runtime->active)
    {
        return -EBUSY;
    }

    representative_id = 0U;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        bit = LINKG_SWITCH_POLICY_CLASS_BIT(class_index);

        if ((class_mask & bit) == 0U)
        {
            if (operation_ids[class_index] != LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
            {
                return -EINVAL;
            }

            continue;
        }

        if (operation_ids[class_index] == LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
        {
            return -EINVAL;
        }

        if (representative_id == 0U)
        {
            representative_id = operation_ids[class_index];
        }

        for (other_index = 0U; other_index < class_index; other_index++)
        {
            if ((class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(other_index)) != 0U &&
                operation_ids[class_index] == operation_ids[other_index])
            {
                return -EINVAL;
            }
        }
    }

    memset(runtime, 0, sizeof(*runtime));

    runtime->peer_node_id     = peer_node_id;
    runtime->peer_generation  = peer_generation;
    runtime->operation_id     = representative_id;
    runtime->class_mask       = class_mask;
    runtime->reason           = reason;
    runtime->wifi_link_id     = observation->wifi.link_id;
    runtime->cellular_link_id = observation->cellular.link_id;
    runtime->started_us       = now_us;
    runtime->deadline_us      = now_us + LINKG_SWITCH_POLICY_VERIFY_DURATION_US;
    runtime->wifi_loss_score  = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;
    runtime->active           = true;

    memcpy(runtime->operation_ids, operation_ids, sizeof(runtime->operation_ids));

    return 0;
}

/**
 * @brief 构造缓存的最终结果。重复调用返回相同的逻辑事件，不再采样。
 */
static void _linkg_switch_verify_make_event(const linkg_switch_policy_verify_runtime_t *runtime,
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

int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime,
                                       const linkg_switch_observation_t *observation,
                                       uint64_t now_us,
                                       linkg_switch_policy_event_t *event,
                                       bool *event_ready)
{
    uint64_t fresh_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    uint64_t fresh_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    uint64_t ignored_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    uint64_t ignored_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT];
    bool     snapshot_valid;

    if (runtime == NULL || observation == NULL || event == NULL || event_ready == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *event_ready = false;

    if (!runtime->active)
    {
        return 0;
    }

    if (runtime->result_pending)
    {
        _linkg_switch_verify_make_event(runtime, event);
        *event_ready = true;
        return 0;
    }

    if (now_us < runtime->started_us || observation->peer_node_id != runtime->peer_node_id)
    {
        return -ESTALE;
    }

    snapshot_valid = observation->valid &&
                     _linkg_switch_verify_fresh(observation->collected_us, now_us,
                                                LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US);

    if (snapshot_valid &&
        (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
         observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE ||
         (observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
          observation->wifi.link_id != runtime->wifi_link_id) ||
         (observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE &&
          observation->cellular.link_id != runtime->cellular_link_id)))
    {
        runtime->path_interrupted = true;
    }

    if (snapshot_valid && !runtime->path_interrupted &&
        observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
        observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE &&
        observation->wifi.link_id == runtime->wifi_link_id &&
        observation->cellular.link_id == runtime->cellular_link_id)
    {
        memset(fresh_lost, 0, sizeof(fresh_lost));
        memset(fresh_samples, 0, sizeof(fresh_samples));
        memset(ignored_lost, 0, sizeof(ignored_lost));
        memset(ignored_samples, 0, sizeof(ignored_samples));

        _linkg_switch_verify_collect_wire(&runtime->wifi, &observation->wifi.loss,
                                          runtime->class_mask, runtime->started_us, runtime->deadline_us,
                                          now_us, fresh_lost, fresh_samples);

        _linkg_switch_verify_collect_wire(&runtime->cellular, &observation->cellular.loss,
                                          runtime->class_mask, runtime->started_us, runtime->deadline_us,
                                          now_us, ignored_lost, ignored_samples);

        _linkg_switch_verify_update_score(runtime, fresh_lost, fresh_samples);

        _linkg_switch_verify_collect_load(&runtime->wifi, &observation->wifi.loss,
                                          runtime->started_us, runtime->deadline_us, now_us);

        _linkg_switch_verify_collect_load(&runtime->cellular, &observation->cellular.loss,
                                          runtime->started_us, runtime->deadline_us, now_us);

        _linkg_switch_verify_collect_cellular_probe(runtime, observation->cellular.probe, now_us);
    }

    if (now_us < runtime->deadline_us)
    {
        return 0;
    }

    runtime->result         = _linkg_switch_verify_decide(runtime, observation, now_us);
    runtime->finished_us    = now_us;
    runtime->result_pending = true;

    _linkg_switch_verify_make_event(runtime, event);
    *event_ready = true;

    return 0;
}
