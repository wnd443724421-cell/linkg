
/**
 * @file switch_policy_loss.c
 * @brief LinkG Wi-Fi丢包质量判决实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-08
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdint.h>

/****************************** 丢包质量判断 ******************************/

/**
 * @brief 根据当前观测判断Wi-Fi丢包退化程度。
 *
 * 当前暂未实现丢包窗口累计与风险判决，固定返回NONE。
 */
int linkg_switch_policy_loss_process(const linkg_switch_policy_input_t *input, uint64_t now_us, linkg_switch_policy_loss_state_t *loss_state)
{
    if (input == NULL || loss_state == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    *loss_state = LINKG_SWITCH_POLICY_LOSS_NONE;

    return 0;
}
