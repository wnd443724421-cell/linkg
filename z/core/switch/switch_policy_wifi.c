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
 * @brief 判断当前Cellular链路是否具备基本使用条件。
 */
static bool _linkg_switch_policy_cellular_usable(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_cellular_observation_t *cellular;

    if (input == NULL || input->cellular_blocked)
    {
        return false;
    }

    cellular = &input->observation.cellular;

    if (!cellular->available ||
        cellular->link_id == LINKG_LINK_ID_INVALID)
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
    linkg_send_plan_t        plan;
    linkg_class_send_plan_t *current;
    uint32_t                 class_index;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (input->observation.cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return -ENODEV;
    }

    plan = input->plan;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &plan.classes[class_index];
        if (current->mode == LINKG_SEND_MODE_NONE)
        {
            continue;
        }

        if (current->primary_link_id == input->observation.wifi.link_id)
        {
            current->mode              = LINKG_SEND_MODE_SINGLE;
            current->primary_link_id   = input->observation.cellular.link_id;
            current->secondary_link_id = LINKG_LINK_ID_INVALID;
        }
        else if (current->secondary_link_id == input->observation.wifi.link_id)
        {
            current->mode              = LINKG_SEND_MODE_SINGLE;
            current->secondary_link_id = LINKG_LINK_ID_INVALID;
        }
    }

    return linkg_switch_plan_update_if_current(input->peer_node_id, input->peer_generation, &input->plan, &plan, true, now_us);
}

/****************************** 单链路处理 ******************************/

/**
 * @brief 将Wi-Fi主链路发送计划切换为双链路冗余。
 */
static int _linkg_switch_policy_enter_dual_verify(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_send_plan_t        plan;
    linkg_class_send_plan_t *current;
    uint32_t                 class_index;
    bool                     changed;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    plan    = input->plan;
    changed = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        current = &plan.classes[class_index];
        if (current->mode == LINKG_SEND_MODE_SINGLE &&
            current->primary_link_id == input->observation.wifi.link_id &&
            current->secondary_link_id == input->observation.cellular.link_id)
        {
            current->mode = LINKG_SEND_MODE_REDUNDANT;
            changed       = true;
        }
    }

    if (!changed)
    {
        return 0;
    }

    return linkg_switch_plan_update_if_current(input->peer_node_id, input->peer_generation, &input->plan, &plan, true, now_us);
}

/**
 * @brief 处理Wi-Fi主链路单发状态。
 */
static int _linkg_switch_policy_process_wifi_single(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_loss_state_t loss_state;
    int                              ret;

    ret = linkg_switch_policy_loss_process(input, now_us, &loss_state);
    if (ret != 0)
    {
        return ret;
    }

    if (loss_state != LINKG_SWITCH_POLICY_LOSS_BAD)
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
    uint32_t class_index;
    bool     redundant;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_switch_policy_cellular_usable(input))
    {
        return 0;
    }

    if (input->wifi_blocked)
    {
        return 0;
    }

    if (_linkg_switch_policy_wifi_hard_failed(input))
    {
        return _linkg_switch_policy_switch_to_cellular(input, now_us);
    }

    redundant = false;

    for (class_index = 0U; class_index < LINKG_TRANSPORT_CLASS_COUNT; class_index++)
    {
        if (input->plan.classes[class_index].primary_link_id != input->observation.wifi.link_id)
        {
            continue;
        }

        if (input->plan.classes[class_index].mode == LINKG_SEND_MODE_SINGLE)
        {
            return _linkg_switch_policy_process_wifi_single(input, now_us);
        }

        redundant = redundant || input->plan.classes[class_index].mode == LINKG_SEND_MODE_REDUNDANT;
    }

    return redundant ? _linkg_switch_policy_process_dual_verify(input, now_us) : 0;
}
