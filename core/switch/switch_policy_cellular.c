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

#define LINKG_SWITCH_POLICY_RECOVERY_RSSI_HYSTERESIS_DB  4     // 质量迁移后，RSSI比迁出时提高至少4dB
#define LINKG_SWITCH_POLICY_DIRECT_RECOVERY_RSSI_DBM  (-60)   // 非质量迁移后，Wi-Fi RSSI达到-60dBm即可直接恢复

/****************************** Cellular主链路策略 ******************************/

/**
 * @brief 检测Cellular单发业务的Wi-Fi恢复候选。
 *
 * LOSS/LOAD维持既有的RSSI滞回及冗余Verify；
 * WIFI_DOWN/UNKNOWN/NONE在Wi-Fi Path可用且RSSI足够强时直接恢复单发。
 * 两类候选分开提交，避免混合业务错误跳过Verify。
 * 本函数只生成策略事件；PlanSync及ACK由Operation负责。
 */
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_event_t           event;
    linkg_switch_peer_runtime_t          *peer;
    const linkg_switch_policy_operation_t *operation;
    const linkg_class_send_plan_t         *current;
    uint32_t                               verify_mask;
    uint32_t                               direct_mask;
    uint32_t                               class_mask;
    uint32_t                               index;
    int32_t                                baseline;

    if (input == NULL || now_us == 0U)
    {
        return -EINVAL;
    }

    /****************************** 检查链路恢复条件 ******************************/

    // Maintenance期间禁止恢复，Wi-Fi和Cellular必须同时可用。
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

    verify_mask = 0U;
    direct_mask = 0U;

    pthread_mutex_lock(&g_switch.lock);

    peer = linkg_switch_find_peer_generation_locked(input->peer_node_id, input->peer_generation);
    if (peer == NULL)
    {
        pthread_mutex_unlock(&g_switch.lock);
        return -ESTALE;
    }

    // 当前存在活动切换事务时，不重复产生恢复候选。
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

        /****************************** 公共恢复资格 ******************************/

        // 业务必须处于空闲状态，且未处于恢复冷却期。
        if (operation->phase != LINKG_SWITCH_POLICY_PHASE_IDLE ||
            (operation->recovery.recovery_after_us != 0U &&
             now_us < operation->recovery.recovery_after_us))
        {
            continue;
        }

        // 只允许Cellular单发、Wi-Fi备用的业务参与恢复。
        if (current->mode != LINKG_SEND_MODE_SINGLE ||
            current->primary_link_id != input->observation.cellular.link_id ||
            current->secondary_link_id != input->observation.wifi.link_id)
        {
            continue;
        }

        /****************************** 第一类：质量迁移恢复 ******************************/

        if (operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS ||
            operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
        {
            // 保留既有算法：必须具有迁出Wi-Fi时的有效RSSI基线。
            if (!operation->recovery.rssi_valid)
            {
                continue;
            }

            baseline = operation->recovery.rssi_dbm;

            if ((int64_t)input->observation.wifi.radio.rssi_dbm - baseline >=
                LINKG_SWITCH_POLICY_RECOVERY_RSSI_HYSTERESIS_DB)
            {
                verify_mask |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
            }

            continue;
        }

        /****************************** 第二类：非质量迁移直接恢复 ******************************/

        // 只有已知的非质量迁移原因才允许使用强RSSI直接恢复。
        if (operation->migration_reason != LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN &&
            operation->migration_reason != LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN &&
            operation->migration_reason != LINKG_SWITCH_POLICY_MIGRATION_NONE)
        {
            continue;
        }

        // Probe黑洞迁出期间，即使RSSI很强也不得直接回切。
        // 必须先确认三类业务的Wi-Fi Probe均已重新响应。
        if (peer->role.sta.policy.wifi_probe.blackhole_latched)
        {
            bool     probe_recovered;
            uint32_t probe_index;

            probe_recovered = true;

            for (probe_index = 0U; probe_index < LINKG_TRANSPORT_CLASS_COUNT; probe_index++)
            {
                const linkg_switch_probe_observation_t *probe;

                probe = &input->observation.wifi.probe[probe_index];
                if (!probe->valid || !probe->reachable || probe->updated_us == 0U ||
                    probe->updated_us > input->observation.collected_us ||
                    input->observation.collected_us - probe->updated_us > 2500000ULL)
                {
                    probe_recovered = false;
                    break;
                }
            }

            if (!probe_recovered)
            {
                continue;
            }
        }

        // 无需旧RSSI基线；Wi-Fi信号足够强即可提交单发切换。
        if (input->observation.wifi.radio.rssi_dbm >= LINKG_SWITCH_POLICY_DIRECT_RECOVERY_RSSI_DBM)
        {
            direct_mask |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
        }
    }

    pthread_mutex_unlock(&g_switch.lock);

    /****************************** 选择本轮恢复事件 ******************************/

    // 优先保留原质量恢复路径；若两类同时满足，下一轮再处理直接恢复类。
    if (verify_mask != 0U)
    {
        class_mask = verify_mask;
    }
    else if (direct_mask != 0U)
    {
        class_mask = direct_mask;
    }
    else
    {
        return 0;
    }

    /****************************** 交付恢复候选事件 ******************************/

    // 仅生成初始事件，不在Policy中直接修改发送计划。
    memset(&event, 0, sizeof(event));

    event.type                              = verify_mask != 0U ?
                                              LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY :
                                              LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY;
    event.peer_node_id                      = input->peer_node_id;
    event.peer_generation                   = input->peer_generation;
    event.occurred_us                       = now_us;
    event.payload.recovery_ready.class_mask = class_mask;

    return linkg_switch_policy_operation_process_event(&event, now_us);
}
