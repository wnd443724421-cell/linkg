/**
 * @file switch_policy_loss.c
 * @brief LinkG Wi-Fi丢包质量判决实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-25
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"

#include "switch_internal.h"

/****************************** 判决参数 ******************************/

#define LINKG_SWITCH_LOSS_MIN_SAMPLES        20U        // 单方向累计达到20包才判决一次
#define LINKG_SWITCH_LOSS_BAD_SCORE          5U         // 进入冗余验证的共享积分门槛
#define LINKG_SWITCH_LOSS_IDLE_RESET_US      2000000ULL // 连续2秒没有合法非零新样本才清除积分
#define LINKG_SWITCH_LOSS_ACCUMULATE_MAX_US  2000000ULL // 单批低样本累计最长保留2秒

/****************************** 单方向质量门槛 ******************************/

/**
 * @brief 判断累计样本是否达到异常加分门槛。
 *
 * BAD要求loss >= max(0.5%, 50% / sqrt(N))。
 * 使用整数运算：200 * L >= N、4 * L * L >= N。
 * 调用方保证N >= 20，因此动态阈值隐含至少丢3包。
 */
static bool _linkg_switch_loss_bad(uint64_t samples, uint64_t lost_packets)
{
    // 短时累计的计数按uint64_t保存，乘法前先处理极大值。
    if (lost_packets < (UINT64_MAX / 200ULL) && 200ULL * lost_packets < samples)
    {
        return false;
    }

    // 避免计算4 * L * L时发生无符号整数溢出。
    return lost_packets >= 32768ULL || 4ULL * lost_packets * lost_packets >= samples;
}

/**
 * @brief 判断累计样本是否达到良好减分门槛。
 *
 * GOOD要求loss <= min(1%, BAD百分比阈值的一半)。
 * N < 10000：100 * L <= N且16 * L * L <= N。
 * N >= 10000：400 * L <= N，即loss <= 0.25%。
 */
static bool _linkg_switch_loss_good(uint64_t samples, uint64_t lost_packets)
{
    if (samples >= 10000ULL)
    {
        return lost_packets <= UINT64_MAX / 400ULL && 400ULL * lost_packets <= samples;
    }

    return 100ULL * lost_packets <= samples && 16ULL * lost_packets * lost_packets <= samples;
}

/****************************** 单方向低样本累计 ******************************/

/**
 * @brief 超出最大累计时长时丢弃当前批次，不改变质量积分。
 */
static void _linkg_switch_loss_expire_accumulator(linkg_switch_loss_accumulator_t *accumulator, uint64_t now_us)
{
    if (accumulator->first_updated_us != 0U &&
        (now_us < accumulator->first_updated_us ||
         now_us - accumulator->first_updated_us > LINKG_SWITCH_LOSS_ACCUMULATE_MAX_US))
    {
        memset(accumulator, 0, sizeof(*accumulator));
    }
}

/**
 * @brief 消费一个方向的新观测，累计至少20包后只输出一次评分增量。
 *
 * 小样本窗口按更新时间去重并逐批累计；样本数达到20后立即判决、清空批次。
 * 仅合法非零的新窗口刷新活跃时间，不以判决频率衡量业务是否空闲。
 * 样本无效、重复、数量为0或累计超时都不会被当作GOOD。
 * 质量评分规则保持BAD +1、GOOD -1、其它0不变。
 */
static int _linkg_switch_loss_evaluate_direction(const linkg_switch_loss_observation_t *sample,
                                                 uint64_t now_us,
                                                 uint64_t *last_updated_us,
                                                 uint64_t *last_sample_us,
                                                 uint64_t *last_valid_us,
                                                 linkg_switch_loss_accumulator_t *accumulator)
{
    uint64_t samples;
    uint64_t lost_packets;

    // 即使本轮没有新的样本，也不能无限期保存旧的零散样本。
    _linkg_switch_loss_expire_accumulator(accumulator, now_us);

    if (!sample->valid ||
        sample->updated_us == 0U ||
        sample->updated_us > now_us ||
        sample->updated_us <= *last_updated_us)
    {
        return 0;
    }

    // 每份观测只消费一次，包括数量非法或为零的窗口。
    *last_updated_us = sample->updated_us;

    if (sample->lost_packets > sample->sample_packets)
    {
        // 计数不可信时丢弃未完成的批次，禁止与后续样本混合。
        memset(accumulator, 0, sizeof(*accumulator));
        return 0;
    }

    if (sample->sample_packets == 0U)
    {
        return 0;
    }

    // 只有合法、非零、且本方向未消费过的新窗口，才能证明链路仍有业务样本。
    // 两个方向共用活跃时间，因此只允许向前推进，不能被较旧的远端报告回退。
    if (sample->updated_us > *last_sample_us)
    {
        *last_sample_us = sample->updated_us;
    }

    // 第一份有效新样本建立该方向的累计起点。
    if (accumulator->first_updated_us == 0U)
    {
        accumulator->first_updated_us = sample->updated_us;
    }

    // 无符号计数累加不允许溢出；溢出视为异常并重新建立当前批次。
    if (accumulator->sample_packets > UINT64_MAX - sample->sample_packets ||
        accumulator->lost_packets > UINT64_MAX - sample->lost_packets)
    {
        memset(accumulator, 0, sizeof(*accumulator));
        accumulator->first_updated_us = sample->updated_us;
    }

    accumulator->sample_packets += sample->sample_packets;
    accumulator->lost_packets   += sample->lost_packets;

    // 未达到20包，保留本批样本，不产生BAD/GOOD判决。
    if (accumulator->sample_packets < LINKG_SWITCH_LOSS_MIN_SAMPLES)
    {
        return 0;
    }

    // 本批样本无论超过20包多少，都只执行一次质量判决。
    samples      = accumulator->sample_packets;
    lost_packets = accumulator->lost_packets;
    memset(accumulator, 0, sizeof(*accumulator));
    *last_valid_us = now_us;

    if (_linkg_switch_loss_bad(samples, lost_packets))
    {
        return 1;
    }

    if (_linkg_switch_loss_good(samples, lost_packets))
    {
        return -1;
    }

    return 0;
}

/****************************** 丢包质量判断 ******************************/

/**
 * @brief 根据STA本轮Wi-Fi UL/DL Wire丢包更新共享异常积分。
 *
 * 上下行分别累计小样本，达到20包才各自判决一次；质量积分仍为共享积分。
 * 两方向均BAD单轮最多+2，方向相反相互抵消；减分单轮最多-1。
 * 共享积分达到5时仅输出一次BAD并清零，后续由Operation处理冗余验证。
 */
int linkg_switch_policy_loss_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_loss_state_t *loss_state)
{
    linkg_switch_loss_policy_runtime_t *detector;
    linkg_switch_peer_runtime_t        *peer;
    uint32_t                            next_score;
    int                                 delta;
    int                                 ul_delta;
    int                                 dl_delta;

    if (input == NULL || loss_state == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *loss_state = LINKG_SWITCH_POLICY_LOSS_NONE;

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ENODEV;
    }

    if (!g_switch.running)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESHUTDOWN;
    }

    if (g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -EPERM;
    }

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    detector = &peer->role.sta.wifi_loss_policy;

    /****************************** 检查观测及Wi-Fi实例 ******************************/

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id ||
        input->observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.wifi.link_id == LINKG_LINK_ID_INVALID)
    {
        memset(detector, 0, sizeof(*detector));
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // Wi-Fi实例变化、首次处理或单调时间回退时重建完整检测器。
    if (!detector->initialized ||
        detector->wifi_link_id != input->observation.wifi.link_id ||
        (detector->last_sample_us != 0U && now_us < detector->last_sample_us))
    {
        memset(detector, 0, sizeof(*detector));
        detector->initialized  = true;
        detector->wifi_link_id = input->observation.wifi.link_id;
    }
    else if (detector->last_sample_us != 0U &&
             now_us - detector->last_sample_us >= LINKG_SWITCH_LOSS_IDLE_RESET_US)
    {
        // 真正连续2秒无新业务样本，才清理积分和未完成的累计批次。
        // 保留各方向最后消费的updated_us，避免重复快照被重置后再次计分。
        detector->score          = 0U;
        detector->last_sample_us = 0U;
        detector->last_valid_us  = 0U;
        memset(&detector->uplink_accumulator, 0, sizeof(detector->uplink_accumulator));
        memset(&detector->downlink_accumulator, 0, sizeof(detector->downlink_accumulator));
    }

    /****************************** 分别累计上下行质量样本 ******************************/

    ul_delta = _linkg_switch_loss_evaluate_direction(&input->observation.wifi.loss.uplink,
                                                     now_us,
                                                     &detector->last_uplink_updated_us,
                                                     &detector->last_sample_us,
                                                     &detector->last_valid_us,
                                                     &detector->uplink_accumulator);

    dl_delta = _linkg_switch_loss_evaluate_direction(&input->observation.wifi.loss.downlink,
                                                     now_us,
                                                     &detector->last_downlink_updated_us,
                                                     &detector->last_sample_us,
                                                     &detector->last_valid_us,
                                                     &detector->downlink_accumulator);

    /****************************** 维持原有共享积分 ******************************/

    delta = ul_delta + dl_delta;

    if (delta > 0)
    {
        next_score      = (uint32_t)detector->score + (uint32_t)delta;
        detector->score = (uint8_t)(next_score >= LINKG_SWITCH_LOSS_BAD_SCORE ?
                                    LINKG_SWITCH_LOSS_BAD_SCORE : next_score);
    }
    else if (delta < 0 && detector->score > 0U)
    {
        // 双向均GOOD时单轮仍最多只减1，保留原有恢复滞后。
        detector->score--;
    }

    if (detector->score >= LINKG_SWITCH_LOSS_BAD_SCORE)
    {
        *loss_state     = LINKG_SWITCH_POLICY_LOSS_BAD;
        detector->score = 0U; // 一次性触发，避免冗余事务后立即再次触发。

        // 已触发本轮质量迁移，未满20包的旧批次也不能带入下次事务。
        memset(&detector->uplink_accumulator, 0, sizeof(detector->uplink_accumulator));
        memset(&detector->downlink_accumulator, 0, sizeof(detector->downlink_accumulator));
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}
