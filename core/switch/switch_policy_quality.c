/**
 * @file switch_policy_quality.c
 * @brief LinkG Wi-Fi负载压力与无线丢包统一质量判决
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-28
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"

#include "switch_internal.h"
#include "switch_policy_quality.h"

/****************************** 判决参数 ******************************/

#define LINKG_SWITCH_QUALITY_MIN_SAMPLES           20U        // 单方向累计达到20包进行一次Wire质量判决
#define LINKG_SWITCH_QUALITY_BAD_SCORE             5U         // LOAD/LOSS独立的Wire异常积分门槛
#define LINKG_SWITCH_QUALITY_WIRE_IDLE_RESET_US    2000000ULL // 连续2秒无合法非零Wire新样本时清除旧积分
#define LINKG_SWITCH_QUALITY_ACCUMULATE_MAX_US     2000000ULL // 单批Wire低样本累计最长保留2秒
#define LINKG_SWITCH_QUALITY_HCC_BAD_SCORE         3U         // HCC持续流控压力确认积分
#define LINKG_SWITCH_QUALITY_HCC_FAST_OFF_COUNT    5U         // 本地单250ms窗口OFF超过5次快速确认
#define LINKG_SWITCH_QUALITY_HCC_WINDOW_US         250000ULL  // HCC压力积分基础窗口
#define LINKG_SWITCH_QUALITY_HCC_MAX_AGE_US        1000000ULL // HCC观测最大年龄
#define LINKG_SWITCH_QUALITY_HCC_CANDIDATE_IDLE_US 2000000ULL // HCC候选积分2秒无新OFF自动老化
#define LINKG_SWITCH_QUALITY_HCC_RECOVERY_US       3000000ULL // HCC压力确认后的LOSS最短保护时间

/****************************** 检测器生命周期 ******************************/

/**
 * @brief 比较完整三业务发送计划，忽略结构体填充字节。
 */
static bool _linkg_switch_quality_plan_equal(const linkg_send_plan_t *left, const linkg_send_plan_t *right)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if (left->classes[index].mode != right->classes[index].mode ||
            left->classes[index].primary_link_id != right->classes[index].primary_link_id ||
            left->classes[index].secondary_link_id != right->classes[index].secondary_link_id)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 清除LOAD和LOSS积分及未完成Wire累计，保留已消费的观测时间戳。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_reset_scores_locked(linkg_switch_quality_policy_runtime_t *detector)
{
    detector->load_score          = 0U;
    detector->loss_score          = 0U;
    detector->last_wire_sample_us = 0U;
    detector->last_load_valid_us  = 0U;
    detector->last_loss_valid_us  = 0U;
    detector->wire_route          = LINKG_SWITCH_QUALITY_WIRE_SKIP;

    memset(&detector->uplink_accumulator, 0, sizeof(detector->uplink_accumulator));
    memset(&detector->downlink_accumulator, 0, sizeof(detector->downlink_accumulator));
}

/**
 * @brief 清除HCC压力积分及保护状态，保留上下行OFF累计差分基线。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_reset_hcc_pressure_locked(linkg_switch_quality_policy_runtime_t *detector)
{
    detector->last_hcc_window_us   = 0U;
    detector->last_hcc_pressure_us = 0U;
    detector->hcc_pressure_score   = 0U;
    detector->hcc_pressure_active  = false;
}

/**
 * @brief 消费单方向Wire或OVF观测，不参与任何质量评分。
 * 返回true表示消费了新的、有效的非零业务窗口。
 * 时间合法但样本内容无效时也推进去重时间戳。
 */
static bool _linkg_switch_quality_consume_sample_locked(const linkg_switch_loss_observation_t *sample, uint64_t observation_us, uint64_t *last_updated_us)
{
    if (sample->updated_us == 0U || sample->updated_us > observation_us || sample->updated_us <= *last_updated_us)
    {
        return false;
    }

    *last_updated_us = sample->updated_us;

    return sample->valid && sample->sample_packets != 0U && sample->lost_packets <= sample->sample_packets;
}

/**
 * @brief 消费有效HCC源观测，仅更新FLOWCTRL_OFF累计计数基线。
 * 不计算OFF差分，不参与HCC压力积分。
 */
static void _linkg_switch_quality_rebase_hcc_locked(const linkg_switch_wifi_hcc_observation_t *sample, uint64_t observation_us, linkg_switch_quality_hcc_state_t *state)
{
    if (!sample->valid || sample->updated_us == 0U || sample->updated_us > observation_us ||
        observation_us - sample->updated_us > LINKG_SWITCH_QUALITY_HCC_MAX_AGE_US ||
        sample->updated_us <= state->last_updated_us)
    {
        return;
    }

    state->last_updated_us = sample->updated_us;
    state->last_off_count  = sample->flowctrl_off_count;
    state->initialized     = true;
}

/**
 * @brief 消费当前Wire、OVF、HCC观测并建立去重基线，不产生积分。
 *
 * 返回true表示至少有一个新的有效Wire或OVF业务窗口。
 * HCC单独更新不解除PlanSync后的业务窗口隔离。
 * 调用方必须持有Switch锁。
 */
static bool _linkg_switch_quality_consume_current_locked(const linkg_switch_policy_input_t *input, linkg_switch_quality_policy_runtime_t *detector)
{
    const linkg_switch_wifi_observation_t *wifi;
    uint64_t                               observation_us;
    bool                                   ul_wire;
    bool                                   dl_wire;
    bool                                   ul_overflow;
    bool                                   dl_overflow;

    wifi           = &input->observation.wifi;
    observation_us = input->observation.collected_us;

    ul_wire = _linkg_switch_quality_consume_sample_locked(&wifi->loss.uplink, observation_us, &detector->last_wire_uplink_updated_us);
    dl_wire = _linkg_switch_quality_consume_sample_locked(&wifi->loss.downlink, observation_us, &detector->last_wire_downlink_updated_us);

    if (ul_wire && wifi->loss.uplink.updated_us > detector->last_wire_sample_us)
    {
        detector->last_wire_sample_us = wifi->loss.uplink.updated_us;
    }

    if (dl_wire && wifi->loss.downlink.updated_us > detector->last_wire_sample_us)
    {
        detector->last_wire_sample_us = wifi->loss.downlink.updated_us;
    }

    ul_overflow = _linkg_switch_quality_consume_sample_locked(&wifi->loss.uplink_load, observation_us, &detector->last_overflow_uplink_updated_us);
    dl_overflow = _linkg_switch_quality_consume_sample_locked(&wifi->loss.downlink_load, observation_us, &detector->last_overflow_downlink_updated_us);

    _linkg_switch_quality_rebase_hcc_locked(&wifi->uplink_hcc, observation_us, &detector->uplink_hcc);
    _linkg_switch_quality_rebase_hcc_locked(&wifi->downlink_hcc, observation_us, &detector->downlink_hcc);

    return ul_wire || dl_wire || ul_overflow || dl_overflow;
}

/**
 * @brief 为当前Wi-Fi Link及完整发送计划重新建立统一质量检测器。
 * 初始化窗口只用于建立观测基线，不参与评分。
 */
static void _linkg_switch_quality_initialize_locked(linkg_switch_quality_policy_runtime_t *detector, const linkg_switch_policy_input_t *input)
{
    memset(detector, 0, sizeof(*detector));

    detector->plan         = input->plan;
    detector->wifi_link_id = input->observation.wifi.link_id;
    detector->wire_route   = LINKG_SWITCH_QUALITY_WIRE_SKIP;
    detector->initialized  = true;
    detector->waiting_sync = true;

    _linkg_switch_quality_consume_current_locked(input, detector);
}

/**
 * @brief 检查Observation、Plan、Wi-Fi实例及PlanSync，维护统一检测器状态。
 *
 * 返回true表示允许执行本轮OVF/HCC/Wire质量判决。
 * 返回false表示仅维护状态并跳过本轮评分。
 * 调用方必须持有Switch锁。
 */
static bool _linkg_switch_quality_prepare_locked(const linkg_switch_policy_input_t *input, uint64_t now_us, const linkg_switch_peer_runtime_t *peer, linkg_switch_quality_policy_runtime_t *detector)
{
    const linkg_switch_plan_sync_runtime_t *sync;

    /****************************** 一、观测有效性 ******************************/

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id ||
        input->observation.collected_us == 0U ||
        input->observation.collected_us > now_us)
    {
        return false;
    }

    if (input->observation.wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
    {
        // 明确断链时销毁旧检测器，避免恢复后复用旧实例的积分。
        memset(detector, 0, sizeof(*detector));
        return false;
    }

    if (input->observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.wifi.link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    /****************************** 二、发送计划一致性 ******************************/

    if (!_linkg_switch_quality_plan_equal(&peer->plan, &input->plan))
    {
        return false;
    }

    /****************************** 三、维护与检测器实例 ******************************/

    if (input->wifi_blocked)
    {
        // 主动维护期间不保留旧积分；恢复后仍需新的业务窗口完成隔离。
        if (detector->initialized)
        {
            _linkg_switch_quality_reset_scores_locked(detector);
            _linkg_switch_quality_reset_hcc_pressure_locked(detector);
            detector->waiting_sync = true;
            _linkg_switch_quality_consume_current_locked(input, detector);
        }

        return false;
    }

    if (!detector->initialized ||
        detector->wifi_link_id != input->observation.wifi.link_id ||
        !_linkg_switch_quality_plan_equal(&detector->plan, &input->plan) ||
        (detector->last_wire_sample_us != 0U && now_us < detector->last_wire_sample_us) ||
        (detector->last_wire_uplink_updated_us != 0U && now_us < detector->last_wire_uplink_updated_us) ||
        (detector->last_wire_downlink_updated_us != 0U && now_us < detector->last_wire_downlink_updated_us) ||
        (detector->last_overflow_uplink_updated_us != 0U && now_us < detector->last_overflow_uplink_updated_us) ||
        (detector->last_overflow_downlink_updated_us != 0U && now_us < detector->last_overflow_downlink_updated_us) ||
        (detector->last_hcc_window_us != 0U && now_us < detector->last_hcc_window_us) ||
        (detector->uplink_hcc.last_updated_us != 0U && now_us < detector->uplink_hcc.last_updated_us) ||
        (detector->downlink_hcc.last_updated_us != 0U && now_us < detector->downlink_hcc.last_updated_us))
    {
        _linkg_switch_quality_initialize_locked(detector, input);
        return false;
    }

    /****************************** 四、PlanSync过渡保护 ******************************/

    sync = &peer->role.sta.plan_sync;

    if (sync->state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
        sync->state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT ||
        sync->state == LINKG_SWITCH_PLAN_SYNC_REJECTED ||
        sync->state == LINKG_SWITCH_PLAN_SYNC_UNCERTAIN)
    {
        _linkg_switch_quality_reset_scores_locked(detector);
        _linkg_switch_quality_reset_hcc_pressure_locked(detector);
        detector->waiting_sync = true;

        _linkg_switch_quality_consume_current_locked(input, detector);
        return false;
    }

    /****************************** 五、同步完成后的样本隔离 ******************************/

    if (detector->waiting_sync)
    {
        // 第一份新的有效业务窗口仅用于解除等待，不参与积分。
        if (_linkg_switch_quality_consume_current_locked(input, detector))
        {
            detector->waiting_sync = false;
        }

        return false;
    }

    /****************************** 六、无业务超时清理 ******************************/

    if (detector->last_wire_sample_us != 0U &&
        now_us - detector->last_wire_sample_us >= LINKG_SWITCH_QUALITY_WIRE_IDLE_RESET_US)
    {
        // 保留Wire/HCC/OVF观测去重基线；空闲只清理旧的质量积分。
        _linkg_switch_quality_reset_scores_locked(detector);
    }

    return true;
}

/****************************** 队列溢出检测 ******************************/

/**
 * @brief 消费单方向新的队列溢出观测，判断是否发生真实丢包。
 *
 * 只接受新的合法非零统计窗口；无效窗口也推进去重时间戳。
 * 返回true表示本轮存在新的OVF丢包，不要求连续积分。
 * 调用方必须持有Switch锁。
 */
static bool _linkg_switch_quality_overflow_direction_locked(const linkg_switch_loss_observation_t *sample, uint64_t observation_us, uint64_t *last_updated_us)
{
    if (sample->updated_us == 0U ||
        sample->updated_us > observation_us ||
        sample->updated_us <= *last_updated_us)
    {
        return false;
    }

    // 先消费时间戳，避免无效窗口被重复检查。
    *last_updated_us = sample->updated_us;

    if (!sample->valid ||
        sample->sample_packets == 0U ||
        sample->lost_packets > sample->sample_packets)
    {
        return false;
    }

    return sample->lost_packets > 0U;
}

/**
 * @brief 检查Wi-Fi双向新OVF窗口，任一方向确认溢出即返回true。
 *
 * 上下行必须各自消费，避免短路导致另一方向重复处理旧窗口。
 * 本函数仅判定溢出，不更新Wire/HCC采样基线或迁移事务。
 * 调用方必须持有Switch锁。
 */
static bool _linkg_switch_quality_overflow_locked(const linkg_switch_policy_input_t *input, linkg_switch_quality_policy_runtime_t *detector)
{
    const linkg_switch_path_loss_observation_t *loss;
    uint64_t                                    observation_us;
    bool                                        ul_overflow;
    bool                                        dl_overflow;

    loss           = &input->observation.wifi.loss;
    observation_us = input->observation.collected_us;

    ul_overflow = _linkg_switch_quality_overflow_direction_locked(&loss->uplink_load, observation_us, &detector->last_overflow_uplink_updated_us);
    dl_overflow = _linkg_switch_quality_overflow_direction_locked(&loss->downlink_load, observation_us, &detector->last_overflow_downlink_updated_us);

    return ul_overflow || dl_overflow;
}

/****************************** HCC流控压力检测 ******************************/

/**
 * @brief 消费单方向新的HCC观测，计算本次新增FLOWCTRL_OFF次数。
 *
 * 返回true表示获得了有效的新差分窗口；首次采样或计数回退仅重建基线。
 * tx_blocked反映最近有效观测中的当前禁止发送状态，不依赖新差分窗口。
 * 无效、过期或重复的观测不作为OFF=0的证据。
 * 调用方必须持有Switch锁。
 */
static bool _linkg_switch_quality_hcc_sample_locked(const linkg_switch_wifi_hcc_observation_t *sample, uint64_t observation_us, uint64_t now_us, linkg_switch_quality_hcc_state_t *state, uint32_t *off_delta, uint64_t *elapsed_us, bool *tx_blocked)
{
    *off_delta  = 0U;
    *elapsed_us = 0U;
    *tx_blocked = false;

    if (!sample->valid || sample->updated_us == 0U ||
        sample->updated_us > observation_us || sample->updated_us > now_us ||
        now_us - sample->updated_us > LINKG_SWITCH_QUALITY_HCC_MAX_AGE_US)
    {
        return false;
    }

    // 即使源时间戳未更新，仍应保留新鲜的Device发送禁止状态。
    *tx_blocked = !sample->tx_allowed;

    if (sample->updated_us <= state->last_updated_us)
    {
        return false;
    }

    // 首次观测和累计计数回退不提供有效OFF差分。
    if (!state->initialized || sample->flowctrl_off_count < state->last_off_count)
    {
        state->last_updated_us = sample->updated_us;
        state->last_off_count  = sample->flowctrl_off_count;
        state->initialized     = true;
        return false;
    }

    *elapsed_us = sample->updated_us - state->last_updated_us;
    *off_delta  = sample->flowctrl_off_count - state->last_off_count;

    state->last_updated_us = sample->updated_us;
    state->last_off_count  = sample->flowctrl_off_count;

    return true;
}

/**
 * @brief 更新HCC压力并确定本轮Wire质量积分归属。
 *
 * 新增OFF：Wire归LOAD；有效新HCC差分无新增OFF：LOAD积分减1；
 * 压力候选或确认保护期且无新增OFF：Wire跳过；无HCC压力：Wire归LOSS。
 * HCC缺失不等于无新增OFF，因此不据此递减LOAD积分，也不永久禁止LOSS。
 * 确认压力时只清除LOSS共享积分，保留LOAD积分及Wire当前窗口。
 * 不改变detector->wire_route；第七阶段负责切换归属时的低样本隔离。
 * 调用方必须持有Switch锁。
 */
static linkg_switch_quality_wire_route_t _linkg_switch_quality_hcc_route_locked(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_quality_policy_runtime_t *detector)
{
    const linkg_switch_wifi_observation_t *wifi;
    uint64_t                               observation_us;
    uint64_t                               ul_elapsed_us;
    uint64_t                               dl_elapsed_us;
    uint64_t                               time_gap_us;
    uint32_t                               ul_off;
    uint32_t                               dl_off;
    uint32_t                               next_score;
    bool                                   ul_valid;
    bool                                   dl_valid;
    bool                                   ul_blocked;
    bool                                   dl_blocked;
    bool                                   off_detected;
    bool                                   simultaneous_off;
    bool                                   fast_pressure;

    wifi           = &input->observation.wifi;
    observation_us = input->observation.collected_us;

    /****************************** 一、候选积分老化 ******************************/

    if (!detector->hcc_pressure_active && detector->hcc_pressure_score > 0U &&
        detector->last_hcc_pressure_us != 0U && now_us >= detector->last_hcc_pressure_us &&
        now_us - detector->last_hcc_pressure_us >= LINKG_SWITCH_QUALITY_HCC_CANDIDATE_IDLE_US)
    {
        detector->hcc_pressure_score = 0U;
    }

    /****************************** 二、HCC新样本及OFF差分 ******************************/

    // 单方向用源时间戳独立去重；同一份Observation不会被重复积分。
    ul_valid = _linkg_switch_quality_hcc_sample_locked(&wifi->uplink_hcc, observation_us, now_us, &detector->uplink_hcc, &ul_off, &ul_elapsed_us, &ul_blocked);
    dl_valid = _linkg_switch_quality_hcc_sample_locked(&wifi->downlink_hcc, observation_us, now_us, &detector->downlink_hcc, &dl_off, &dl_elapsed_us, &dl_blocked);

    if (detector->last_hcc_window_us < observation_us)
    {
        detector->last_hcc_window_us = observation_us;
    }

    off_detected = (ul_valid && ul_off > 0U) || (dl_valid && dl_off > 0U);

    /****************************** 三、单窗口快速确认 ******************************/

    // 只对STA本地UL采用OFF>5快速确认；AP的接收时间不能证明真实250ms跨度。
    fast_pressure = ul_valid && ul_elapsed_us <= LINKG_SWITCH_QUALITY_HCC_WINDOW_US &&
                    ul_off > LINKG_SWITCH_QUALITY_HCC_FAST_OFF_COUNT;

    /****************************** 四、共享HCC压力积分 ******************************/

    time_gap_us = wifi->uplink_hcc.updated_us >= wifi->downlink_hcc.updated_us ?
                  wifi->uplink_hcc.updated_us - wifi->downlink_hcc.updated_us :
                  wifi->downlink_hcc.updated_us - wifi->uplink_hcc.updated_us;

    // +2仅用于两个源的新增OFF窗口足够接近，避免跨多个窗口误合并。
    simultaneous_off = ul_valid && dl_valid && ul_off > 0U && dl_off > 0U &&
                       ul_elapsed_us <= LINKG_SWITCH_QUALITY_HCC_WINDOW_US &&
                       dl_elapsed_us <= LINKG_SWITCH_QUALITY_HCC_WINDOW_US &&
                       time_gap_us <= LINKG_SWITCH_QUALITY_HCC_WINDOW_US;

    if (off_detected)
    {
        next_score = (uint32_t)detector->hcc_pressure_score + 1U + (simultaneous_off ? 1U : 0U);

        detector->hcc_pressure_score = (uint8_t)(next_score >= LINKG_SWITCH_QUALITY_HCC_BAD_SCORE ?
                                                LINKG_SWITCH_QUALITY_HCC_BAD_SCORE : next_score);
        detector->last_hcc_pressure_us = now_us;
    }
    else if ((ul_valid || dl_valid) && !ul_blocked && !dl_blocked)
    {
        // 至少一个方向的新HCC差分确认本轮无新增OFF，HCC压力积分回落1分。
        if (detector->hcc_pressure_score > 0U)
        {
            detector->hcc_pressure_score--;
        }

        // 本轮缺少新的负载流控证据，LOAD积分独立回落1分。
        // 不要求Wire GOOD：即使Wire BAD，也由LOSS路径在HCC保护解除后处理。
        if (detector->load_score > 0U)
        {
            detector->load_score--;
        }
    }

    /****************************** 五、确认压力并隔离LOSS积分 ******************************/

    if (fast_pressure || (off_detected && detector->hcc_pressure_score >= LINKG_SWITCH_QUALITY_HCC_BAD_SCORE))
    {
        if (!detector->hcc_pressure_active)
        {
            // 仅清除无线质量评分；LOAD积分及本轮Wire样本仍可参与负载判决。
            detector->loss_score         = 0U;
            detector->last_loss_valid_us = 0U;
        }

        detector->hcc_pressure_score   = LINKG_SWITCH_QUALITY_HCC_BAD_SCORE;
        detector->hcc_pressure_active  = true;
        detector->last_hcc_pressure_us = now_us;
    }

    /****************************** 六、Device当前流控状态 ******************************/

    if (ul_blocked || dl_blocked)
    {
        if (detector->hcc_pressure_active)
        {
            detector->last_hcc_pressure_us = now_us;
        }

        // 本轮若已确认新增OFF，仍交由LOAD处理业务丢包；否则暂停Wire评分。
        return off_detected ? LINKG_SWITCH_QUALITY_WIRE_LOAD : LINKG_SWITCH_QUALITY_WIRE_SKIP;
    }

    /****************************** 七、确认压力恢复 ******************************/

    if (detector->hcc_pressure_active && detector->last_hcc_pressure_us != 0U &&
        now_us >= detector->last_hcc_pressure_us &&
        now_us - detector->last_hcc_pressure_us >= LINKG_SWITCH_QUALITY_HCC_RECOVERY_US)
    {
        // 无新OFF证据持续3秒，且没有当前Device阻塞证据，解除LOSS保护。
        detector->hcc_pressure_active = false;
        detector->hcc_pressure_score  = 0U;
    }

    /****************************** 八、Wire样本归属 ******************************/

    // 新OFF优先归LOAD；本轮即使HCC达到3分，也不能丢掉属于LOAD的Wire样本。
    if (off_detected)
    {
        return LINKG_SWITCH_QUALITY_WIRE_LOAD;
    }

    if (detector->hcc_pressure_active || detector->hcc_pressure_score > 0U)
    {
        return LINKG_SWITCH_QUALITY_WIRE_SKIP;
    }

    // 无新HCC观测且没有有效压力，不额外阻止无线质量评分。
    return LINKG_SWITCH_QUALITY_WIRE_LOSS;
}

/****************************** Wire样本归属 ******************************/

/**
 * @brief 按本轮HCC结果切换Wire累计路径，隔离不同来源的低样本批次。
 *
 * LOAD/LOSS共享上下行的Wire去重时间戳，但积分和已完成结果互相独立。
 * 路径切换只清除不足20包的累计批次，不清除LOAD/LOSS共享积分。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_select_wire_route_locked(linkg_switch_quality_policy_runtime_t *detector, linkg_switch_quality_wire_route_t wire_route)
{
    if (detector->wire_route == wire_route)
    {
        return;
    }

    memset(&detector->uplink_accumulator, 0, sizeof(detector->uplink_accumulator));
    memset(&detector->downlink_accumulator, 0, sizeof(detector->downlink_accumulator));

    detector->wire_route = wire_route;
}

/**
 * @brief 消费本轮Wire新窗口，更新业务活跃时间，但不执行质量判决。
 *
 * 仅在WIRE_SKIP时使用；不修改HCC差分基线，也不改变LOAD/LOSS积分。
 * 第七阶段的路径切换已经清除了旧的低样本累计。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_consume_wire_current_locked(const linkg_switch_policy_input_t *input, linkg_switch_quality_policy_runtime_t *detector)
{
    const linkg_switch_wifi_observation_t *wifi;
    uint64_t                               observation_us;
    bool                                   ul_valid;
    bool                                   dl_valid;

    wifi           = &input->observation.wifi;
    observation_us = input->observation.collected_us;

    ul_valid = _linkg_switch_quality_consume_sample_locked(&wifi->loss.uplink, observation_us, &detector->last_wire_uplink_updated_us);
    dl_valid = _linkg_switch_quality_consume_sample_locked(&wifi->loss.downlink, observation_us, &detector->last_wire_downlink_updated_us);

    if (ul_valid && wifi->loss.uplink.updated_us > detector->last_wire_sample_us)
    {
        detector->last_wire_sample_us = wifi->loss.uplink.updated_us;
    }

    if (dl_valid && wifi->loss.downlink.updated_us > detector->last_wire_sample_us)
    {
        detector->last_wire_sample_us = wifi->loss.downlink.updated_us;
    }
}

/****************************** Wire质量判决 ******************************/

/**
 * @brief 判断Wire窗口是否达到BAD丢包阈值。
 *
 * BAD要求loss >= max(0.5%, 50% / sqrt(N))。
 * 等价整数条件：200 * L >= N && 4 * L * L >= N。
 * 调用方保证N >= 20，且L <= N。
 */
static bool _linkg_switch_quality_wire_bad(uint64_t samples, uint64_t lost_packets)
{
    // 使用除法计算ceil(N / 200)，避免乘法溢出。
    if (lost_packets < samples / 200ULL + (samples % 200ULL != 0U ? 1ULL : 0ULL))
    {
        return false;
    }

    // 只有L >= 2^31时4*L*L才可能超过uint64_t范围，此时必定满足第二条件。
    return lost_packets >= 2147483648ULL || 4ULL * lost_packets * lost_packets >= samples;
}

/**
 * @brief 判断Wire窗口是否达到GOOD丢包阈值。
 *
 * GOOD要求loss <= min(1%, BAD阈值的一半)。
 * N < 10000：100 * L <= N && 16 * L * L <= N。
 * N >= 10000：400 * L <= N，即丢包率不超过0.25%。
 */
static bool _linkg_switch_quality_wire_good(uint64_t samples, uint64_t lost_packets)
{
    if (samples >= 10000ULL)
    {
        return lost_packets <= UINT64_MAX / 400ULL && 400ULL * lost_packets <= samples;
    }

    return 100ULL * lost_packets <= samples && 16ULL * lost_packets * lost_packets <= samples;
}

/**
 * @brief 超过2秒的未完成小样本批次不能继续与新窗口合并。
 */
static void _linkg_switch_quality_expire_accumulator_locked(linkg_switch_quality_accumulator_t *accumulator, uint64_t now_us)
{
    if (accumulator->first_updated_us != 0U &&
        (now_us < accumulator->first_updated_us ||
         now_us - accumulator->first_updated_us > LINKG_SWITCH_QUALITY_ACCUMULATE_MAX_US))
    {
        memset(accumulator, 0, sizeof(*accumulator));
    }
}

/**
 * @brief 消费单方向新Wire样本，每累计至少20包输出一次BAD/GOOD评分增量。
 *
 * BAD返回+1，GOOD返回-1，其他情况返回0。
 * 每份源观测仅消费一次；非法样本和空窗口都不能当作GOOD。
 * 有效非零窗口推进业务活跃时间，完成一次评分时推进对应路径的有效时间。
 */
static int _linkg_switch_quality_evaluate_direction_locked(const linkg_switch_loss_observation_t *sample, uint64_t observation_us, uint64_t now_us, uint64_t *last_updated_us, uint64_t *last_sample_us, uint64_t *last_valid_us, linkg_switch_quality_accumulator_t *accumulator)
{
    uint64_t samples;
    uint64_t lost_packets;

    _linkg_switch_quality_expire_accumulator_locked(accumulator, now_us);

    if (sample->updated_us == 0U || sample->updated_us > observation_us ||
        sample->updated_us > now_us || sample->updated_us <= *last_updated_us)
    {
        return 0;
    }

    // 源时间戳合法时先去重，包括无效样本，避免后续重复计入。
    *last_updated_us = sample->updated_us;

    if (!sample->valid)
    {
        return 0;
    }

    if (sample->lost_packets > sample->sample_packets)
    {
        // 非法计数不得与当前未完成的样本批次混合。
        memset(accumulator, 0, sizeof(*accumulator));
        return 0;
    }

    if (sample->sample_packets == 0U)
    {
        return 0;
    }

    if (sample->updated_us > *last_sample_us)
    {
        *last_sample_us = sample->updated_us;
    }

    if (accumulator->first_updated_us == 0U)
    {
        accumulator->first_updated_us = sample->updated_us;
    }

    if (accumulator->sample_packets > UINT64_MAX - sample->sample_packets ||
        accumulator->lost_packets > UINT64_MAX - sample->lost_packets)
    {
        // 累加计数溢出时，仅重新建立当前批次基线。
        memset(accumulator, 0, sizeof(*accumulator));
        accumulator->first_updated_us = sample->updated_us;
    }

    accumulator->sample_packets += sample->sample_packets;
    accumulator->lost_packets   += sample->lost_packets;

    if (accumulator->sample_packets < LINKG_SWITCH_QUALITY_MIN_SAMPLES)
    {
        return 0;
    }

    samples      = accumulator->sample_packets;
    lost_packets = accumulator->lost_packets;

    memset(accumulator, 0, sizeof(*accumulator));
    *last_valid_us = now_us;

    if (_linkg_switch_quality_wire_bad(samples, lost_packets))
    {
        return 1;
    }

    if (_linkg_switch_quality_wire_good(samples, lost_packets))
    {
        return -1;
    }

    return 0;
}

/**
 * @brief 对本轮Wire唯一归属路径计算上下行质量积分，并输出质量事件。
 *
 * LOAD/LOSS使用相同BAD/GOOD门槛和0～5分算法，但共享Wire样本去重。
 * 每个方向最多一次评分，单轮正积分最多+2、负积分最多-1。
 * 积分达到5后只清除本路径积分及未完成的批次，不影响另一条路径积分。
 * 调用方必须持有Switch锁。
 */
static linkg_switch_policy_quality_state_t _linkg_switch_quality_evaluate_wire_locked(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_quality_policy_runtime_t *detector, linkg_switch_quality_wire_route_t wire_route)
{
    const linkg_switch_path_loss_observation_t *loss;
    uint64_t                                   *last_valid_us;
    uint8_t                                    *score;
    uint32_t                                    next_score;
    int                                         ul_delta;
    int                                         dl_delta;
    int                                         delta;

    if (wire_route != LINKG_SWITCH_QUALITY_WIRE_LOAD && wire_route != LINKG_SWITCH_QUALITY_WIRE_LOSS)
    {
        return LINKG_SWITCH_POLICY_QUALITY_NONE;
    }

    loss = &input->observation.wifi.loss;

    if (wire_route == LINKG_SWITCH_QUALITY_WIRE_LOAD)
    {
        score         = &detector->load_score;
        last_valid_us = &detector->last_load_valid_us;
    }
    else
    {
        score         = &detector->loss_score;
        last_valid_us = &detector->last_loss_valid_us;
    }

    ul_delta = _linkg_switch_quality_evaluate_direction_locked(&loss->uplink, input->observation.collected_us, now_us, &detector->last_wire_uplink_updated_us, &detector->last_wire_sample_us, last_valid_us, &detector->uplink_accumulator);
    dl_delta = _linkg_switch_quality_evaluate_direction_locked(&loss->downlink, input->observation.collected_us, now_us, &detector->last_wire_downlink_updated_us, &detector->last_wire_sample_us, last_valid_us, &detector->downlink_accumulator);

    delta = ul_delta + dl_delta;

    if (delta > 0)
    {
        next_score = (uint32_t)(*score) + (uint32_t)delta;
        *score     = (uint8_t)(next_score >= LINKG_SWITCH_QUALITY_BAD_SCORE ? LINKG_SWITCH_QUALITY_BAD_SCORE : next_score);
    }
    else if (delta < 0 && *score > 0U)
    {
        (*score)--;
    }

    if (*score < LINKG_SWITCH_QUALITY_BAD_SCORE)
    {
        return LINKG_SWITCH_POLICY_QUALITY_NONE;
    }

    // 输出一次异常后只重置对应路径积分，PlanSync/Operation负责后续事务去重。
    *score = 0U;
    memset(&detector->uplink_accumulator, 0, sizeof(detector->uplink_accumulator));
    memset(&detector->downlink_accumulator, 0, sizeof(detector->downlink_accumulator));

    return wire_route == LINKG_SWITCH_QUALITY_WIRE_LOAD ?
           LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD : LINKG_SWITCH_POLICY_QUALITY_LOSS_BAD;
}

/****************************** 统一质量判决入口 ******************************/

/**
 * @brief 基于Wi-Fi双向队列溢出、HCC流控和Wire丢包输出唯一质量结果。
 *
 * 本函数仅负责质量判决，不修改发送Plan，不启动PlanSync或Operation事务。
 * 返回NONE表示本轮没有形成迁移事件，不表示Wi-Fi质量一定良好。
 */
int linkg_switch_policy_quality_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_quality_state_t *quality_state)
{
    linkg_switch_quality_policy_runtime_t *detector;
    linkg_switch_peer_runtime_t           *peer;
    linkg_switch_quality_wire_route_t      wire_route;

    /****************************** 一、参数校验 ******************************/

    if (input == NULL || quality_state == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *quality_state = LINKG_SWITCH_POLICY_QUALITY_NONE;

    pthread_mutex_lock(&g_switch.lock);

    /****************************** 二、Switch运行状态 ******************************/

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

    /****************************** 三、Peer身份校验 ******************************/

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    /****************************** 四、检测器生命周期 ******************************/

    detector = &peer->role.sta.wifi_quality_policy;

    if (!_linkg_switch_quality_prepare_locked(input, now_us, peer, detector))
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 五、队列溢出检测 ******************************/

    // 新有效OVF窗口确认数据丢弃，直接输出LOAD_BAD，不等待Wire质量积分。
    if (_linkg_switch_quality_overflow_locked(input, detector))
    {
        // 队列溢出已证实负载损失，丢弃旧Wire质量积分及未完成批次。
        _linkg_switch_quality_reset_scores_locked(detector);

        // 当前窗口已归属LOAD：更新Wire/HCC基线，不把本轮重复用于后续积分。
        _linkg_switch_quality_consume_current_locked(input, detector);

        *quality_state = LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD;

        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 六、HCC流控压力检测 ******************************/

    wire_route = _linkg_switch_quality_hcc_route_locked(input, now_us, detector);

    /****************************** 七、Wire样本归属 ******************************/

    // LOAD/LOSS/SKIP切换时隔离低样本批次，历史有效积分保持独立。
    _linkg_switch_quality_select_wire_route_locked(detector, wire_route);

    /****************************** 八、Wire质量判决 ******************************/

    if (wire_route == LINKG_SWITCH_QUALITY_WIRE_SKIP)
    {
        // 无新增OFF且HCC保护仍有效：只消费Wire，不参与任一路径的积分。
        _linkg_switch_quality_consume_wire_current_locked(input, detector);
    }
    else
    {
        // 本轮最多处理一次上下行Wire，并仅更新所选路径的积分。
        *quality_state = _linkg_switch_quality_evaluate_wire_locked(input, now_us, detector, wire_route);
    }

    /****************************** 九、异常结果输出 ******************************/

    // OVF已经在第五阶段优先返回；这里的LOAD/LOSS互斥，始终最多输出一个状态。

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}
