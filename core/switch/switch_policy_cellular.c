
/**
 * @file switch_policy_cellular.c
 * @brief LinkG Cellular主链路的Wi-Fi恢复候选检测
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-22
 */

#include "switch_policy_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_link.h"

#include "switch_internal.h"
#include "switch_policy_event.h"
#include "switch_policy_operation.h"

/****************************** 恢复参数 ******************************/

#define LINKG_SWITCH_POLICY_RECOVERY_RSSI_HYSTERESIS_DB 4 // 比迁出时高4dB才启动恢复比较

/****************************** Cellular主链路策略 ******************************/

/**
 * @brief 检测Cellular主发业务的Wi-Fi恢复候选。
 *
 * 仅对具有有效迁移历史的Cellular单发业务执行恢复判断。
 * Wi-Fi RSSI必须达到迁出基线以上4dB，且满足恢复冷却时间。
 * 本函数只产生RECOVERY_READY事件，最终切回由Operation和Verify确认。
 */
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_event_t           event;
    linkg_switch_peer_runtime_t          *peer;
    const linkg_switch_policy_operation_t *operation;
    const linkg_class_send_plan_t         *current;
    uint32_t                               class_mask;
    uint32_t                               index;
    int32_t                                baseline;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    /****************************** 检查链路恢复条件 ******************************/

    // Maintenance期间禁止恢复，且Wi-Fi和Cellular必须同时可用。
    if (input->wifi_blocked || input->cellular_blocked ||
        !input->observation.valid ||
        input->observation.wifi.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        input->observation.cellular.state != LINKG_SWITCH_OBSERVATION_USABLE ||
        !input->observation.wifi.radio.statistics_valid ||
        input->observation.wifi.link_id == LINKG_LINK_ID_INVALID)
    {
        return 0;
    }

    /****************************** 获取当前Peer ******************************/

    class_mask = 0U;

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    // 存在活动切换事务时，不允许重复创建恢复候选。
    if (peer->role.sta.policy.group_mask != 0U)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return 0;
    }

    /****************************** 遍历Cellular主发业务 ******************************/

    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current   = &input->plan.classes[index];
        operation = &peer->role.sta.policy.operations[index];

        /****************************** 检查恢复资格 ******************************/

        // 当前业务必须空闲，并具有有效的Wi-Fi迁移历史和RSSI基线。
        if (operation->phase != LINKG_SWITCH_POLICY_PHASE_IDLE ||
            operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_NONE ||
            !operation->recovery.rssi_valid)
        {
            continue;
        }

        // 迁移后的恢复冷却时间尚未结束。
        if (operation->recovery.recovery_after_us != 0U &&
            now_us < operation->recovery.recovery_after_us)
        {
            continue;
        }

        /****************************** 检查当前发送计划 ******************************/

        // 只允许Cellular单发、Wi-Fi备用的业务参与恢复。
        if (current->mode != LINKG_SEND_MODE_SINGLE ||
            current->primary_link_id != input->observation.cellular.link_id ||
            current->secondary_link_id != input->observation.wifi.link_id)
        {
            continue;
        }

        /****************************** 判断Wi-Fi RSSI恢复 ******************************/

        // 当前RSSI比迁出时提高至少4dB，才允许进入恢复验证。
        baseline = operation->recovery.rssi_dbm;

        if ((int64_t)input->observation.wifi.radio.rssi_dbm - baseline >=
            LINKG_SWITCH_POLICY_RECOVERY_RSSI_HYSTERESIS_DB)
        {
            class_mask |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
        }
    }

    pthread_mutex_unlock(&g_switch.lock);

    /****************************** 检查恢复业务集合 ******************************/

    // 没有业务满足恢复条件，本轮不创建切换事务。
    if (class_mask == 0U)
    {
        return 0;
    }

    /****************************** 交付恢复候选事件 ******************************/

    // 只通知Operation创建恢复事务，不直接修改发送计划。
    memset(&event, 0, sizeof(event));

    event.type                              = LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY;
    event.peer_node_id                      = input->peer_node_id;
    event.peer_generation                   = input->peer_generation;
    event.occurred_us                       = now_us;
    event.payload.recovery_ready.class_mask = class_mask;

    return linkg_switch_policy_operation_process_event(&event, now_us);
}
