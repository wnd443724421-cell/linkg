/**
 * @file switch_policy_wifi.c
 * @brief LinkG Wi-Fi主链路切换策略实现
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

#include "switch_plan.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断当前Cellular备用链路是否具备基本使用条件。
 */
static bool _linkg_switch_policy_cellular_usable(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_cellular_observation_t *cellular;

    if (input == NULL)
    {
        return false;
    }

    if (input->cellular_blocked)
    {
        return false;
    }

    cellular = &input->observation.cellular;

    if (!cellular->available)
    {
        return false;
    }

    if (cellular->link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

    return true;
}

/**
 * @brief 判断当前Wi-Fi主链路是否已经发生明确硬故障。
 */
static bool _linkg_switch_policy_wifi_hard_failed(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_wifi_observation_t *wifi;

    if (input == NULL)
    {
        return false;
    }

    wifi = &input->observation.wifi;

    if (!wifi->available)
    {
        return true;
    }

    if (wifi->radio.valid && !wifi->radio.connected)
    {
        return true;
    }

    return false;
}

/**
 * @brief 将当前STA发送计划切换为Cellular单链路。
 */
static int _linkg_switch_policy_switch_to_cellular(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_send_plan_t plan;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (input->observation.cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return -ENODEV;
    }

    memset(&plan, 0, sizeof(plan));

    plan.mode              = LINKG_SEND_MODE_SINGLE;
    plan.primary_link_id   = input->observation.cellular.link_id;
    plan.secondary_link_id = LINKG_LINK_ID_INVALID;

    return linkg_switch_plan_commit_local(input->peer_node_id, input->peer_generation, &plan, now_us);
}

/****************************** 单链路处理 ******************************/

/**
 * @brief 请求进入Wi-Fi主链路双发健康检查。
 *
 * 当前暂未实现健康检查计划提交。
 */
static int _linkg_switch_policy_enter_dual_verify(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 处理Wi-Fi主链路单发状态。
 */
static int _linkg_switch_policy_process_wifi_single(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_loss_state_t loss_state;
    linkg_switch_policy_rf_state_t   rf_state;
    int                              ret;

    ret = linkg_switch_policy_loss_process(input, now_us, &loss_state);
    if (ret != 0)
    {
        return ret;
    }

    if (loss_state == LINKG_SWITCH_POLICY_LOSS_NONE)
    {
        return 0;
    }

    if (loss_state == LINKG_SWITCH_POLICY_LOSS_BAD)
    {
        return _linkg_switch_policy_enter_dual_verify(input, now_us);
    }

    if (loss_state != LINKG_SWITCH_POLICY_LOSS_SUSPECT)
    {
        return 0;
    }

    ret = linkg_switch_policy_rf_evaluate(&input->observation.wifi.radio, &rf_state);
    if (ret != 0)
    {
        return ret;
    }

    if (rf_state != LINKG_SWITCH_POLICY_RF_DEGRADED)
    {
        return 0;
    }

    return _linkg_switch_policy_enter_dual_verify(input, now_us);
}

/****************************** 健康检查 ******************************/

/**
 * @brief 处理Wi-Fi主链路双发健康检查状态。
 */
static int _linkg_switch_policy_process_dual_verify(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    (void)input;
    (void)now_us;

    return 0;
}

/****************************** Wi-Fi主链路策略 ******************************/

/**
 * @brief 处理当前Wi-Fi为主链路的切换策略。
 */
int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_policy_cellular_usable(input))
    {
        return 0;
    }

    if (_linkg_switch_policy_wifi_hard_failed(input))
    {
        return _linkg_switch_policy_switch_to_cellular(input, now_us);
    }

    if (input->wifi_blocked)
    {
        return 0;
    }

    if (input->plan.mode == LINKG_SEND_MODE_SINGLE)
    {
        return _linkg_switch_policy_process_wifi_single(input, now_us);
    }

    if (input->plan.mode == LINKG_SEND_MODE_REDUNDANT)
    {
        return _linkg_switch_policy_process_dual_verify(input, now_us);
    }

    return 0;
}
