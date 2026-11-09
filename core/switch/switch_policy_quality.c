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
#define LINKG_SWITCH_QUALITY_FAST_PERIOD_US        100000ULL  // Probe快速检查周期100ms，外部Worker负责调度
#define LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS       200U       // 每包实际发送后最多等待200ms
#define LINKG_SWITCH_QUALITY_FAST_ACTIVE_AGE_MS    1500ULL    // 只探测最近有真实USER_DATA的业务
#define LINKG_SWITCH_QUALITY_FAST_MAX_OBS_AGE_US   500000ULL  // 快速判决所需Observation最大年龄
#define LINKG_SWITCH_QUALITY_FAST_MAX_PENDING_US   500000ULL  // 超时未发送/未完成的本地请求取消保护
#define LINKG_SWITCH_QUALITY_FAST_MASK_AGE_US      300000ULL  // OVF复用快速业务掩码的有效期
#define LINKG_SWITCH_QUALITY_OVERFLOW_MAX_AGE_US   500000ULL  // OVF源窗口允许延迟消费的最大年龄
#define LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC     0x01U      // 250ms路径产生的新HCC OFF，供100ms路径消费

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
    linkg_send_plan_t plan;                                         // 已绑定的完整发送计划
    uint64_t          handle;                                       // 当前唯一在途异步Probe
    uint64_t          submit_us;                                    // 当前Probe提交时间，仅用于尚未发送时的本地排队保护
    uint64_t          active_checked_us;                            // 最近一次成功获取真实活跃业务统计的时间
    uint8_t           peer_node_id;                                 // 当前Peer身份
    uint32_t          peer_generation;                              // 当前Peer运行代际
    uint32_t          wifi_link_id;                                 // 当前Wi-Fi链路实例
    uint32_t          selected_class;                               // 当前正在探测的业务类别，COUNT为无业务
    uint32_t          active_mask;                                  // 最近一次成功读取的真实活跃业务集合
    bool              warned;                                       // 当前Probe的100ms检查点已计入正式LOAD积分
    bool              initialized;                                  // 当前运行实例是否已经绑定
} linkg_switch_quality_fast_probe_runtime_t;

typedef struct
{
    uint32_t active_mask;                                           // 当前真实活跃且Wi-Fi单发的类别集合
    uint32_t selected_class;                                        // 本次选中的业务类别
    bool     valid;                                                 // 已成功读取Transport业务统计
    bool     warning;                                               // 已确认发送的Probe超过100ms仍PENDING，正式加1分
    bool     timeout_100ms_counted;                                // 本Probe的100ms积分此前已计入，防止终态重复计分
    bool     long_timeout;                                          // 当前Probe已确认真实200ms TIMEOUT
    bool     ack_good;                                              // RTT严格小于100ms的正常ACK
    bool     ack_slow;                                              // 已收到ACK，但RTT达到或超过100ms
    bool     broken;                                                // 发送失败/取消/API异常，不作为网络超时
} linkg_switch_quality_fast_result_t;

// 当前平台STA仅有一个直连AP；此状态必须只由同一个Switch Worker串行访问。
static linkg_switch_quality_fast_probe_runtime_t g_quality_fast_probe;

/**
 * @brief 取消快速探测，释放唯一在途Probe槽位。
 * 不允许持有g_switch.lock调用，避免Probe与Switch锁反向嵌套。
 */
static void _linkg_switch_quality_fast_reset(void)
{
    if (g_quality_fast_probe.handle != 0U)
    {
        (void)linkg_path_probe_async_cancel(g_quality_fast_probe.handle);
    }

    memset(&g_quality_fast_probe, 0, sizeof(g_quality_fast_probe));
    g_quality_fast_probe.selected_class = LINKG_TRANSPORT_CLASS_COUNT;
}

/**
 * @brief 检查是否具备Wi-Fi单发及Cellular备用的快速探测条件。
 */
static bool _linkg_switch_quality_fast_eligible(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    uint32_t index;

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id ||
        input->observation.collected_us == 0U ||
        input->observation.collected_us > now_us ||
        now_us - input->observation.collected_us > LINKG_SWITCH_QUALITY_FAST_MAX_OBS_AGE_US ||
        input->wifi_blocked || input->cellular_blocked ||
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
 * @brief 探测一次真实活跃业务；优先DATA，再VIDEO，最后REALTIME。
 * 每Peer仅有一个在途Probe；Poll/Cancel/Submit均在Switch锁外执行。
 * 仅对Scheduler已接受发送的PENDING按sent_us计时；100ms和200ms分别加1分。
 */
static void _linkg_switch_quality_fast_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_quality_fast_result_t *output)
{
    linkg_transport_peer_stats_t          stats;
    linkg_path_probe_async_result_t       result;
    const linkg_class_send_plan_t         *item;
    const linkg_transport_class_stats_t   *class_stats;
    uint64_t                              now_ms;
    uint64_t                              handle;
    uint32_t                              index;
    uint32_t                              bit;
    uint32_t                              selected;
    bool                                  active;
    int                                   ret;

    memset(output, 0, sizeof(*output));
    output->selected_class = LINKG_TRANSPORT_CLASS_COUNT;

    if (!_linkg_switch_quality_fast_eligible(input, now_us))
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
        g_quality_fast_probe.initialized      = true;
        g_quality_fast_probe.peer_node_id     = input->peer_node_id;
        g_quality_fast_probe.peer_generation  = input->peer_generation;
        g_quality_fast_probe.wifi_link_id     = input->observation.wifi.link_id;
        g_quality_fast_probe.plan             = input->plan;
    }

    ret = linkg_transport_get_peer_stats(input->peer_node_id, &stats);
    if (ret != 0)
    {
        // 统计获取失败不等于业务不活跃；禁止复用旧掩码参与OVF和LOAD评分。
        g_quality_fast_probe.active_checked_us = 0U;
        return;
    }

    now_ms  = linkg_time_elapsed_ms();
    selected = LINKG_TRANSPORT_CLASS_COUNT;

    for (index = LINKG_TRANSPORT_CLASS_COUNT; index > 0U; index--)
    {
        uint32_t class_index = index - 1U;

        item        = &input->plan.classes[class_index];
        class_stats = &stats.classes[class_index];
        bit         = 1U << class_index;

        active = (class_stats->last_user_tx_ms != 0U &&
                  class_stats->last_user_tx_ms <= now_ms &&
                  now_ms - class_stats->last_user_tx_ms <= LINKG_SWITCH_QUALITY_FAST_ACTIVE_AGE_MS) ||
                 (class_stats->last_user_rx_ms != 0U &&
                  class_stats->last_user_rx_ms <= now_ms &&
                  now_ms - class_stats->last_user_rx_ms <= LINKG_SWITCH_QUALITY_FAST_ACTIVE_AGE_MS);

        if (!active || item->mode != LINKG_SEND_MODE_SINGLE ||
            item->primary_link_id != input->observation.wifi.link_id ||
            item->secondary_link_id != input->observation.cellular.link_id)
        {
            continue;
        }

        output->active_mask |= bit;
        if (selected == LINKG_TRANSPORT_CLASS_COUNT)
        {
            selected = class_index;
        }
    }

    output->selected_class = selected;
    output->valid          = true;
    g_quality_fast_probe.active_mask       = output->active_mask;
    g_quality_fast_probe.active_checked_us = now_us;

    if (selected != g_quality_fast_probe.selected_class)
    {
        if (g_quality_fast_probe.handle != 0U)
        {
            (void)linkg_path_probe_async_cancel(g_quality_fast_probe.handle);
        }

        g_quality_fast_probe.handle         = 0U;
        g_quality_fast_probe.submit_us      = 0U;
        g_quality_fast_probe.warned         = false;
        g_quality_fast_probe.selected_class = selected;
    }

    if (selected == LINKG_TRANSPORT_CLASS_COUNT)
    {
        return;
    }

    if (g_quality_fast_probe.handle != 0U)
    {
        memset(&result, 0, sizeof(result));
        ret = linkg_path_probe_async_poll(g_quality_fast_probe.handle, &result);

        if (ret == 0 && result.state != LINKG_PATH_PROBE_ASYNC_PENDING)
        {
            // 保存本包100ms检查点是否已加分；终态最多补齐本包剩余积分。
            output->timeout_100ms_counted = g_quality_fast_probe.warned;
            g_quality_fast_probe.handle = 0U;

            if (result.traffic_class != (linkg_transport_class_t)selected ||
                result.sent_us == 0U || result.sent_us > now_us ||
                result.completed_us < result.sent_us)
            {
                output->broken = true;
            }
            else if (result.state == LINKG_PATH_PROBE_ASYNC_TIMEOUT &&
                     result.completed_us - result.sent_us >=
                     (uint64_t)LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS * 1000ULL)
            {
                // 只有实际发送后形成的真实200ms终态才能确认长超时。
                output->long_timeout = true;
            }
            else if (result.state == LINKG_PATH_PROBE_ASYNC_ACK &&
                     result.rtt_us <= result.completed_us - result.sent_us &&
                     result.rtt_us < (uint32_t)LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS * 1000U)
            {
                if (result.rtt_us < (uint32_t)(LINKG_SWITCH_QUALITY_FAST_PERIOD_US))
                {
                    // 即使在后续Tick才Poll到ACK，也按实际RTT判定正常。
                    output->ack_good = true;
                }
                else
                {
                    // RTT在100~200ms之间：100ms未响应成立，但不构成200ms超时。
                    output->ack_slow = true;
                }
            }
            else
            {
                output->broken = true;
            }

            g_quality_fast_probe.warned    = false;
            g_quality_fast_probe.submit_us = 0U;
        }
        else if (ret != 0)
        {
            output->timeout_100ms_counted = g_quality_fast_probe.warned;
            (void)linkg_path_probe_async_cancel(g_quality_fast_probe.handle);
            g_quality_fast_probe.handle    = 0U;
            output->broken                  = true;
            g_quality_fast_probe.warned     = false;
            g_quality_fast_probe.submit_us  = 0U;
        }
        else if (g_quality_fast_probe.submit_us != 0U && now_us >= g_quality_fast_probe.submit_us)
        {
            uint64_t elapsed_us = now_us - g_quality_fast_probe.submit_us;

            // Scheduler已接纳发送后，回收超时也必须以sent_us为起点。
            // 避免本地排队较久的Probe刚发出100ms就被提交时间保护提前取消。
            if (result.sent_us != 0U && result.sent_us <= now_us)
            {
                elapsed_us = now_us - result.sent_us;
            }

            if (elapsed_us >= LINKG_SWITCH_QUALITY_FAST_MAX_PENDING_US)
            {
                // 本地请求长期无终态时释放槽位，并撤销该请求之前已加的100ms积分。
                output->timeout_100ms_counted = g_quality_fast_probe.warned;
                (void)linkg_path_probe_async_cancel(g_quality_fast_probe.handle);
                g_quality_fast_probe.handle    = 0U;
                g_quality_fast_probe.submit_us = 0U;
                g_quality_fast_probe.warned    = false;
                output->broken                 = true;
            }
            else if (!g_quality_fast_probe.warned && result.sent_us != 0U &&
                     result.sent_us <= now_us &&
                     now_us - result.sent_us >= LINKG_SWITCH_QUALITY_FAST_PERIOD_US)
            {
                // 仅在Scheduler确认提交后计分，禁止将本地排队时间当作网络超时。
                output->warning           = true;
                g_quality_fast_probe.warned = true;
            }
        }
    }

    if (g_quality_fast_probe.handle == 0U)
    {
        handle = 0U;
        ret = linkg_path_probe_async_submit(input->peer_node_id,
                                            input->observation.wifi.link_id,
                                            (linkg_transport_class_t)selected,
                                            LINKG_SWITCH_QUALITY_FAST_TIMEOUT_MS,
                                            &handle);
        if (ret == 0)
        {
            g_quality_fast_probe.handle    = handle;
            g_quality_fast_probe.submit_us = now_us;
            g_quality_fast_probe.warned    = false;
        }
        else
        {
            // -EBUSY及本地提交失败不能作为无线超时。
            output->broken = true;
        }
    }
}

/**
 * @brief Wire空闲时的LOSS独立重置与生命周期完整重置。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_reset_loss_locked(linkg_switch_quality_policy_runtime_t *detector)
{
    detector->loss_score          = 0U;
    detector->last_wire_sample_us = 0U;
    detector->last_loss_valid_us  = 0U;
    detector->wire_route          = LINKG_SWITCH_QUALITY_WIRE_SKIP;
    memset(&detector->uplink_accumulator, 0, sizeof(detector->uplink_accumulator));
    memset(&detector->downlink_accumulator, 0, sizeof(detector->downlink_accumulator));
}

/**
 * @brief 清除快速LOAD正式积分、当前Probe预警和连续超时状态。
 * 调用方必须持有Switch锁。
 */
static void _linkg_switch_quality_reset_load_locked(linkg_switch_quality_policy_runtime_t *detector)
{
    detector->load_score                 = 0U;
    detector->last_load_valid_us         = 0U;
    detector->fast_window_started_us     = 0U;
    detector->fast_window_evidence       = 0U;
    detector->fast_window_ack_mask       = 0U;
    detector->fast_window_probe_points   = 0U;
    detector->fast_window_active_mask    = 0U;
    detector->fast_probe_warning_active  = false;
    memset(detector->fast_probe_timeout_streak, 0, sizeof(detector->fast_probe_timeout_streak));
    memset(detector->fast_probe_last_timeout_us, 0, sizeof(detector->fast_probe_last_timeout_us));
}

/**
 * @brief 生命周期变化或真实OVF后完整重置LOAD和LOSS。
 * 调用方必须持有g_switch.lock。
 */
static void _linkg_switch_quality_reset_scores_locked(linkg_switch_quality_policy_runtime_t *detector)
{
    _linkg_switch_quality_reset_loss_locked(detector);
    _linkg_switch_quality_reset_load_locked(detector);
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
        // 仅Wire空闲，不能清理仍在接受Probe/HCC证据的快速LOAD积分。
        _linkg_switch_quality_reset_loss_locked(detector);
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
static bool _linkg_switch_quality_overflow_direction_locked(const linkg_switch_loss_observation_t *sample, uint64_t observation_us, uint64_t now_us, uint64_t *last_updated_us)
{
    if (sample->updated_us == 0U ||
        sample->updated_us > observation_us ||
        sample->updated_us > now_us ||
        sample->updated_us <= *last_updated_us)
    {
        return false;
    }

    // 先消费时间戳，避免无效窗口被重复检查。
    *last_updated_us = sample->updated_us;

    if (now_us - sample->updated_us > LINKG_SWITCH_QUALITY_OVERFLOW_MAX_AGE_US ||
        !sample->valid ||
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
static bool _linkg_switch_quality_overflow_locked(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_quality_policy_runtime_t *detector)
{
    const linkg_switch_path_loss_observation_t *loss;
    uint64_t                                    observation_us;
    bool                                        ul_overflow;
    bool                                        dl_overflow;

    loss           = &input->observation.wifi.loss;
    observation_us = input->observation.collected_us;

    ul_overflow = _linkg_switch_quality_overflow_direction_locked(&loss->uplink_load, observation_us, now_us, &detector->last_overflow_uplink_updated_us);
    dl_overflow = _linkg_switch_quality_overflow_direction_locked(&loss->downlink_load, observation_us, now_us, &detector->last_overflow_downlink_updated_us);

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

        // LOAD减分由100ms快速入口在Probe得到正常ACK后执行。
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
 * 这里只累计LOSS积分；LOAD由独立100ms Probe/HCC证据窗口评分。
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

/****************************** LOAD快速积分判决 ******************************/

/**
 * @brief 检查快照中是否仍有新鲜HCC流控关闭状态。
 * 仅用于避免在当前持续禁止发送时把正常ACK误认为负载已恢复。
 */
static bool _linkg_switch_quality_fast_hcc_blocked(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    const linkg_switch_wifi_hcc_observation_t *hcc;
    uint32_t                                   index;

    for (index = 0U; index < 2U; index++)
    {
        hcc = index == 0U ? &input->observation.wifi.uplink_hcc :
                            &input->observation.wifi.downlink_hcc;

        if (hcc->valid && hcc->updated_us != 0U &&
            hcc->updated_us <= input->observation.collected_us &&
            hcc->updated_us <= now_us &&
            now_us - hcc->updated_us <= LINKG_SWITCH_QUALITY_HCC_MAX_AGE_US &&
            !hcc->tx_allowed)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 合并HCC OFF与Probe积分，正式积分唯一归Peer Quality Runtime所有。
 *
 * 已发送且100ms仍PENDING：正式加1分；真实200ms TIMEOUT：再加1分。
 * 若100ms Tick未消费到PENDING，则终态一次补齐2分，保证每包只计一次。
 * 调用方必须持有g_switch.lock。
 */
static bool _linkg_switch_quality_fast_score_locked(linkg_switch_quality_policy_runtime_t *detector, const linkg_switch_policy_input_t *input, const linkg_switch_quality_fast_result_t *probe, uint64_t now_us)
{
    uint32_t next_score;
    uint32_t delta;
    bool     new_hcc_off;
    bool     hcc_blocked;

    if (!probe->valid)
    {
        return false;
    }

    if (detector->fast_window_active_mask != probe->active_mask)
    {
        // 活跃集合变化时，旧业务的正式积分、预警和HCC证据全部隔离。
        _linkg_switch_quality_reset_load_locked(detector);
        detector->fast_window_active_mask = probe->active_mask;
        detector->fast_window_started_us  = now_us;
        return false;
    }

    if (probe->active_mask == 0U || probe->selected_class >= LINKG_TRANSPORT_CLASS_COUNT)
    {
        _linkg_switch_quality_reset_load_locked(detector);
        detector->fast_window_started_us = now_us;
        return false;
    }

    new_hcc_off = (detector->fast_window_evidence & LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC) != 0U;
    hcc_blocked = _linkg_switch_quality_fast_hcc_blocked(input, now_us);
    detector->fast_window_evidence &= (uint8_t)~LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC;

    // HCC新增OFF是独立正式证据，每份250ms Observation最多消费一次。
    delta = new_hcc_off ? 1U : 0U;

    if (probe->warning)
    {
        // 当前请求实际已提交发送，满100ms仍PENDING时立即正式加1分。
        delta += 1U;
        detector->fast_probe_warning_active = true;
    }

    if (probe->long_timeout)
    {
        // 若本包100ms检查点未被Worker消费，先补上那一个检查点。
        if (!probe->timeout_100ms_counted)
        {
            delta++;
        }

        // 200ms真实TIMEOUT只增加本检查点的1分，绝不叠加连续超时奖励。
        delta++;
        detector->fast_probe_warning_active = false;
    }
    else if (probe->ack_slow)
    {
        // 100～200ms ACK只保留首分，不能在终态再次加同一分。
        if (!probe->timeout_100ms_counted)
        {
            delta++;
        }
        detector->fast_probe_warning_active = false;
    }
    else if (probe->ack_good || probe->broken)
    {
        // 迟到的本地错误或正常快速ACK，需要撤销之前误记在本包上的100ms分。
        if (probe->timeout_100ms_counted && detector->fast_probe_warning_active &&
            detector->load_score > 0U)
        {
            detector->load_score--;
        }

        detector->fast_probe_warning_active = false;
    }

    if (delta != 0U)
    {
        next_score = (uint32_t)detector->load_score + delta;
        detector->load_score = (uint8_t)(next_score >= LINKG_SWITCH_QUALITY_BAD_SCORE ?
                                         LINKG_SWITCH_QUALITY_BAD_SCORE : next_score);
        detector->last_load_valid_us = now_us;
    }
    else if (probe->ack_good && !new_hcc_off && !hcc_blocked && detector->load_score > 0U)
    {
        // 只有实际RTT<100ms、无新OFF且当前没有HCC阻塞时才恢复1分。
        detector->load_score--;
        detector->last_load_valid_us = now_us;
    }

    detector->fast_window_started_us = now_us;

    if (detector->load_score < LINKG_SWITCH_QUALITY_BAD_SCORE)
    {
        return false;
    }

    _linkg_switch_quality_reset_load_locked(detector);
    return true;
}

/****************************** 100ms快速LOAD入口 ******************************/

/**
 * @brief 独立消费异步Probe，更新共享LOAD积分，达到5分立即输出LOAD_BAD。
 * 本接口仅供同一个Switch Worker每100ms调用，禁止并发执行。
 * 不触碰LOSS积分、Wire累计或Operation事务。
 */
int linkg_switch_policy_quality_fast_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_quality_state_t *quality_state)
{
    linkg_switch_quality_policy_runtime_t *detector;
    linkg_switch_peer_runtime_t           *peer;
    const linkg_switch_plan_sync_runtime_t *sync;
    linkg_switch_quality_fast_result_t     fast;
    bool                                   load_bad;

    if (input == NULL || quality_state == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *quality_state = LINKG_SWITCH_POLICY_QUALITY_NONE;

    pthread_mutex_lock(&g_switch.lock);

    if (!g_switch.initialized || !g_switch.running || g_switch.role != LINKG_DEVICE_ROLE_STA)
    {
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    detector = &peer->role.sta.wifi_quality_policy;
    if (!_linkg_switch_quality_plan_equal(&peer->plan, &input->plan) ||
        !_linkg_switch_quality_fast_eligible(input, now_us))
    {
        detector->fast_probe_warning_active = false;
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    sync = &peer->role.sta.plan_sync;
    if (sync->state == LINKG_SWITCH_PLAN_SYNC_WAIT_ACK ||
        sync->state == LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT ||
        sync->state == LINKG_SWITCH_PLAN_SYNC_REJECTED ||
        sync->state == LINKG_SWITCH_PLAN_SYNC_UNCERTAIN)
    {
        _linkg_switch_quality_reset_load_locked(detector);
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    if (!detector->initialized ||
        detector->wifi_link_id != input->observation.wifi.link_id ||
        !_linkg_switch_quality_plan_equal(&detector->plan, &input->plan))
    {
        // 初始化旧观测基线，防止将Plan/Link变化前的HCC、Wire、OVF当成新证据。
        _linkg_switch_quality_initialize_locked(detector, input);
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    if (detector->waiting_sync)
    {
        detector->fast_probe_warning_active = false;
        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

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
    load_bad = _linkg_switch_quality_fast_score_locked(detector, input, &fast, now_us);
    if (load_bad)
    {
        *quality_state = LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD;
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (load_bad)
    {
        // 进入PlanSync前释放Probe槽位，供后续Verify复用。
        _linkg_switch_quality_fast_reset();
    }

    return 0;
}

/****************************** 250ms完整质量入口 ******************************/

/**
 * @brief 保留原有OVF/HCC/Wire/LOSS；HCC新增OFF只存入共享LOAD证据。
 * LOAD加减分统一由100ms快速入口结算，避免双重计分。
 */
int linkg_switch_policy_quality_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_quality_state_t *quality_state)
{
    linkg_switch_quality_policy_runtime_t *detector;
    linkg_switch_peer_runtime_t           *peer;
    linkg_switch_quality_wire_route_t      wire_route;
    bool                                   hcc_off;
    bool                                   overflow;
    bool                                   active_fresh;

    /****************************** 一、参数校验 ******************************/

    if (input == NULL || quality_state == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *quality_state = LINKG_SWITCH_POLICY_QUALITY_NONE;
    pthread_mutex_lock(&g_switch.lock);

    /****************************** 二、Switch与Peer有效性 ******************************/

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

    /****************************** 三、检测器生命周期 ******************************/

    detector = &peer->role.sta.wifi_quality_policy;
    if (!_linkg_switch_quality_prepare_locked(input, now_us, peer, detector))
    {
        pthread_mutex_unlock(&g_switch.lock);
        // waiting_sync只隔离250ms的旧样本；不能重置独立100ms快速Probe。
        return 0;
    }

    /****************************** 四、LinkG队列OVF直接兜底 ******************************/

    active_fresh = g_quality_fast_probe.initialized &&
                   g_quality_fast_probe.peer_node_id == input->peer_node_id &&
                   g_quality_fast_probe.peer_generation == input->peer_generation &&
                   g_quality_fast_probe.wifi_link_id == input->observation.wifi.link_id &&
                   _linkg_switch_quality_plan_equal(&g_quality_fast_probe.plan, &input->plan) &&
                   g_quality_fast_probe.active_checked_us != 0U &&
                   now_us >= g_quality_fast_probe.active_checked_us &&
                   now_us - g_quality_fast_probe.active_checked_us <= LINKG_SWITCH_QUALITY_FAST_MASK_AGE_US;

    overflow = false;
    if (active_fresh &&
        g_quality_fast_probe.active_mask != 0U &&
        detector->fast_window_active_mask == g_quality_fast_probe.active_mask)
    {
        // 只有业务身份与活跃掩码新鲜时才消费OVF，避免先去重后因窗口错配丢失证据。
        overflow = _linkg_switch_quality_overflow_locked(input, now_us, detector);
    }

    if (overflow)
    {
        // 已确认的队列溢出直接LOAD_BAD，不等待5分。
        _linkg_switch_quality_reset_scores_locked(detector);
        _linkg_switch_quality_consume_current_locked(input, detector);
        *quality_state = LINKG_SWITCH_POLICY_QUALITY_LOAD_BAD;

        pthread_mutex_unlock(&g_switch.lock);
        _linkg_switch_quality_fast_reset();
        return 0;
    }

    /****************************** 五、HCC压力及Wire归属 ******************************/

    hcc_off   = false;
    wire_route = _linkg_switch_quality_hcc_route_locked(input, now_us, detector, &hcc_off);
    _linkg_switch_quality_select_wire_route_locked(detector, wire_route);

    if (wire_route != LINKG_SWITCH_QUALITY_WIRE_LOSS)
    {
        _linkg_switch_quality_consume_wire_current_locked(input, detector);
    }
    else
    {
        *quality_state = _linkg_switch_quality_evaluate_wire_locked(input, now_us, detector, wire_route);
    }

    // HCC原有压力确认和LOSS保护逻辑保持不变，只传递一次新的OFF给100ms LOAD积分。
    if (hcc_off &&
        active_fresh &&
        g_quality_fast_probe.active_mask != 0U &&
        detector->fast_window_active_mask == g_quality_fast_probe.active_mask)
    {
        detector->fast_window_evidence |= LINKG_SWITCH_QUALITY_FAST_EVIDENCE_HCC;
    }

    if (*quality_state == LINKG_SWITCH_POLICY_QUALITY_LOSS_BAD)
    {
        // LOSS事件将进入独立验证事务，旧LOAD正式积分和预警不得跨事务保留。
        _linkg_switch_quality_reset_load_locked(detector);
    }

    pthread_mutex_unlock(&g_switch.lock);

    if (*quality_state != LINKG_SWITCH_POLICY_QUALITY_NONE)
    {
        _linkg_switch_quality_fast_reset();
    }

    return 0;
}


