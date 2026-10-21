/**
 * @file switch_policy_event.h
 * @brief LinkG业务链路分配策略事件定义
 */

#ifndef SWITCH_POLICY_EVENT_H
#define SWITCH_POLICY_EVENT_H

#include <stdint.h>

#include "linkg_transport_types.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_POLICY_OPERATION_ID_INVALID       0U // 无效逻辑事务编号

#define LINKG_SWITCH_POLICY_CLASS_BIT(traffic_class)  (1U << (uint32_t)(traffic_class))

#define LINKG_SWITCH_POLICY_CLASS_MASK_ALL  ((1U << LINKG_TRANSPORT_CLASS_COUNT) - 1U)

/****************************** 策略事件类型 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_EVENT_NONE = 0,      // 无效事件

    LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN,     // Wi-Fi硬故障，需要立即迁移受影响业务
    LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD, // Wi-Fi无线丢包异常，触发整组冗余验证
    LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD, // Wi-Fi队列溢出，触发单类冗余迁移

    LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY, // 5G业务满足Wi-Fi恢复候选条件
    LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT,   // LOAD冗余计时到期
    LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE,    // LOSS或RECOVERY质量验证完成

    LINKG_SWITCH_POLICY_EVENT_COUNT           // 策略事件类型数量
} linkg_switch_policy_event_type_t;

/****************************** 质量验证结果 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_NONE = 0,      // 尚无有效验证结果
    LINKG_SWITCH_POLICY_VERIFY_WIFI,          // 验证结果选择Wi-Fi
    LINKG_SWITCH_POLICY_VERIFY_CELLULAR,      // 验证结果选择Cellular
    LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE   // 样本不足或无法形成可靠判决
} linkg_switch_policy_verify_result_t;

/****************************** 策略事件 ******************************/

typedef struct
{
    linkg_switch_policy_event_type_t type;            // 当前策略事件类型
    uint8_t                          peer_node_id;    // 当前直接AP节点编号
    uint32_t                         peer_generation; // 事件对应的Peer运行代际
    uint64_t                         occurred_us;     // 事件产生时的单调时间

    union
    {
        struct
        {
            uint32_t class_mask; // 本轮符合恢复条件的5G业务候选集合
        } recovery_ready;

        struct
        {
            uint32_t operation_id; // LOAD计时器所属逻辑事务
        } load_timeout;

        struct
        {
            uint32_t                            operation_id; // 质量比较所属逻辑事务
            linkg_switch_policy_verify_result_t result;       // 最终验证结果
        } verify_done;

    } payload;

} linkg_switch_policy_event_t;

#endif
