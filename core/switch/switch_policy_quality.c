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
#include "linkg_path_probe.h"
#include "linkg_time.h"
#include "linkg_transport.h"

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
#define LINKG_SWITCH_QUALITY_FAST_PERIOD_US        250000ULL  // 单发Probe与快速积分窗口250ms
#define LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS       200U       // 单包Probe网络响应超时200ms
#define LINKG_SWITCH_QUALITY_FAST_ACTIVE_AGE_MS    1500ULL    // 只探测最近有真实USER_DATA的业务
#define LINKG_SWITCH_QUALITY_FAST_STREAK_GAP_US    750000ULL  // 两次有效TIMEOUT间隔超过750ms不再视为连续
#define LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC     0x01U      // 本窗口HCC新增OFF
#define LINKG_SWITCH_QUALITY_FAST_EVIDENCE_WIRE    0x02U      // 本窗口Wire BAD丢包
#define LINKG_SWITCH_QUALITY_FAST_EVIDENCE_PROBE   0x04U      // 本窗口有效Probe超时

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

/****************************** Wi-Fi单发异步负载探测 ******************************/

typedef struct
{
    uint64_t handles[LINKG_TRANSPORT_CLASS_COUNT];
    uint64_t last_submit_us[LINKG_TRANSPORT_CLASS_COUNT];
    linkg_send_plan_t plan;
    uint8_t  peer_node_id;
    uint32_t peer_generation;
    uint32_t wifi_link_id;
    bool     initialized;
} linkg_switch_quality_fast_probe_runtime_t;

typedef struct
{
    uint32_t active_mask;  // 当前有真实业务且Wi-Fi单发的类别
    uint32_t ack_mask;     // 当前轮收到合格ACK的类别
    uint32_t timeout_mask; // 当前轮真实TIMEOUT的类别
    uint32_t break_mask;   // 当前轮结果无效/发送失败/取消，打断连续超时但不加分
} linkg_switch_quality_fast_result_t;

// 当前平台STA仅有一个直连AP；该状态只在唯一Switch Worker上下文中读写。
static linkg_switch_quality_fast_probe_runtime_t g_quality_fast_probe;

/**
 * @brief 取消单发阶段异步探测，释放各业务Probe槽位。
 * 不持有Switch锁，避免Probe锁与Switch锁反向嵌套。
 */
static void _linkg_switch_quality_fast_reset(void)
{
    uint32_t index;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        if (g_quality_fast_probe.handles[index] != 0U)
        {
            (void)linkg_path_probe_async_cancel(g_quality_fast_probe.handles[index]);
        }
    }

    memset(&g_quality_fast_probe, 0, sizeof(g_quality_fast_probe));
}

/**
 * @brief 当前是否允许Wi-Fi单发快速探测。
 */
static bool _linkg_switch_quality_fast_eligible(const linkg_switch_policy_input_t *input)
{
    uint32_t index;

    if (input->wifi_blocked || input->cellular_blocked ||
        input->observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.wifi.link_id == LINKG_LINK_ID_INVALID ||
        input->observation.cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        const linkg_class_send_plan_t *item = &input->plan.classes[index];
        if (item->mode == LINKG_SEND_MODE_SINGLE &&
            item->primary_link_id == input->observation.wifi.link_id &&
            item->secondary_link_id == input->observation.cellular.link_id)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 非阻塞消费每类单包Probe并按250ms间隔提交下一包。
 * 仅实际发送后形成的TIMEOUT可作为未收到ACK的加分证据。
 */
static void _linkg_switch_quality_fast_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_quality_fast_result_t *output)
{
    linkg_transport_peer_stats_t   stats;
    linkg_path_probe_async_result_t result;
    uint64_t                       now_ms;
    uint64_t                       handle;
    uint32_t                       index;
    uint32_t                       bit;
    bool                           active;
    int                            ret;

    memset(output, 0, sizeof(*output));

    if (!_linkg_switch_quality_fast_eligible(input))
    {
        _linkg_switch_quality_fast_reset();
        return;
    }

    if (!g_quality_fast_probe.initialized ||
        g_quality_fast_probe.peer_node_id != input->peer_node_id ||
        g_quality_fast_probe.peer_generation != input->peer_generation ||
        g_quality_fast_probe.wifi_link_id != input->observation.wifi.link_id ||
        !_linkg_switch_quality_plan_equal(&g_quality_fast_probe.plan, &input->plan))
    {
        _linkg_switch_quality_fast_reset();
        g_quality_fast_probe.initialized     = true;
        g_quality_fast_probe.peer_node_id    = input->peer_node_id;
        g_quality_fast_probe.peer_generation = input->peer_generation;
        g_quality_fast_probe.wifi_link_id    = input->observation.wifi.link_id;
        g_quality_fast_probe.plan            = input->plan;
    }

    // 与Operation的LOAD活跃定义一致；不把空闲DATA Probe当作VIDEO异常。
    ret = linkg_transport_get_peer_stats(input->peer_node_id, &stats);
    if (ret != 0)
    {
        return;
    }

    now_ms = linkg_time_elapsed_ms();

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        const linkg_class_send_plan_t *item = &input->plan.classes[index];
        const linkg_transport_class_stats_t *class_stats = &stats.classes[index];

        bit = 1U << index;
        active = (class_stats->last_user_tx_ms != 0U &&
                  class_stats->last_user_tx_ms <= now_ms &&
                  now_ms - class_stats->last_user_tx_ms <= LINKG_SWITCH_QUALITY_FAST_ACTIVE_AGE_MS) ||
                 (class_stats->last_user_rx_ms != 0U &&
                  class_stats->last_user_rx_ms <= now_ms &&
                  now_ms - class_stats->last_user_rx_ms <= LINKG_SWITCH_QUALITY_FAST_ACTIVE_AGE_MS);
        active = active && item->mode == LINKG_SEND_MODE_SINGLE &&
                 item->primary_link_id == input->observation.wifi.link_id &&
                 item->secondary_link_id == input->observation.cellular.link_id;

        handle = g_quality_fast_probe.handles[index];
        if (!active)
        {
            if (handle != 0U)
            {
                (void)linkg_path_probe_async_cancel(handle);
                g_quality_fast_probe.handles[index] = 0U;
            }
            g_quality_fast_probe.last_submit_us[index] = 0U;
            continue;
        }

        output->active_mask |= bit;

        if (handle != 0U)
        {
            memset(&result, 0, sizeof(result));
            ret = linkg_path_probe_async_poll(handle, &result);
            if (ret == 0 && result.state != LINKG_PATH_PROBE_ASYNC_PENDING)
            {
                g_quality_fast_probe.handles[index] = 0U;
                if (result.traffic_class == (linkg_transport_class_t)index &&
                    result.sent_us != 0U && result.sent_us <= now_us &&
                    result.completed_us >= result.sent_us)
                {
                    if (result.state == LINKG_PATH_PROBE_ASYNC_TIMEOUT)
                    {
                        output->timeout_mask |= bit;
                    }
                    else if (result.state == LINKG_PATH_PROBE_ASYNC_ACK &&
                             result.rtt_us < (uint32_t)LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS * 1000U)
                    {
                        output->ack_mask |= bit;
                    }
                    else
                    {
                        // ACK超出合格时延或其它终态，均不可伪造连续TIMEOUT。
                        output->break_mask |= bit;
                    }
                }
                else
                {
                    // 无效时间戳/业务标记不参与评分，也不能连接前后TIMEOUT。
                    output->break_mask |= bit;
                }
            }
            else if (ret != 0)
            {
                (void)linkg_path_probe_async_cancel(handle);
                g_quality_fast_probe.handles[index] = 0U;
                output->break_mask |= bit;
            }
        }

        if (g_quality_fast_probe.handles[index] == 0U &&
            (g_quality_fast_probe.last_submit_us[index] == 0U ||
             (now_us >= g_quality_fast_probe.last_submit_us[index] &&
              now_us - g_quality_fast_probe.last_submit_us[index] >= LINKG_SWITCH_QUALITY_FAST_PERIOD_US)))
        {
            handle = 0U;
            ret = linkg_path_probe_async_submit(input->peer_node_id,
                                                input->observation.wifi.link_id,
                                                (linkg_transport_class_t)index,
                                                LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS,
                                                &handle);
            if (ret == 0)
            {
                g_quality_fast_probe.handles[index] = handle;
                g_quality_fast_probe.last_submit_us[index] = now_us;
            }
            // -EBUSY和发送失败不是网络超时，不允许用它们加减分。
        }
    }
}

/**
 * @brief 清除LOAD和LOSS积分及未完成Wire累计，保留已消费的观测时间戳。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_reset_scores_locked(linkg_switch_quality_policy_runtime_t *detector)
{
    detector->load_score              = 0U;
    detector->loss_score              = 0U;
    detector->last_wire_sample_us     = 0U;
    detector->last_load_valid_us      = 0U;
    detector->last_loss_valid_us      = 0U;
    detector->wire_route              = LINKG_SWITCH_QUALITY_WIRE_SKIP;
    detector->fast_window_started_us  = 0U;
    detector->fast_window_evidence    = 0U;
    detector->fast_window_ack_mask    = 0U;
    detector->fast_window_probe_points= 0U;
    detector->fast_window_active_mask = 0U;
    memset(detector->fast_probe_timeout_streak, 0, sizeof(detector->fast_probe_timeout_streak));
    memset(detector->fast_probe_last_timeout_us, 0, sizeof(detector->fast_probe_last_timeout_us));

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

    // 初始化/PlanSync隔离期间的Wire不能在快速LOAD窗口被二次计分。
    if (wifi->loss.uplink.updated_us != 0U &&
        wifi->loss.uplink.updated_us <= observation_us &&
        wifi->loss.uplink.updated_us > detector->fast_wire_uplink_us)
    {
        detector->fast_wire_uplink_us = wifi->loss.uplink.updated_us;
    }
    if (wifi->loss.downlink.updated_us != 0U &&
        wifi->loss.downlink.updated_us <= observation_us &&
        wifi->loss.downlink.updated_us > detector->fast_wire_downlink_us)
    {
        detector->fast_wire_downlink_us = wifi->loss.downlink.updated_us;
    }

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
 * 新增OFF：Wire归LOAD，且通知快速LOAD计分；无新增OFF不再独立扣减LOAD；
 * 压力候选或确认保护期且无新增OFF：Wire跳过；无HCC压力：Wire归LOSS。
 * HCC缺失不等于无新增OFF，因此不据此递减LOAD积分，也不永久禁止LOSS。
 * 确认压力时只清除LOSS共享积分，保留LOAD积分及Wire当前窗口。
 * 不改变detector->wire_route；第七阶段负责切换归属时的低样本隔离。
 * 调用方必须持有Switch锁。
 */
static linkg_switch_quality_wire_route_t _linkg_switch_quality_hcc_route_locked(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_quality_policy_runtime_t *detector, bool *new_off)
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
    *new_off = off_detected;

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

        // LOAD减分改由250ms快速窗口在收到合格ACK且无BAD证据后执行。
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
 * 这里只累计LOSS积分；LOAD统一由250ms HCC/Wire/Probe证据窗口评分。
 * 每个方向最多一次评分，LOSS单轮正积分最多+2、负积分最多-1。
 * LOSS积分达到5后清除LOSS积分及未完成的批次。
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

    if (wire_route != LINKG_SWITCH_QUALITY_WIRE_LOSS)
    {
        return LINKG_SWITCH_POLICY_QUALITY_NONE;
    }

    loss = &input->observation.wifi.loss;

    score         = &detector->loss_score;
    last_valid_us = &detector->last_loss_valid_us;

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

    return LINKG_SWITCH_POLICY_QUALITY_LOSS_BAD;
}

/****************************** 统一质量判决入口 ******************************/

/**
 * @brief 基于OVF、HCC OFF、Wire BAD和同业务Probe输出质量结果。
 *
 * 本函数仅负责质量判决，不修改发送Plan，不启动PlanSync或Operation事务。
 * 返回NONE表示本轮没有形成迁移事件，不表示Wi-Fi质量一定良好。
 */
/**
 * @brief 独立检查Wire BAD，避免与LOSS评分窗口共用去重时间戳。
 * 无效和重复样本不构成负载证据；每方向每窗口最多提供一个WIRE证据。
 */
static bool _linkg_switch_quality_fast_wire_bad(const linkg_switch_loss_observation_t *sample, uint64_t observation_us, uint64_t *last_us)
{
    if (sample->updated_us == 0U || sample->updated_us > observation_us ||
        sample->updated_us <= *last_us)
    {
        return false;
    }

    *last_us = sample->updated_us;
    return sample->valid && sample->sample_packets >= LINKG_SWITCH_QUALITY_MIN_SAMPLES &&
           sample->lost_packets <= sample->sample_packets &&
           _linkg_switch_quality_wire_bad(sample->sample_packets, sample->lost_packets);
}

/**
 * @brief 按业务类别维护连续Probe TIMEOUT并计算本轮探测证据权重。
 *
 * 首次TIMEOUT +1，750ms内再次连续TIMEOUT权重为+2（后续保持+2）。
 * 合格ACK/不可信终态打断连续性；PENDING不计分，由时间间隔保护避免跨长时间复用。
 * 多业务同窗口只取一个Probe证据权重，避免按业务数量重复加分。
 */
static void _linkg_switch_quality_fast_update_probe_locked(linkg_switch_quality_policy_runtime_t *detector, const linkg_switch_quality_fast_result_t *probe, uint64_t now_us)
{
    uint64_t last_us;
    uint32_t index;
    uint32_t bit;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        bit = 1U << index;
        if ((probe->active_mask & bit) == 0U)
        {
            continue;
        }

        if (((probe->ack_mask | probe->break_mask) & bit) != 0U)
        {
            detector->fast_probe_timeout_streak[index]  = 0U;
            detector->fast_probe_last_timeout_us[index] = 0U;
            continue;
        }

        if ((probe->timeout_mask & bit) == 0U)
        {
            // PENDING/本轮无终态：保持连续性，但不凭空增加次数。
            continue;
        }

        last_us = detector->fast_probe_last_timeout_us[index];
        if (last_us != 0U && now_us >= last_us &&
            now_us - last_us <= LINKG_SWITCH_QUALITY_FAST_STREAK_GAP_US &&
            detector->fast_probe_timeout_streak[index] > 0U)
        {
            // 连续第二次及以后统一按+2，不突破全局单窗+2上限。
            detector->fast_probe_timeout_streak[index] = 2U;
        }
        else
        {
            detector->fast_probe_timeout_streak[index] = 1U;
        }

        detector->fast_probe_last_timeout_us[index] = now_us;
        detector->fast_window_evidence |= LINKG_SWITCH_QUALITY_FAST_EVIDENCE_PROBE;
        if (detector->fast_window_probe_points < detector->fast_probe_timeout_streak[index])
        {
            detector->fast_window_probe_points = detector->fast_probe_timeout_streak[index];
        }
    }
}

/**
 * @brief 合并250ms的HCC OFF、Wire BAD及按类别加权的Probe TIMEOUT，总加分最多+2。
 * 只有所有活跃Wi-Fi单发业务均收到合格ACK且本窗口没有BAD，才减1分。
 * 活跃集合变化时清除旧LOAD积分和Probe连续性并重建窗口，避免跨业务借用证据。
 * 无活跃业务时只推进快速Wire去重基线，不参与LOAD评分。
 */
static bool _linkg_switch_quality_fast_score_locked(linkg_switch_quality_policy_runtime_t *detector, const linkg_switch_policy_input_t *input, const linkg_switch_quality_fast_result_t *probe, bool hcc_off, uint64_t now_us)
{
    const linkg_switch_path_loss_observation_t *loss = &input->observation.wifi.loss;
    uint32_t evidence;
    uint32_t plus;
    bool     active_changed;
    bool     uplink_bad;
    bool     downlink_bad;

    active_changed = detector->fast_window_active_mask != probe->active_mask;

    if (active_changed)
    {
        // 活跃集合变更后，旧业务的积分、ACK和BAD证据均不得传给新业务。
        // 新窗口从本轮开始计时；本轮尚未覆盖完整250ms，只重建采样基线。
        detector->load_score               = 0U;
        detector->fast_window_started_us   = now_us;
        detector->fast_window_evidence     = 0U;
        detector->fast_window_ack_mask     = 0U;
        detector->fast_window_probe_points = 0U;
        detector->fast_window_active_mask = probe->active_mask;
        memset(detector->fast_probe_timeout_streak, 0, sizeof(detector->fast_probe_timeout_streak));
        memset(detector->fast_probe_last_timeout_us, 0, sizeof(detector->fast_probe_last_timeout_us));
    }
    else if (detector->fast_window_started_us == 0U || now_us < detector->fast_window_started_us)
    {
        detector->fast_window_started_us = now_us;
    }

    // 即使当前没有活跃业务，也必须消费上下行新Wire窗口，防止后续重复加分。
    uplink_bad   = _linkg_switch_quality_fast_wire_bad(&loss->uplink, input->observation.collected_us, &detector->fast_wire_uplink_us);
    downlink_bad = _linkg_switch_quality_fast_wire_bad(&loss->downlink, input->observation.collected_us, &detector->fast_wire_downlink_us);

    if (active_changed || probe->active_mask == 0U)
    {
        // 空业务不允许LOAD加减分或输出LOAD_BAD；活跃集合变更首轮也不计分。
        // LOSS的Wire判决与HCC压力更新由调用方独立执行，不在此修改。
        if (probe->active_mask == 0U)
        {
            detector->load_score               = 0U;
            detector->fast_window_evidence     = 0U;
            detector->fast_window_ack_mask     = 0U;
            detector->fast_window_probe_points = 0U;
            detector->fast_window_started_us   = now_us;
        }

        return false;
    }

    if (hcc_off)
    {
        detector->fast_window_evidence |= LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC;
    }

    if (uplink_bad || downlink_bad)
    {
        detector->fast_window_evidence |= LINKG_SWITCH_QUALITY_FAST_EVIDENCE_WIRE;
    }

    // Probe结果以业务类别为单位计连续性，不能让不同类别拼接成连续TIMEOUT。
    _linkg_switch_quality_fast_update_probe_locked(detector, probe, now_us);

    detector->fast_window_ack_mask |= probe->ack_mask;

    if (now_us - detector->fast_window_started_us < LINKG_SWITCH_QUALITY_FAST_PERIOD_US)
    {
        return false;
    }

    evidence = detector->fast_window_evidence;
    plus     = (evidence & LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC ? 1U : 0U) +
               (evidence & LINKG_SWITCH_QUALITY_FAST_EVIDENCE_WIRE ? 1U : 0U) +
               (evidence & LINKG_SWITCH_QUALITY_FAST_EVIDENCE_PROBE ?
                    detector->fast_window_probe_points : 0U);
    if (plus > 2U)
    {
        plus = 2U;
    }

    if (plus != 0U)
    {
        uint32_t next = (uint32_t)detector->load_score + plus;
        detector->load_score = (uint8_t)(next >= LINKG_SWITCH_QUALITY_BAD_SCORE ?
                                          LINKG_SWITCH_QUALITY_BAD_SCORE : next);
    }
    else if (probe->active_mask != 0U &&
             (detector->fast_window_ack_mask & probe->active_mask) == probe->active_mask &&
             detector->load_score > 0U)
    {
        detector->load_score--;
    }

    detector->fast_window_started_us   = now_us;
    detector->fast_window_evidence     = 0U;
    detector->fast_window_ack_mask     = 0U;
    detector->fast_window_probe_points = 0U;

    if (detector->load_score < LINKG_SWITCH_QUALITY_BAD_SCORE)
    {
        return false;
    }

    // LOAD事件已产生，下次诊断须重新从首次TIMEOUT开始。
    detector->load_score = 0U;
    memset(detector->fast_probe_timeout_streak, 0, sizeof(detector->fast_probe_timeout_streak));
    memset(detector->fast_probe_last_timeout_us, 0, sizeof(detector->fast_probe_last_timeout_us));
    return true;
}

int linkg_switch_policy_quality_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_quality_state_t *quality_state)
{
    linkg_switch_quality_policy_runtime_t *detector;
    linkg_switch_peer_runtime_t           *peer;
    linkg_switch_quality_wire_route_t      wire_route;
    linkg_switch_quality_fast_result_t     fast;
    bool                                   hcc_off;
    bool                                   load_bad;
    bool                                   overflow;

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
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    // 异步Probe API的互斥锁不可在Switch锁内获取。放锁后poll/submit，
    // 再检查Peer身份和Plan连续性，防止会话变化后的结果污染新检测器。
    pthread_mutex_unlock(&g_switch.lock);
    _linkg_switch_quality_fast_process(input, now_us, &fast);
    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL || !g_switch.running ||
        !_linkg_switch_quality_plan_equal(&peer->plan, &input->plan) ||
        peer->role.sta.wifi_quality_policy.wifi_link_id != input->observation.wifi.link_id)
    {
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return -ESTALE;
    }
    detector = &peer->role.sta.wifi_quality_policy;

    /****************************** 五、队列溢出检测 ******************************/

    // 始终消费新OVF窗口并推进时间戳，但空业务或活跃集合变更首轮不得触发LOAD_BAD。
    // 活跃集合变化后的第一份观测可能仍包含上一业务的溢出，必须先完成隔离。
    overflow = _linkg_switch_quality_overflow_locked(input, detector);
    if (overflow && fast.active_mask != 0U &&
        detector->fast_window_active_mask == fast.active_mask)
    {
        // 队列溢出已证实负载损失，丢弃旧Wire质量积分及未完成批次。
        _linkg_switch_quality_reset_scores_locked(detector);

        // 当前窗口已归属LOAD：更新Wire/HCC基线，不把本轮重复用于后续积分。
        _linkg_switch_quality_consume_current_locked(input, detector);

        *quality_state = LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD;

        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    /****************************** 六、HCC流控压力检测 ******************************/

    hcc_off  = false;
    wire_route = _linkg_switch_quality_hcc_route_locked(input, now_us, detector, &hcc_off);

    /****************************** 七、Wire样本归属 ******************************/

    // LOAD/LOSS/SKIP切换时隔离低样本批次，历史有效积分保持独立。
    _linkg_switch_quality_select_wire_route_locked(detector, wire_route);

    /****************************** 八、Wire质量判决 ******************************/

    if (wire_route != LINKG_SWITCH_QUALITY_WIRE_LOSS)
    {
        // 无新增OFF且HCC保护仍有效：只消费Wire，不参与任一路径的积分。
        _linkg_switch_quality_consume_wire_current_locked(input, detector);
    }
    else
    {
        // 本轮最多处理一次上下行Wire，并仅更新所选路径的积分。
        *quality_state = _linkg_switch_quality_evaluate_wire_locked(input, now_us, detector, wire_route);
    }

    /****************************** 九、250ms快速负载评分 ******************************/

    load_bad = _linkg_switch_quality_fast_score_locked(detector, input, &fast, hcc_off, now_us);
    if (load_bad)
    {
        // 同轮若LOSS也达到门槛，优先LOAD：已确认的负载证据需要先迁移救场。
        *quality_state = LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD;
    }

    /****************************** 十、异常结果输出 ******************************/

    // OVF具有最高优先级，快速LOAD覆盖同轮LOSS；始终只输出一个状态。

    pthread_mutex_unlock(&g_switch.lock);

    if (*quality_state != LINKG_SWITCH_POLICY_QUALITY_NONE)
    {
        // 在提交PlanSync前释放Probe槽位，避免ACK后Verify抢占冲突。
        _linkg_switch_quality_fast_reset();
    }

    return 0;
}
