
/**
 * @file switch_policy_loss.c
 * @brief LinkG Wi-Fi丢包质量判决实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-08
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"

#include "switch_internal.h"

/****************************** 判决参数 ******************************/

#define LINKG_SWITCH_LOSS_MIN_SAMPLES   20U        // 单方向本轮最低有效样本数
#define LINKG_SWITCH_LOSS_BAD_SCORE     5U         // 进入冗余验证的共享积分门槛
#define LINKG_SWITCH_LOSS_IDLE_RESET_US 2000000ULL // 连续无有效样本后清除旧积分

/****************************** 单方向质量判断 ******************************/

/**
 * @brief 判断本轮是否达到随样本数变化的加分门槛。
 *
 * 要求loss >= max(0.5%, 50% / sqrt(N))。
 * 转换为整数判断：200 * L >= N、4 * L * L >= N。
 * 调用方保证N >= 20，因此动态阈值已经隐含至少丢3包。
 */
static bool _linkg_switch_loss_bad(uint32_t samples, uint64_t lost_packets)
{
    // 丢包率不足0.5%，不满足最低加分门槛。
    if (200ULL * lost_packets < samples)
    {
        return false;
    }

    // 样本数为uint32_t，避免L较大时的平方运算溢出。
    return lost_packets >= 32768ULL || 4ULL * lost_packets * lost_packets >= samples;
}

/**
 * @brief 判断本轮是否满足严格的减分门槛。
 *
 * GOOD要求loss <= min(1%, BAD百分比阈值的一半)。
 * N < 10000：100 * L <= N且16 * L * L <= N。
 * N >= 10000：400 * L <= N，即loss <= 0.25%。
 */
static bool _linkg_switch_loss_good(uint32_t samples, uint64_t lost_packets)
{
    if (samples >= 10000U)
    {
        return 400ULL * lost_packets <= samples;
    }

    return 100ULL * lost_packets <= samples && 16ULL * lost_packets * lost_packets <= samples;
}

/**
 * @brief 消费一个方向的新观测，输出-1、0或+1。
 *
 * 样本不足20、报告重复或数据无效时不改变积分。
 * 每份窗口最多处理一次，不跨窗口累计丢包样本。
 */
static int _linkg_switch_loss_evaluate_direction(const linkg_switch_loss_observation_t *sample, uint64_t now_us, uint64_t *last_updated_us, uint64_t *last_valid_us)
{
    if (!sample->valid ||
        sample->updated_us == 0U ||
        sample->updated_us > now_us ||
        sample->updated_us <= *last_updated_us)
    {
        return 0;
    }

    // 无论样本是否足够，都先标记本轮已消费，防止重复计分。
    *last_updated_us = sample->updated_us;

    if (sample->sample_packets < LINKG_SWITCH_LOSS_MIN_SAMPLES ||
        sample->lost_packets > sample->sample_packets)
    {
        return 0;
    }

    *last_valid_us = now_us;

    if (_linkg_switch_loss_bad(sample->sample_packets, sample->lost_packets))
    {
        return 1;
    }

    if (_linkg_switch_loss_good(sample->sample_packets, sample->lost_packets))
    {
        return -1;
    }

    return 0;
}

/****************************** 丢包质量判断 ******************************/

/**
 * @brief 根据STA本轮Wi-Fi UL/DL Wire丢包更新共享异常积分。
 *
 * 每方向独立评估，不跨窗口累计丢包样本；只保存一个共享积分。
 * 两个方向都BAD时单轮+2，方向相反时相互抵消；减分单轮最多-1。
 * 达到5分仅输出一次BAD并清零积分，冗余验证及最终迁移由上层处理。
 */
int linkg_switch_policy_loss_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_loss_state_t *loss_state)
{
    linkg_switch_loss_policy_runtime_t *detector;
    linkg_switch_peer_runtime_t        *peer;
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

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id ||
        !input->observation.wifi.available ||
        input->observation.wifi.link_id == LINKG_LINK_ID_INVALID)
    {
        memset(detector, 0, sizeof(*detector));
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (!detector->initialized ||
        detector->wifi_link_id != input->observation.wifi.link_id ||
        (detector->last_valid_us != 0U &&
         (now_us < detector->last_valid_us ||
          now_us - detector->last_valid_us >= LINKG_SWITCH_LOSS_IDLE_RESET_US)))
    {
        memset(detector, 0, sizeof(*detector));
        detector->initialized  = true;
        detector->wifi_link_id = input->observation.wifi.link_id;
    }

    ul_delta = _linkg_switch_loss_evaluate_direction(&input->observation.wifi.loss.uplink,
                                                     now_us,
                                                     &detector->last_uplink_updated_us,
                                                     &detector->last_valid_us);

    dl_delta = _linkg_switch_loss_evaluate_direction(&input->observation.wifi.loss.downlink,
                                                     now_us,
                                                     &detector->last_downlink_updated_us,
                                                     &detector->last_valid_us);

    delta = ul_delta + dl_delta;

    if (delta > 0)
    {
        uint32_t next_score = (uint32_t)detector->score + (uint32_t)delta;

        detector->score = (uint8_t)(next_score >= LINKG_SWITCH_LOSS_BAD_SCORE ? LINKG_SWITCH_LOSS_BAD_SCORE : next_score);
    }
    else if (delta < 0 && detector->score > 0U)
    {
        // 双向都良好也只减1，保留异常积分的恢复滞后。
        detector->score--;
    }

    if (detector->score >= LINKG_SWITCH_LOSS_BAD_SCORE)
    {
        *loss_state     = LINKG_SWITCH_POLICY_LOSS_BAD;
        detector->score = 0U; // 一次性触发，避免退出冗余后沿用满分立即重入。
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}
