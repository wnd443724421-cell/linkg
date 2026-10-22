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

#include "switch_policy_event.h"
#include "switch_policy_operation.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断至少有一个Wi-Fi主发业务，且存在可用的Cellular备份。
 */
static bool _linkg_switch_policy_wifi_has_candidate(const linkg_switch_policy_input_t *input)
{
    const linkg_class_send_plan_t *current;
    uint32_t                       index;

    if (input->cellular_blocked ||
        input->observation.cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.cellular.link_id == LINKG_LINK_ID_INVALID)
    {
        return false;
    }

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
 * @brief 使用Wi-Fi双向实际业务速率判断是否允许软件质量迁移。
 *
 * 配置或流量样本缺失时不进行LOSS/LOAD迁移；硬故障不受此门槛限制。
 */
static bool _linkg_switch_policy_wifi_within_cellular_capacity(const linkg_switch_policy_input_t *input)
{
    const linkg_switch_traffic_observation_t *traffic;
    linkg_cellular_config_t                   config;
    uint64_t                                  limit_bps;

    traffic = &input->observation.wifi.traffic;
    if (!traffic->valid || traffic->updated_us == 0U ||
        linkg_config_get_cellular(&config) != 0)
    {
        return false;
    }

    limit_bps = (uint64_t)config.max_load_mbps * 1000000ULL;
    return traffic->tx_bps <= limit_bps &&
           traffic->rx_bps <= limit_bps - traffic->tx_bps;
}

/**
 * @brief 交付同步策略事件给唯一Switch Worker内的Operation状态机。
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

/****************************** Wi-Fi策略入口 ******************************/

/**
 * @brief 判断Wi-Fi硬故障、无线丢包或队列溢出并产生策略初始事件。
 *
 * 不直接调用Plan，不管理PlanSync ACK，不在Policy内处理VERIFY_DONE。
 */
int linkg_switch_policy_wifi_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_loss_state_t loss_state;
    linkg_switch_policy_load_state_t load_state;
    int                              ret;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    if (!input->observation.valid ||
        input->observation.peer_node_id != input->peer_node_id)
    {
        return 0;
    }

    // 只有确认UNUSABLE才触发硬故障，不将UNKNOWN当成物理断开。
    if (input->observation.wifi.state == LINKG_SWITCH_OBSERVATION_UNUSABLE)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN);
    }

    if (input->wifi_blocked ||
        input->observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        !_linkg_switch_policy_wifi_has_candidate(input) ||
        !_linkg_switch_policy_wifi_within_cellular_capacity(input))
    {
        return 0;
    }

    ret = linkg_switch_policy_loss_process(input, now_us, &loss_state);
    if (ret != 0)
    {
        return ret;
    }

    if (loss_state == LINKG_SWITCH_POLICY_LOSS_BAD)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD);
    }

    ret = linkg_switch_policy_load_process(input, now_us, &load_state);
    if (ret != 0)
    {
        return ret;
    }

    if (load_state == LINKG_SWITCH_POLICY_LOAD_BAD)
    {
        return _linkg_switch_policy_wifi_raise(input, now_us, LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD);
    }

    return 0;
}
