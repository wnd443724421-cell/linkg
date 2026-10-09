
/**
 * @file switch_policy_metric.c
 * @brief LinkG Wi-Fi无线质量辅助判断实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-08
 */

#include "switch_policy_internal.h"

#include <errno.h>

/****************************** RF质量判断 ******************************/

/**
 * @brief 根据Wi-Fi无线状态评估当前RF环境。
 *
 * 当前暂未实现SNR、带宽及MCS辅助判断，固定返回UNKNOWN。
 */
int linkg_switch_policy_rf_evaluate(const linkg_switch_wifi_radio_observation_t *radio, linkg_switch_policy_rf_state_t *rf_state)
{
    if (radio == NULL || rf_state == NULL)
    {
        return -EINVAL;
    }

    *rf_state = LINKG_SWITCH_POLICY_RF_UNKNOWN;

    return 0;
}
