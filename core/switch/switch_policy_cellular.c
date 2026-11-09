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

#define LINKG_SWITCH_POLICY_RECOVERY_RSSI_HYSTERESIS_DB       3     // LOSS恢复要求RSSI较迁出时提高至少3dB
#define LINKG_SWITCH_POLICY_LOAD_RECOVERY_RSSI_HYSTERESIS_DB  0     // LOAD恢复要求RSSI较迁出时提高至少0dB
#define LINKG_SWITCH_POLICY_DIRECT_RECOVERY_RSSI_DBM         (-60)  // 非质量迁移后，RSSI达到-60dBm直接恢复

/****************************** Cellular主链路策略 ******************************/


/**
 * @brief 检测Cellular单发业务的Wi-Fi恢复候选。
 *
 * 所有迁移原因统一按照REALTIME、VIDEO、DATA优先级恢复。
 * 更高优先级业务仍在Cellular且不满足恢复条件时，禁止低优先级抢先恢复。
 * 同一迁移原因且连续满足条件的业务允许整组恢复。
 * LOSS和LOAD分别保留原有质量验证，非质量迁移保留直接恢复。
 * 本函数只生成策略事件；PlanSync及ACK由Operation负责。
 */
int linkg_switch_policy_cellular_process(const linkg_switch_policy_input_t *input, uint64_t now_us)
{
    linkg_switch_policy_event_t             event;
    linkg_switch_peer_runtime_t            *peer;
    const linkg_switch_policy_operation_t  *operation;
    const linkg_class_send_plan_t          *current;
    const linkg_switch_probe_observation_t *probe;
    linkg_switch_policy_migration_reason_t  group_reason;
    linkg_switch_policy_event_type_t        event_type;
    uint32_t                                class_mask;
    uint32_t                                index;
    uint32_t                                probe_index;
    int32_t                                 baseline;
    bool                                    recovery_ready;
    bool                                    probe_recovered;

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

    class_mask   = 0U;
    group_reason = LINKG_SWITCH_POLICY_MIGRATION_NONE;
    event_type   = LINKG_SWITCH_POLICY_EVENT_NONE;

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

    /****************************** 按业务优先级生成恢复候选 ******************************/

    // 严格按照REALTIME、VIDEO、DATA顺序检查。
    // 低优先级业务必须等待更高优先级的Cellular业务先满足恢复资格。
    for (index = 0U; index < LINKG_TRANSPORT_CLASS_COUNT; index++)
    {
        current   = &input->plan.classes[index];
        operation = &peer->role.sta.policy.operations[index];

        /****************************** 一、检查当前业务链路 ******************************/

        // 未启用业务不参与恢复优先级约束。
        if (current->mode == LINKG_SEND_MODE_NONE)
        {
            continue;
        }

        // 已经在Wi-Fi单发的高优先级业务不阻挡后续业务。
        if (current->mode == LINKG_SEND_MODE_SINGLE &&
            current->primary_link_id == input->observation.wifi.link_id)
        {
            continue;
        }

        // 当前业务尚未处于可恢复的Cellular单发状态。
        // 停止检查更低优先级业务，防止恢复顺序反转。
        if (current->mode != LINKG_SEND_MODE_SINGLE ||
            current->primary_link_id != input->observation.cellular.link_id ||
            current->secondary_link_id != input->observation.wifi.link_id)
        {
            break;
        }

        /****************************** 二、检查恢复原因分组 ******************************/

        // 已选择恢复组后，低优先级业务必须具有相同迁移原因。
        // 不同原因使用不同恢复判决，不能混入当前事务。
        if (class_mask != 0U &&
            operation->migration_reason != group_reason)
        {
            break;
        }

        /****************************** 三、检查公共恢复资格 ******************************/

        // 当前业务尚有活动事务或未结束冷却期时，停止向下选择。
        // 即使低优先级业务已经满足条件，也不能绕过当前业务。
        if (operation->phase != LINKG_SWITCH_POLICY_PHASE_IDLE ||
            (operation->recovery.recovery_after_us != 0U &&
             now_us < operation->recovery.recovery_after_us))
        {
            break;
        }

        recovery_ready = false;

        /****************************** 四、LOSS无线质量恢复 ******************************/

        if (operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS)
        {
            if (operation->recovery.rssi_valid)
            {
                baseline = operation->recovery.rssi_dbm;

                // 保留原有LOSS RSSI恢复门槛。
                if ((int64_t)input->observation.wifi.radio.rssi_dbm - baseline >=
                    LINKG_SWITCH_POLICY_RECOVERY_RSSI_HYSTERESIS_DB)
                {
                    recovery_ready = true;
                }
            }
        }

        /****************************** 五、LOAD队列溢出恢复 ******************************/

        else if (operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
        {
            if (operation->recovery.rssi_valid)
            {
                baseline = operation->recovery.rssi_dbm;

                // LOAD恢复要求RSSI不低于迁出基线。
                if ((int64_t)input->observation.wifi.radio.rssi_dbm - baseline >=
                    LINKG_SWITCH_POLICY_LOAD_RECOVERY_RSSI_HYSTERESIS_DB)
                {
                    recovery_ready = true;
                }
            }
        }

        /****************************** 六、非质量迁移直接恢复 ******************************/

        else if (operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_WIFI_DOWN ||
                 operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_UNKNOWN ||
                 operation->migration_reason == LINKG_SWITCH_POLICY_MIGRATION_NONE)
        {
            probe_recovered = true;

            // Probe黑洞迁出期间，必须先确认三类Wi-Fi Probe均恢复。
            if (peer->role.sta.policy.wifi_probe.blackhole_latched)
            {
                for (probe_index = 0U; probe_index < LINKG_TRANSPORT_CLASS_COUNT; probe_index++)
                {
                    probe = &input->observation.wifi.probe[probe_index];

                    if (!probe->valid || !probe->reachable || probe->updated_us == 0U ||
                        probe->updated_us > input->observation.collected_us ||
                        input->observation.collected_us - probe->updated_us > 2500000ULL)
                    {
                        probe_recovered = false;
                        break;
                    }
                }
            }

            // 非质量迁移保持原有强RSSI直接恢复条件。
            if (probe_recovered &&
                input->observation.wifi.radio.rssi_dbm >= LINKG_SWITCH_POLICY_DIRECT_RECOVERY_RSSI_DBM)
            {
                recovery_ready = true;
            }
        }

        /****************************** 七、检查严格恢复顺序 ******************************/

        // 高优先级业务未满足恢复条件时，不能继续选择低优先级业务。
        // 已经选中的高优先级业务仍然可以作为本轮恢复组提交。
        if (!recovery_ready)
        {
            break;
        }

        /****************************** 八、建立本轮恢复组 ******************************/

        if (class_mask == 0U)
        {
            // 首个满足条件的业务决定本轮恢复原因和事件类型。
            group_reason = operation->migration_reason;

            if (group_reason == LINKG_SWITCH_POLICY_MIGRATION_LOSS ||
                group_reason == LINKG_SWITCH_POLICY_MIGRATION_LOAD)
            {
                event_type = LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY;
            }
            else
            {
                event_type = LINKG_SWITCH_POLICY_EVENT_WIFI_DIRECT_RECOVERY;
            }
        }

        // 同原因且满足条件的低优先级业务一起加入本轮恢复组。
        class_mask |= LINKG_SWITCH_POLICY_CLASS_BIT(index);
    }

    pthread_mutex_unlock(&g_switch.lock);

    /****************************** 检查本轮恢复候选 ******************************/

    if (class_mask == 0U)
    {
        return 0;
    }

    /****************************** 交付恢复候选事件 ******************************/

    // 仅生成初始事件，不在Policy中直接修改发送计划。
    memset(&event, 0, sizeof(event));

    event.type                              = event_type;
    event.peer_node_id                      = input->peer_node_id;
    event.peer_generation                   = input->peer_generation;
    event.occurred_us                       = now_us;
    event.payload.recovery_ready.class_mask = class_mask;

    return linkg_switch_policy_operation_process_event(&event, now_us);
}
