/**
 * @file switch_policy_wifi.c
 * @brief LinkG Wi-Fi主链路质量检测与策略事件生成
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-22
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_link.h"

#include "switch_internal.h"
#include "switch_policy_event.h"
#include "switch_policy_operation.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断是否存在可迁移的Wi-Fi单发业务及可用Cellular备份。
 */
static bool _linkg_switch_policy_wifi_has_candidate(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       index;

    // Cellular维护或不可用时，禁止软件质量迁移。
    if (input->cellular_blocked ||
        input->observation.cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    // 至少存在一类Wi-Fi主发、Cellular备用的单发业务。
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current = &input->plan.classes[index];

        if (current->mode == LINKG_SEND_MODE_SINGLE &&
            current->primary_link_id == input->observation.wifi.link_id &&
            current->secondary_link_id == input->observation.cellular.link_id)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 判断Wi-Fi双向业务总速率是否在Cellular配置容量范围内。
 *
 * 流量样本或配置不可用时，禁止LOSS/LOAD软件质量迁移。
 * 硬故障切换不受此限制。
 */
static bool _linkg_switch_policy_wifi_within_cellular_capacity(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_traffic_observation_t *traffic;
    linkg_cellular_config_t                   config;
    uint64_t                                  limit_bps;

    traffic = &input->observation.wifi.traffic;

    // 必须具备有效流量样本及Cellular容量配置。
    if (!traffic->valid || traffic->updated_us == 0U || linkg_config_get_cellular(&config) != 0)
    {
        return false;
    }

    limit_bps = (uint64_t)config.max_load_mbps * 1000000ULL;

    // 先检查发送速率，避免后续无符号减法下溢。
    if (traffic->tx_bps > limit_bps)
    {
        return false;
    }

    // 上下行总速率不得超过Cellular配置容量。
    return traffic->rx_bps <= limit_bps - traffic->tx_bps;
}

/**
 * @brief 将Wi-Fi策略初始事件同步交付给Switch Operation状态机。
 */
static int _linkg_switch_policy_wifi_raise(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_event_type_t type)
{
    linkg_switch_policy_event_t event;

    memset(&event, 0, sizeof(event));

    event.type            = type;
    event.peer_node_id    = input->peer_node_id;
    event.peer_generation = input->peer_generation;
    event.occurred_us     = now_us;

    return linkg_switch_policy_operation_process_event(&event, now_us);
}

/****************************** Probe逻辑断链检测 ******************************/

#define LINKG_SWITCH_POLICY_PROBE_TIMEOUT_COUNT       3U          // 三类业务各连续超时3次
#define LINKG_SWITCH_POLICY_PROBE_LOW_LOAD_BPS        2500000ULL  // Wi-Fi双向总速率必须严格低于2.5Mbps
#define LINKG_SWITCH_POLICY_PROBE_LOW_LOAD_HOLD_US    2000000ULL  // 低负载至少维持2秒
#define LINKG_SWITCH_POLICY_PROBE_MAX_RESULT_AGE_US   2500000ULL  // Probe源结果最大年龄
#define LINKG_SWITCH_POLICY_PROBE_MAX_RESULT_GAP_US   2000000ULL  // 相邻有效Probe结果最大间隔

/**
 * @brief 清空当前Probe检测窗口，但保留未恢复的逻辑黑洞标记。
 *
 * 调用方持有g_switch.lock。
 */
static void _linkg_switch_policy_wifi_probe_reset_locked(linkg_switch_policy_probe_runtime_t *runtime)
{
    bool blackhole_latched;

    blackhole_latched = runtime->blackhole_latched;
    memset(runtime, 0, sizeof(*runtime));
    runtime->blackhole_latched = blackhole_latched;
}

/**
 * @brief 三类Wi-Fi Probe是否均已获得新鲜的成功响应。
 *
 * Observation已执行2.5秒有效期过滤；这里再校验时间，避免直接使用旧快照。
 */
static bool _linkg_switch_policy_wifi_probe_all_reachable(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_probe_observation_t *probe;
    uint64_t                               observed_us;
    uint32_t                               index;

    observed_us = input->observation.collected_us;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        probe = &input->observation.wifi.probe[index];

        if (!probe->valid || !probe->reachable || probe->updated_us == 0U ||
            probe->updated_us > observed_us ||
            observed_us - probe->updated_us > LINKG_SWITCH_POLICY_PROBE_MAX_RESULT_AGE_US)
        {
            return false;
        }
    }

    return true;
}


/**
 * @brief 检测低负载下Wi-Fi三类Probe连续超时故障。
 *
 * 三类Probe分别连续超时3次，且Wi-Fi双向总速率持续低于2.5Mbps，
 * 才认为Wi-Fi存在逻辑断链。
 *
 * 每个Probe结果只消费一次，成功响应清除对应类别的超时积分。
 * 检测状态由g_switch.lock保护，本函数不直接修改发送计划。
 */
static int _linkg_switch_policy_wifi_probe_process(const linkg_switch_policy_input_t *input,
                                                    bool *probe_down)
{
    const linkg_switch_traffic_observation_t *traffic;
    const linkg_switch_probe_observation_t   *probe;
    linkg_switch_policy_probe_runtime_t      *runtime;
    linkg_switch_peer_runtime_t              *peer;
    uint64_t                                  observed_us;
    uint64_t                                  last_updated_us;
    uint32_t                                  index;
    bool                                      eligible;
    bool                                      all_failed;

    /****************************** 输入检查 ******************************/

    if (input == NULL || probe_down == NULL)
    {
        return -EINVAL;
    }

    *probe_down = false;

    observed_us = input->observation.collected_us;
    traffic     = &input->observation.wifi.traffic;

    /****************************** 检查链路资格 ******************************/

    eligible = input->observation.wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
               input->observation.wifi.link_id != LINKG_LINK_ID_INVALID &&
               !input->wifi_blocked &&
               _linkg_switch_policy_wifi_has_candidate(input);

    /****************************** 获取Peer检测状态 ******************************/

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    runtime = &peer->role.sta.policy.wifi_probe;

    /****************************** 更新黑洞恢复状态 ******************************/

    // Wi-Fi重新主发且三类Probe均可达时，解除历史逻辑断链标记。
    if (runtime->blackhole_latched && eligible &&
        _linkg_switch_policy_wifi_probe_all_reachable(input))
    {
        runtime->blackhole_latched = false;
    }

    /****************************** 检查低负载条件 ******************************/

    // 链路不具备检测资格或流量样本无效时，清除本轮检测窗口。
    if (!eligible ||
        observed_us == 0U ||
        !traffic->valid ||
        traffic->updated_us == 0U ||
        traffic->updated_us > observed_us)
    {
        _linkg_switch_policy_wifi_probe_reset_locked(runtime);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    // 双向总速率必须严格低于2.5Mbps。
    // 先检查TX，避免后续无符号减法下溢。
    if (traffic->tx_bps >= LINKG_SWITCH_POLICY_PROBE_LOW_LOAD_BPS ||
        traffic->rx_bps >= LINKG_SWITCH_POLICY_PROBE_LOW_LOAD_BPS - traffic->tx_bps)
    {
        _linkg_switch_policy_wifi_probe_reset_locked(runtime);
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 初始化检测窗口 ******************************/

    // Wi-Fi Link变化、首次检测或时间回退时重新建立检测窗口。
    if (!runtime->initialized ||
        runtime->wifi_link_id != input->observation.wifi.link_id ||
        observed_us < runtime->low_load_since_us)
    {
        _linkg_switch_policy_wifi_probe_reset_locked(runtime);

        runtime->initialized  = true;
        runtime->wifi_link_id = input->observation.wifi.link_id;
    }

    // 首次确认低负载时记录起始时间。
    if (runtime->low_load_since_us == 0U)
    {
        runtime->low_load_since_us = observed_us;
    }

    /****************************** 检查三类Probe ******************************/

    all_failed = true;

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        probe           = &input->observation.wifi.probe[index];
        last_updated_us = runtime->last_probe_updated_us[index];

        /****************************** 检查样本有效性 ******************************/

        // 无效、未来或过期结果不计入连续超时。
        if (!probe->valid ||
            probe->updated_us == 0U ||
            probe->updated_us > observed_us ||
            observed_us - probe->updated_us >
                LINKG_SWITCH_POLICY_PROBE_MAX_RESULT_AGE_US)
        {
            runtime->timeout_count[index] = 0U;
            all_failed                    = false;
            continue;
        }

        /****************************** 消费新Probe结果 ******************************/

        if (probe->updated_us > last_updated_us)
        {
            // 低负载窗口之前的超时，以及间隔过长的结果，不继承旧积分。
            if (probe->updated_us < runtime->low_load_since_us ||
                (last_updated_us != 0U &&
                 probe->updated_us - last_updated_us >
                 LINKG_SWITCH_POLICY_PROBE_MAX_RESULT_GAP_US))
            {
                runtime->timeout_count[index] = 0U;
            }

            runtime->last_probe_updated_us[index] = probe->updated_us;

            // 只有低负载窗口内的新超时结果才增加积分。
            if (!probe->reachable &&
                probe->updated_us >= runtime->low_load_since_us)
            {
                if (runtime->timeout_count[index] <
                    LINKG_SWITCH_POLICY_PROBE_TIMEOUT_COUNT)
                {
                    runtime->timeout_count[index]++;
                }
            }
            else
            {
                // 收到正常响应，或结果不属于低负载窗口，清除积分。
                runtime->timeout_count[index] = 0U;
            }
        }
        else
        {
            /****************************** 处理重复或乱序结果 ******************************/

            // 重复结果不重复计分；历史结果过期或时间回退时清除积分。
            if (last_updated_us == 0U ||
                last_updated_us > observed_us ||
                observed_us - last_updated_us >
                    LINKG_SWITCH_POLICY_PROBE_MAX_RESULT_AGE_US)
            {
                runtime->timeout_count[index] = 0U;
            }
        }

        /****************************** 检查当前业务超时次数 ******************************/

        if (runtime->timeout_count[index] <
            LINKG_SWITCH_POLICY_PROBE_TIMEOUT_COUNT)
        {
            all_failed = false;
        }
    }

    /****************************** 最终故障判决 ******************************/

    // 必须同时满足三类连续超时和低负载持续时间要求。
    if (all_failed &&
        observed_us - runtime->low_load_since_us >=
        LINKG_SWITCH_POLICY_PROBE_LOW_LOAD_HOLD_US)
    {
        // 保留黑洞标记，防止后续仅凭强RSSI直接回切Wi-Fi。
        runtime->blackhole_latched = true;
        *probe_down                = true;
    }

    pthread_mutex_unlock(&g_switch.lock);

    return 0;
}

/****************************** Wi-Fi策略入口 ******************************/

/**
 * @brief 检测Wi-Fi硬故障、无线丢包及队列溢出并触发初始策略事件。
 *
 * 硬故障优先于软件质量判决。
 * 软件质量判决必须满足维护、候选链路及容量限制。
 * LOSS优先于LOAD，同一轮最多交付一个初始事件。
 *
 * 不直接修改Plan，不管理PlanSync及后续质量验证事务。
 */
int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_loss_state_t loss_state;
    linkg_switch_policy_load_state_t load_state;
    bool                             probe_down;
    int                              ret;

    /****************************** 输入有效性 ******************************/

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (!input->observation.valid || input->observation.peer_node_id != input->peer_node_id)
    {
        return 0;
    }

    /****************************** Probe低负载逻辑断链检测 ******************************/

    // 与LOSS/LOAD独立：低负载、三类Probe均连续超时3次，视为逻辑硬故障。
    // 先维护检测状态，触发时与物理WIFI_DOWN共用原有直接切换事务。
    ret = _linkg_switch_policy_wifi_probe_process(input, &probe_down);
    if (ret != 0)
    {
        return ret;
    }

    /****************************** Wi-Fi硬故障 ******************************/

    // 只有明确UNUSABLE才触发硬故障，UNKNOWN不作为断链依据。
    if (input->observation.wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN);
    }

    /****************************** 软件迁移准入 ******************************/

    // Wi-Fi状态不明确或不可用时，不执行软件质量判决。
    if (input->observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE)
    {
        return 0;
    }

    // Wi-Fi维护期间禁止LOSS/LOAD软件质量切换。
    if (input->wifi_blocked)
    {
        return 0;
    }

    // 必须存在Wi-Fi单发业务，并具备可用的Cellular备用链路。
    if (!_linkg_switch_policy_wifi_has_candidate(input))
    {
        return 0;
    }

    /****************************** Probe逻辑硬故障 ******************************/

    // Probe黑洞不进入LOSS/LOAD质量比较，直接复用WIFI_DOWN切换事务。
    if (probe_down)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN);
    }

    // Wi-Fi业务负载不能超过Cellular配置容量。
    if (!_linkg_switch_policy_wifi_within_cellular_capacity(input))
    {
        return 0;
    }

    /****************************** 无线丢包判决 ******************************/

    ret = linkg_switch_policy_loss_process(input, now_us, &loss_state);
    if (ret != 0)
    {
        return ret;
    }

    // LOSS达到BAD门槛，触发Wi-Fi单发到冗余质量验证。
    if (loss_state == LINKG_SWITCH_POLICY_LOSS_BAD)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD);
    }

    /****************************** 队列溢出判决 ******************************/

    ret = linkg_switch_policy_load_process(input, now_us, &load_state);
    if (ret != 0)
    {
        return ret;
    }

    // LOAD达到BAD门槛，触发单类活跃业务的冗余迁移。
    if (load_state == LINKG_SWITCH_POLICY_LOAD_BAD)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD);
    }

    return 0;
}
