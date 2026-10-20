/**
 * @file switch_policy_load.c
 * @brief LinkG Wi-Fi发送队列溢出积分判决实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"

#include "switch_internal.h"

/****************************** 判决参数 ******************************/

#define LINKG_SWITCH_LOAD_BAD_SCORE     3U         // 触发一次分级业务迁移的共享积分门槛
#define LINKG_SWITCH_LOAD_IDLE_RESET_US 2000000ULL // 连续无有效新观测后清除旧积分

/****************************** 内部辅助 ******************************/

/**
 * @brief 比较完整发送计划的实际业务字段，不比较结构体填充字节。
 */
static bool _linkg_switch_load_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    uint32_t class_index;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (left->classes[class_index].mode != right->classes[class_index].mode ||
            left->classes[class_index].primary_link_id != right->classes[class_index].primary_link_id ||
            left->classes[class_index].secondary_link_id != right->classes[class_index].secondary_link_id)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 消费本轮两方向时间戳，并报告是否消费了至少一个新有效窗口。
 *
 * 无效窗口只更新时间戳，不作为结束同步过渡期的依据。
 * 调用方已持有Switch锁。
 */
static bool _linkg_switch_load_consume_current(const linkg_switch_path_loss_observation_t *loss, uint64_t now_us,
                                               linkg_switch_load_policy_runtime_t *detector)
{
    const linkg_switch_loss_observation_t *sample;
    bool                                   consumed_valid;

    consumed_valid = false;

    sample = &loss->uplink_load;
    if (sample->updated_us != 0U &&
        sample->updated_us <= now_us &&
        sample->updated_us > detector->last_uplink_updated_us)
    {
        detector->last_uplink_updated_us = sample->updated_us;
        consumed_valid = consumed_valid ||
                         (sample->valid && sample->sample_packets != 0U && sample->lost_packets <= sample->sample_packets);
    }

    sample = &loss->downlink_load;
    if (sample->updated_us != 0U &&
        sample->updated_us <= now_us &&
        sample->updated_us > detector->last_downlink_updated_us)
    {
        detector->last_downlink_updated_us = sample->updated_us;
        consumed_valid = consumed_valid ||
                         (sample->valid && sample->sample_packets != 0U && sample->lost_packets <= sample->sample_packets);
    }

    return consumed_valid;
}

/**
 * @brief 消费一个方向的新Queue Overflow观测。
 *
 * 返回true表示存在新的、有效的统计窗口；overflow返回是否发生队列溢出。
 * 无样本、无效样本和重复时间戳都不作为零溢出窗口。
 */
static bool _linkg_switch_load_evaluate_direction(const linkg_switch_loss_observation_t *sample, uint64_t now_us,
                                                  uint64_t *last_updated_us, bool *overflow)
{
    *overflow = false;

    if (!sample->valid || sample->updated_us == 0U ||
        sample->updated_us > now_us || sample->updated_us <= *last_updated_us)
    {
        return false;
    }

    // 新窗口先标记为已消费；非法样本不能在后续周期重复参与判断。
    *last_updated_us = sample->updated_us;

    if (sample->sample_packets == 0U || sample->lost_packets > sample->sample_packets)
    {
        return false;
    }

    *overflow = sample->lost_packets != 0U;

    return true;
}

/****************************** 队列溢出质量判断 ******************************/

/**
 * @brief 汇总Wi-Fi上、下行队列溢出并维护0～3分共享积分。
 *
 * 任一方向发生溢出，本轮只加1分；有有效新样本且都未溢出时减1分。
 * 两个方向都没有有效新样本时保持积分，不跨窗口累计报文数量。
 * 达到3分返回一次LOAD_BAD，并清零积分供下一迁移阶段重新计分。
 * 计划切换和PLAN_SYNC未确认期间，跳过过渡样本。
 */
int linkg_switch_policy_load_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_load_state_t *load_state)
{
    linkg_switch_load_policy_runtime_t         *detector;
    linkg_switch_plan_sync_runtime_t           *sync;
    linkg_switch_peer_runtime_t                *peer;
    const linkg_switch_path_loss_observation_t *loss;
    bool                                        ul_valid;
    bool                                        dl_valid;
    bool                                        ul_overflow;
    bool                                        dl_overflow;

    if (input == NULL || load_state == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *load_state = LINKG_SWITCH_POLICY_LOAD_NONE;

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

    // 输入快照可能在取得后被Reconcile或其它策略更新，旧Plan不得参与积分。
    if (!_linkg_switch_load_plan_equal(&peer->plan, &input->plan))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    detector = &peer->role.sta.wifi_load_policy;
    sync     = &peer->role.sta.plan_sync;
    loss     = &input->observation.wifi.loss;

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id ||
        input->wifi_blocked ||
        !input->observation.wifi.available ||
        input->observation.wifi.link_id == LINKG_LINK_ID_INVALID)
    {
        memset(detector, 0, sizeof(*detector));
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (!detector->initialized)
    {
        memset(detector, 0, sizeof(*detector));
        detector->initialized  = true;
        detector->wifi_link_id = input->observation.wifi.link_id;
        detector->plan         = input->plan;
    }
    else if (detector->wifi_link_id != input->observation.wifi.link_id ||
             !_linkg_switch_load_plan_equal(&detector->plan, &input->plan))
    {
        // 发送计划变化后不评价当前窗口，避免将切换前溢出计入下一阶段。
        memset(detector, 0, sizeof(*detector));
        detector->initialized  = true;
        detector->wifi_link_id = input->observation.wifi.link_id;
        detector->plan         = input->plan;
        detector->waiting_sync = true;
        _linkg_switch_load_consume_current(loss, now_us, detector);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 同步失败且未确认时停止推进；本地Plan变更主动取消的旧事务除外。
    if (sync->active || sync->recovery_pending ||
        (sync->message_id != LINKG_SWITCH_WIRE_MESSAGE_ID_INVALID &&
         !sync->confirmed && sync->last_status != -ECANCELED))
    {
        detector->score         = 0U;
        detector->last_valid_us = 0U;
        detector->waiting_sync  = true;
        _linkg_switch_load_consume_current(loss, now_us, detector);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (detector->waiting_sync)
    {
        // 同步完成后必须实际消费到新的有效窗口，才结束过渡期。
        // 无新观测不能解除等待；首个新有效窗口仅用于过渡，不参与积分。
        if (_linkg_switch_load_consume_current(loss, now_us, detector))
        {
            detector->waiting_sync = false;
        }

        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    if (detector->last_valid_us != 0U &&
        (now_us < detector->last_valid_us ||
         now_us - detector->last_valid_us >= LINKG_SWITCH_LOAD_IDLE_RESET_US))
    {
        detector->score         = 0U;
        detector->last_valid_us = 0U;
    }

    ul_valid = _linkg_switch_load_evaluate_direction(&loss->uplink_load,
                                                      now_us,
                                                      &detector->last_uplink_updated_us,
                                                      &ul_overflow);

    dl_valid = _linkg_switch_load_evaluate_direction(&loss->downlink_load,
                                                      now_us,
                                                      &detector->last_downlink_updated_us,
                                                      &dl_overflow);

    if (ul_valid || dl_valid)
    {
        detector->last_valid_us = now_us;

        if (ul_overflow || dl_overflow)
        {
            if (detector->score < LINKG_SWITCH_LOAD_BAD_SCORE)
            {
                detector->score++;
            }
        }
        else if (detector->score > 0U)
        {
            detector->score--;
        }
    }

    if (detector->score >= LINKG_SWITCH_LOAD_BAD_SCORE)
    {
        *load_state     = LINKG_SWITCH_POLICY_LOAD_BAD;
        detector->score = 0U;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}
