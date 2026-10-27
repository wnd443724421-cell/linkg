/**
 * @file switch_policy_verify.c
 * @brief LinkG冗余阶段LOSS无线质量与LOAD队列承载验证
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-26
 *
 * 流程：ACK启动 → 三秒采集 → LOSS按无线积分、LOAD按队列溢出判决
 *       → 固化VERIFY_DONE → Operation决定最终单发Plan。
 *
 * 本模块只评估质量，不修改发送计划，也不执行PlanSync。
 */

#include "switch_policy_verify.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"


/**
 * @brief 将已固定的验证结果填充为VERIFY_DONE事件。
 */
static void _linkg_switch_verify_fill_event(const linkg_switch_policy_verify_runtime_t *runtime, linkg_switch_policy_event_t *event)
{
    memset(event, 0, sizeof(*event));

    event->type                             = LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE;
    event->peer_node_id                     = runtime->peer_node_id;
    event->peer_generation                  = runtime->peer_generation;
    event->occurred_us                      = runtime->finished_us;
    event->payload.verify_done.operation_id = runtime->operation_id;
    event->payload.verify_done.result       = runtime->result;
}


/**
 * @brief 固定本次验证结果并生成VERIFY_DONE事件。
 *
 * 结果只允许固化一次，后续通过result_pending重复交付。
 */
static void _linkg_switch_verify_finish(linkg_switch_policy_verify_runtime_t *runtime, linkg_switch_policy_verify_result_t result, uint64_t now_us, linkg_switch_policy_verify_process_output_t *output)
{
    runtime->result         = result;
    runtime->finished_us    = now_us;
    runtime->result_pending = true;

    _linkg_switch_verify_fill_event(runtime, &output->event);
    output->event_ready = true;
}

/**
 * @brief 清理当前验证会话及全部运行状态。
 *
 * 仅清理Verify运行时数据，不修改Operation事务或发送计划。
 * 由Operation在事务取消、结束或明确放弃验证时调用。
 */
void linkg_switch_policy_verify_reset(linkg_switch_policy_verify_runtime_t *runtime)
{
    if (runtime == NULL)
    {
        return;
    }

    memset(runtime, 0, sizeof(*runtime));
}

/**
 * @brief 冗余PlanSync ACK确认后启动一次质量验证会话。
 *
 * 校验事务身份、验证原因及链路观测，固定本次验证的
 * Peer、Operation、Link实例和三秒时间窗口。
 * 不执行质量判决，不修改发送计划。
 */
int linkg_switch_policy_verify_begin(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_begin_params_t *params)
{
    const linkg_switch_observation_t *observation;
    uint32_t                          operation_id;
    uint32_t                          class_index;
    uint32_t                          other_index;

    /****************************** 基础参数检查 ******************************/

    if (runtime == NULL || params == NULL || params->observation == NULL)
    {
        return -EINVAL;
    }

    if (params->peer_node_id == 0U ||
        params->peer_node_id == UINT8_MAX ||
        params->peer_generation == 0U ||
        params->class_mask == 0U ||
        (params->class_mask & ~LINKG_SWITCH_POLICY_CLASS_MASK_ALL) != 0U ||
        params->now_us == 0U ||
        params->now_us > UINT64_MAX - LINKG_SWITCH_POLICY_VERIFY_DURATION_US)
    {
        return -EINVAL;
    }

    /****************************** 验证原因检查 ******************************/

    // 仅允许LOSS进入验证，或Cellular主链路发起RECOVERY验证。
    if (params->reason != LINKG_SWITCH_POLICY_REASON_LOSS &&
        params->reason != LINKG_SWITCH_POLICY_REASON_RECOVERY)
    {
        return -EINVAL;
    }

    // LOSS不是恢复事务，不允许携带历史迁移原因。
    if (params->reason == LINKG_SWITCH_POLICY_REASON_LOSS &&
        params->recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_NONE)
    {
        return -EINVAL;
    }

    // RECOVERY仅支持原始LOSS或LOAD迁移原因。
    if (params->reason == LINKG_SWITCH_POLICY_REASON_RECOVERY &&
        params->recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_LOSS &&
        params->recovery_reason != LINKG_SWITCH_POLICY_MIGRATION_LOAD)
    {
        return -EINVAL;
    }

    /****************************** 会话状态检查 ******************************/

    // 不允许覆盖活动验证或尚未消费的固定结果。
    if (runtime->active || runtime->result_pending)
    {
        return -EBUSY;
    }

    /****************************** 链路观测检查 ******************************/

    observation = params->observation;

    if (!observation->valid ||
        observation->peer_node_id != params->peer_node_id ||
        observation->collected_us == 0U ||
        observation->collected_us > params->now_us ||
        params->now_us - observation->collected_us > LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US)
    {
        return -EINVAL;
    }

    // 冗余验证启动时必须存在两条可用物理链路。
    if (observation->wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        observation->wifi.link_id == LINKG_LINK_ID_INVALID ||
        observation->cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return -EINVAL;
    }

    /****************************** Operation身份检查 ******************************/

    operation_id = LINKG_SWITCH_POLICY_OPERATION_ID_INVALID;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        // 未参与业务不允许携带Operation ID。
        if ((params->class_mask & LINKG_SWITCH_POLICY_CLASS_BIT(class_index)) == 0U)
        {
            if (params->operation_ids[class_index] != LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
            {
                return -EINVAL;
            }

            continue;
        }

        // 参与业务必须具有有效且唯一的Operation ID。
        if (params->operation_ids[class_index] == LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
        {
            return -EINVAL;
        }

        for (other_index = 0U; other_index < class_index; other_index++)
        {
            if (params->operation_ids[class_index] == params->operation_ids[other_index])
            {
                return -EINVAL;
            }
        }

        // 首个参与业务的Operation ID作为组级事件身份。
        if (operation_id == LINKG_SWITCH_POLICY_OPERATION_ID_INVALID)
        {
            operation_id = params->operation_ids[class_index];
        }
    }

    /****************************** 初始化验证会话 ******************************/

    // 全部检查通过后清空上一次会话状态。
    memset(runtime, 0, sizeof(*runtime));

    // 固定Peer及Operation事务身份。
    runtime->peer_node_id    = params->peer_node_id;
    runtime->peer_generation = params->peer_generation;
    runtime->operation_id    = operation_id;

    memcpy(runtime->operation_ids, params->operation_ids, sizeof(runtime->operation_ids));

    // 保存本次验证范围和进入原因。
    runtime->class_mask      = params->class_mask;
    runtime->reason          = params->reason;
    runtime->recovery_reason = params->recovery_reason;

    // 固定本次验证对应的物理Link实例。
    runtime->wifi_link_id     = observation->wifi.link_id;
    runtime->cellular_link_id = observation->cellular.link_id;

    // 从冗余ACK确认时刻开始固定三秒验证。
    runtime->started_us  = params->now_us;
    runtime->deadline_us = params->now_us + LINKG_SWITCH_POLICY_VERIFY_DURATION_US;

    // LOSS及LOSS原因的恢复验证从异常积分开始。
    if (params->reason == LINKG_SWITCH_POLICY_REASON_LOSS ||
        params->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS)
    {
        runtime->wifi_loss_score = LINKG_SWITCH_POLICY_VERIFY_ENTER_SCORE;
    }

    runtime->result = LINKG_SWITCH_POLICY_VERIFY_NONE;

    // 所有会话信息初始化完成后才允许process消费。
    runtime->active = true;

    return 0;
}

/**
 * @brief 周期处理双链路质量验证并交付验证结果。
 *
 * 验证期间持续采集链路观测，三秒到期后按验证原因执行质量判决。
 * 最终结果只计算一次，并持续交付VERIFY_DONE直至Operation消费。
 */
int linkg_switch_policy_verify_process(linkg_switch_policy_verify_runtime_t *runtime, const linkg_switch_policy_verify_process_params_t *params, linkg_switch_policy_verify_process_output_t *output)
{
    const linkg_switch_observation_t *observation;
    bool                              observation_fresh;
    bool                              paths_usable;
    bool                              wifi_link_changed;
    bool                              cellular_link_changed;


    /****************************** 一、参数与会话检查 ******************************/

    // 检查输入参数。
    if (runtime == NULL || params == NULL || output == NULL ||
        params->observation == NULL || params->now_us == 0U)
    {
        return -EINVAL;
    }

    // 清空本轮事件输出，避免保留上次调用的结果。
    memset(output, 0, sizeof(*output));

    // 当前没有活动验证会话，无需处理。
    if (!runtime->active)
    {
        return 0;
    }

    // 拒绝其他Peer的观测或早于验证启动时间的调用。
    if (params->observation->peer_node_id != runtime->peer_node_id ||
        params->now_us < runtime->started_us)
    {
        return -ESTALE;
    }

    // 已固定结果直接重复交付，不重新采样或计算。
    if (runtime->result_pending)
    {
        _linkg_switch_verify_fill_event(runtime, &output->event);
        output->event_ready = true;
        return 0;
    }


    /****************************** 二、观测与链路检查 ******************************/

    observation = params->observation;

    // 仅允许验证启动后、截止时间内生成的新鲜观测参与处理。
    observation_fresh = observation->valid &&
                        observation->collected_us > runtime->started_us &&
                        observation->collected_us <= runtime->deadline_us &&
                        observation->collected_us <= params->now_us &&
                        params->now_us - observation->collected_us <= LINKG_SWITCH_POLICY_VERIFY_OBSERVATION_MAX_AGE_US;

    paths_usable = false;

    if (observation_fresh)
    {
        /****************************** Link实例连续性 ******************************/

        // 仅将已确认的新Link实例视为变化，UNKNOWN状态下的无效ID不直接判定断链。
        wifi_link_changed     = observation->wifi.link_id != LINKG_LINK_ID_INVALID && observation->wifi.link_id != runtime->wifi_link_id;
        cellular_link_changed = observation->cellular.link_id != LINKG_LINK_ID_INVALID && observation->cellular.link_id != runtime->cellular_link_id;

        if (wifi_link_changed || cellular_link_changed)
        {
            // 原始Link实例已经改变，旧验证样本不能继续用于本次质量判断。
            runtime->path_interrupted = true;

            _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE, params->now_us, output);
            return 0;
        }

        /****************************** 明确链路故障 ******************************/

        if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE || observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
        {
            runtime->path_interrupted = true;

            // Wi-Fi已明确不可用，Cellular仍可用，提前选择Cellular。
            if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE &&
                observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE)
            {
                _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_CELLULAR, params->now_us, output);
                return 0;
            }

            // Cellular已明确不可用，Wi-Fi仍可用，提前选择Wi-Fi。
            if (observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE &&
                observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE)
            {
                _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_WIFI, params->now_us, output);
                return 0;
            }

            // 双链路均明确不可用，当前没有安全的单发目标。
            if (observation->wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE &&
                observation->cellular.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
            {
                _linkg_switch_verify_finish(runtime, LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE, params->now_us, output);
                return 0;
            }
        }

        /****************************** 采样条件 ******************************/

        // 仅在两条链路均可用、实例未变化且没有历史中断时允许采样。
        paths_usable = !runtime->path_interrupted &&
                    observation->wifi.state == LINKG_SWITCH_OBSERVATION_USABLE &&
                    observation->cellular.state == LINKG_SWITCH_OBSERVATION_USABLE &&
                    observation->wifi.link_id == runtime->wifi_link_id &&
                    observation->cellular.link_id == runtime->cellular_link_id;
    }


    /****************************** 三、验证数据采集 ******************************/

    if (paths_usable)
    {
        if (runtime->reason == LINKG_SWITCH_POLICY_REASON_LOSS)
        {
            // LOSS：
            // 1. 检查Wi-Fi队列负载。
            // 2. 采集Wi-Fi Wire，更新恢复积分。
            // 3. 采集Cellular质量。
            // 4. 记录LOAD抢占条件。
        }
        else if (runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS)
        {
            // RECOVERY + LOSS：
            // 1. 采集参与业务的Wi-Fi Wire。
            // 2. 更新各业务恢复积分。
            // 3. 记录对应HCC队列压力。
        }
        else if (runtime->recovery_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
        {
            // RECOVERY + LOAD：
            // 1. 采集对应HCC队列水位。
            // 2. 采集队列溢出。
            // 3. 记录实际负载。
        }
    }

    /****************************** 四、截止时间与质量判决 ******************************/

    // 未达到三秒截止时间时结束本轮处理。
    // 到期后根据reason和recovery_reason选择判决路径。
    // LOSS：Wi-Fi丢包质量与负载拥塞判决。
    // RECOVERY + LOSS：Wi-Fi丢包恢复判决。
    // RECOVERY + LOAD：Wi-Fi负载承载恢复判决。

    /****************************** 五、固定结果与事件交付 ******************************/

    // 固定本次质量验证结果。
    // 保存验证完成时间。
    // 设置result_pending，防止重复计算。
    // 填充VERIFY_DONE事件并设置event_ready。

    // 框架尚未实现，禁止将其视为成功完成验证。
    (void)runtime;
    (void)params;
    (void)output;

    return -ENOSYS;
}
