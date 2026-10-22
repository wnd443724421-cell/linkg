/**
 * @file switch_policy_verify.c
 * @brief LinkG冗余阶段无线丢包恢复积分及双链路交付质量验证
 */

#include "switch_policy_verify.h"

#include <errno.h>
#include <limits.h>
#include <stddef.h>
#include <string.h>

/****************************** Wi-Fi积分数学判据 ******************************/

/**
 * @brief 与switch_policy_loss.c相同的无线BAD/GOOD数学门槛。
 *
 * 当前LOSS检测器达到5分会清零；Verify必须有自己的恢复积分，
 * 并从5分开始累积本次冗余ACK之后的新样本，不能读取被清零的检测器积分。
 * 后续审核LOSS时可以将以下纯数学函数抽取为共享辅助函数。
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

/****************************** 样本累计 ******************************/

/**
 * @brief 丢包计数使用饱和累积，不能让极端计数溢出翻转成小值。
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
 * @brief 仅消费本次验证开始之后的新窗口；不重复计入AP上报的缓存样本。
 */
static void _linkg_switch_verify_consume_loss(linkg_switch_policy_verify_path_t *path,
                                               const linkg_switch_path_loss_observation_t *loss,
                                               uint32_t class_mask,
                                               uint64_t started_us,
                                               uint64_t deadline_us,
                                               uint64_t now_us,
                                               uint64_t fresh_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT],
                                               uint64_t fresh_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT])
{
    const linkg_switch_loss_observation_t *sample;
    uint32_t                               direction;
    uint32_t                               class_index;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
        {
            if ((class_mask & (1U << class_index)) == 0U)
            {
                continue;
            }

            sample = direction == LINKG_SWITCH_POLICY_VERIFY_UPLINK ?
                         &loss->uplink_classes[class_index] :
                         &loss->downlink_classes[class_index];

            if (!sample->valid || sample->sample_packets == 0U ||
                sample->lost_packets > sample->sample_packets ||
                sample->updated_us <= started_us ||
                sample->updated_us > deadline_us || sample->updated_us > now_us ||
                sample->updated_us <= path->last_loss_us[direction][class_index])
            {
                continue;
            }

            path->last_loss_us[direction][class_index] = sample->updated_us;

            _linkg_switch_verify_accumulate(&path->lost_packets[direction], sample->lost_packets);
            _linkg_switch_verify_accumulate(&path->sample_packets[direction], sample->sample_packets);
            _linkg_switch_verify_accumulate(&path->class_samples[class_index], sample->sample_packets);
            _linkg_switch_verify_accumulate(&fresh_lost[direction], sample->lost_packets);
            _linkg_switch_verify_accumulate(&fresh_samples[direction], sample->sample_packets);
        }
    }
}

/**
 * @brief 记录整链路发送队列丢弃。不能直接加到业务Wire丢包中：
 *        现有队列丢弃缺少业务类别，并且可能已经被接收端Wire序列窗口计入。
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

        if (!sample->valid || sample->lost_packets > sample->sample_packets ||
            sample->updated_us <= started_us ||
            sample->updated_us > deadline_us || sample->updated_us > now_us ||
            sample->updated_us <= path->last_load_us[direction])
        {
            continue;
        }

        path->last_load_us[direction] = sample->updated_us;
        if (sample->lost_packets != 0U)
        {
            path->queue_overflow = true;
        }
    }
}

/**
 * @brief 收集新Probe；任何明确不可达或超250ms结果都会否决本轮5G候选。
 */
static void _linkg_switch_verify_consume_probe(linkg_switch_policy_verify_path_t *path,
                                                const linkg_switch_probe_observation_t probe[LINKG_TRANSPORT_CLASS_COUNT],
                                                uint32_t class_mask,
                                                uint64_t started_us,
                                                uint64_t deadline_us,
                                                uint64_t now_us)
{
    const linkg_switch_probe_observation_t *sample;
    uint32_t                                class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        sample = &probe[class_index];
        if (!sample->valid || sample->updated_us <= started_us ||
            sample->updated_us > deadline_us || sample->updated_us > now_us ||
            sample->updated_us <= path->last_probe_us[class_index])
        {
            continue;
        }

        path->last_probe_us[class_index] = sample->updated_us;
        if (!sample->reachable || sample->rtt_us == 0U ||
            sample->rtt_us >= LINKG_SWITCH_POLICY_VERIFY_RTT_LIMIT_US)
        {
            path->probe_failed = true;
            continue;
        }

        path->probe_seen_mask |= 1U << class_index;
        if (sample->rtt_us > path->max_rtt_us)
        {
            path->max_rtt_us = sample->rtt_us;
        }
    }
}

/****************************** 恢复积分 ******************************/

static void _linkg_switch_verify_update_wifi_score(linkg_switch_policy_verify_runtime_t *runtime,
                                                    const uint64_t fresh_lost[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT],
                                                    const uint64_t fresh_samples[LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT])
{
    int32_t  delta;
    uint32_t direction;

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
        uint32_t next = (uint32_t)runtime->wifi_loss_score + (uint32_t)delta;
        runtime->wifi_loss_score = next >= LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE ?
                                      LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE : (uint8_t)next;
    }
    else if (delta < 0 && runtime->wifi_loss_score > 0U)
    {
        // 与LOSS触发检测器一致：单轮两个GOOD最多减1分。
        runtime->wifi_loss_score--;
    }
}

/****************************** 最终判决 ******************************/

/**
 * @brief 比较两条路径在同一业务集合中的Wire交付率。
 *
 * 必须有双链路相同业务集合的对应方向样本；不允许只拿Wi-Fi UL
 * 与5G DL比较，也不允许把无样本当成0丢包。
 * 队列溢出仅用于保护5G候选，不重复相加到Wire损失数量。
 */
static bool _linkg_switch_verify_cellular_wire_better(const linkg_switch_policy_verify_runtime_t *runtime)
{
    uint64_t wifi_lost;
    uint64_t wifi_samples;
    uint64_t cellular_lost;
    uint64_t cellular_samples;
    uint32_t direction;
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if ((runtime->class_mask & (1U << class_index)) == 0U)
        {
            continue;
        }

        // 本组的任一活跃业务缺少候选链路样本，不可用其它业务的包代替它证明质量。
        if ((runtime->wifi.class_samples[class_index] == 0U) !=
            (runtime->cellular.class_samples[class_index] == 0U))
        {
            return false;
        }
    }

    wifi_lost        = 0U;
    wifi_samples     = 0U;
    cellular_lost    = 0U;
    cellular_samples = 0U;

    for (direction = 0U; direction < LINKG_SWITCH_POLICY_VERIFY_DIRECTION_COUNT; direction++)
    {
        const uint64_t wifi_n = runtime->wifi.sample_packets[direction];
        const uint64_t cell_n = runtime->cellular.sample_packets[direction];

        if (wifi_n == 0U && cell_n == 0U)
        {
            continue;
        }

        if (wifi_n == 0U || cell_n == 0U)
        {
            return false;
        }

        _linkg_switch_verify_accumulate(&wifi_lost, runtime->wifi.lost_packets[direction]);
        _linkg_switch_verify_accumulate(&wifi_samples, wifi_n);
        _linkg_switch_verify_accumulate(&cellular_lost, runtime->cellular.lost_packets[direction]);
        _linkg_switch_verify_accumulate(&cellular_samples, cell_n);
    }

    if (wifi_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES ||
        cellular_samples < LINKG_SWITCH_POLICY_VERIFY_COMPARE_MIN_SAMPLES)
    {
        return false;
    }

    // 每3秒仅比较一次；浮点运算避免64位计数交叉相乘的溢出。
    return (double)cellular_lost / (double)cellular_samples <
           (double)wifi_lost / (double)wifi_samples;
}

static linkg_switch_policy_verify_result_t _linkg_switch_verify_result(const linkg_switch_policy_verify_runtime_t *runtime,
                                                                       const linkg_switch_observation_t *observation,
                                                                       uint64_t now_us)
{
    // Link已替换或任何一侧不再USABLE：不对过期路径下结论；交由硬故障流程接管。
    if (runtime->path_interrupted || !observation->valid ||
        observation->collected_us == 0U || observation->collected_us > now_us ||
        now_us - observation->collected_us > 1000000ULL ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id != runtime->wifi_link_id ||
        observation->cellular.link_id != runtime->cellular_link_id)
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    // 5进入、低于3退出；Wi-Fi恢复优先于5G交付率比较。
    if (runtime->wifi_loss_score < LINKG_SWITCH_POLICY_VERIFY_RECOVER_SCORE)
    {
        return LINKG_SWITCH_POLICY_VERIFY_WIFI;
    }

    // RECOVERY未确认Wi-Fi恢复时，保留原有5G单发；无需尝试证明5G更好。
    if (runtime->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
    }

    if ((runtime->cellular.probe_seen_mask & runtime->class_mask) != runtime->class_mask ||
        runtime->cellular.probe_failed ||
        runtime->cellular.queue_overflow ||
        !_linkg_switch_verify_cellular_wire_better(runtime))
    {
        return LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE;
    }

    return LINKG_SWITCH_POLICY_VERIFY_CELLULAR;
}

/****************************** 对外接口 ******************************/

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
                                     uint32_t operation_id,
                                     uint32_t class_mask,
                                     linkg_switch_policy_reason_t reason,
                                     const linkg_switch_observation_t *observation,
                                     uint64_t now_us)
{
    if (runtime == NULL || observation == NULL ||
        peer_node_id == 0U || peer_generation == 0U || operation_id == 0U ||
        class_mask == 0U || (class_mask & ~LINKG_SWITCH_POLICY_CLASS_MASK_ALL) != 0U ||
        (reason != LINKG_SWITCH_POLICY_REASON_LOSS && reason != LINKG_SWITCH_POLICY_REASON_RECOVERY) ||
        !observation->valid || observation->peer_node_id != peer_node_id ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        now_us == 0U || now_us > UINT64_MAX - LINKG_SWITCH_POLICY_VERIFY_DURATION_US)
    {
        return -EINVAL;
    }

    // 同一Peer只允许一个正在执行的整组验证；不得覆盖旧事务。
    if (runtime->active)
    {
        return -EBUSY;
    }

    memset(runtime, 0, sizeof(*runtime));
    runtime->active           = true;
    runtime->peer_node_id     = peer_node_id;
    runtime->peer_generation  = peer_generation;
    runtime->operation_id     = operation_id;
    runtime->class_mask       = class_mask;
    runtime->reason           = reason;
    runtime->wifi_link_id     = observation->wifi.link_id;
    runtime->cellular_link_id = observation->cellular.link_id;
    runtime->started_us       = now_us;
    runtime->deadline_us      = now_us + LINKG_SWITCH_POLICY_VERIFY_DURATION_US;
    runtime->wifi_loss_score  = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;

    return 0;
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

    if (runtime == NULL || observation == NULL || event == NULL || event_ready == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *event_ready = false;
    if (!runtime->active || runtime->reported)
    {
        return 0;
    }

    if (observation->peer_node_id != runtime->peer_node_id ||
        now_us < runtime->started_us)
    {
        return -ESTALE;
    }

    memset(fresh_lost, 0, sizeof(fresh_lost));
    memset(fresh_samples, 0, sizeof(fresh_samples));
    memset(ignored_lost, 0, sizeof(ignored_lost));
    memset(ignored_samples, 0, sizeof(ignored_samples));

    if (!observation->valid ||
        observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id != runtime->wifi_link_id ||
        observation->cellular.link_id != runtime->cellular_link_id)
    {
        runtime->path_interrupted = true;
    }

    if (!runtime->path_interrupted)
    {
        _linkg_switch_verify_consume_loss(&runtime->wifi, &observation->wifi.loss,
                                           runtime->class_mask, runtime->started_us, runtime->deadline_us, now_us,
                                           fresh_lost, fresh_samples);

        _linkg_switch_verify_consume_loss(&runtime->cellular, &observation->cellular.loss,
                                           runtime->class_mask, runtime->started_us, runtime->deadline_us, now_us,
                                           ignored_lost, ignored_samples);

        _linkg_switch_verify_update_wifi_score(runtime, fresh_lost, fresh_samples);

        _linkg_switch_verify_consume_load(&runtime->wifi, &observation->wifi.loss,
                                           runtime->started_us, runtime->deadline_us, now_us);
        _linkg_switch_verify_consume_load(&runtime->cellular, &observation->cellular.loss,
                                           runtime->started_us, runtime->deadline_us, now_us);

        _linkg_switch_verify_consume_probe(&runtime->cellular, observation->cellular.probe,
                                            runtime->class_mask, runtime->started_us, runtime->deadline_us, now_us);
    }

    if (now_us < runtime->deadline_us)
    {
        return 0;
    }

    memset(event, 0, sizeof(*event));
    event->type                            = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
    event->peer_node_id                    = runtime->peer_node_id;
    event->peer_generation                 = runtime->peer_generation;
    event->occurred_us                     = now_us;
    event->payload.verify_done.operation_id = runtime->operation_id;
    event->payload.verify_done.result       = _linkg_switch_verify_result(runtime, observation, now_us);

    runtime->reported = true;
    *event_ready = true;
    return 0;
}
