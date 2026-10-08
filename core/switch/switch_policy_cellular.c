/**
 * @file switch_policy_cellular.c
 * @brief LinkG Cellular主链路恢复策略实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-08
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdint.h>

/****************************** Cellular主链路策略 ******************************/

/**
 * @brief 处理当前Cellular为主链路的Wi-Fi恢复策略。
 */
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    return 0;
}
