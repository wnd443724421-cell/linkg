
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
