/**
 * @file switch_policy_event.h
 * @brief LinkG三类业务切换策略的初始事件及事务后续事件
 */

#ifndef SWITCH_POLICY_EVENT_H
#define SWITCH_POLICY_EVENT_H

#include <stdint.h>

#include "linkg_transport_types.h"

/****************************** 事件常量 ******************************/

#define LINKG_SWITCH_POLICY_OPERATION_ID_INVALID 0U // 无活动逻辑事务

#define LINKG_SWITCH_POLICY_CLASS_BIT(traffic_class) (1U << (uint32_t)(traffic_class))
#define LINKG_SWITCH_POLICY_CLASS_MASK_ALL         ((1U << LINKG_TRANSPORT_CLASS_COUNT) - 1U)

/****************************** 策略事件类型 ******************************/

/**
 * 初始事件不携带operation_id，由Operation根据当前Plan、phase及资源约束
 * 决定是否接受事件并创建事务。
 *
 * 后续事件必须关联已有operation_id，不能创建新事务。
 * PlanSync ACK/失败结果使用独立的on_plan_result接口，不属于此枚举。
 */
typedef enum
{
    LINKG_SWITCH_POLICY_EVENT_NONE = 0,       // 无效事件

    // 初始事件：请求创建或抢占切换事务
    LINKG_SWITCH_POLICY_EVENT_WIFI_DOWN,      // Wi-Fi明确硬故障，处理受影响业务
    LINKG_SWITCH_POLICY_EVENT_WIFI_LOSS_BAD,  // Wi-Fi无线丢包异常，申请冗余质量验证
    LINKG_SWITCH_POLICY_EVENT_WIFI_LOAD_BAD,  // Wi-Fi队列溢出，申请单类业务迁移
    LINKG_SWITCH_POLICY_EVENT_RECOVERY_READY, // 5G业务满足Wi-Fi恢复候选条件

    // 后续事件：仅推进已存在的逻辑事务
    LINKG_SWITCH_POLICY_EVENT_LOAD_TIMEOUT,   // LOAD冗余计时到期
    LINKG_SWITCH_POLICY_EVENT_VERIFY_DONE,    // LOSS/RECOVERY质量验证完成

    LINKG_SWITCH_POLICY_EVENT_COUNT           // 策略事件类型数量
} linkg_switch_policy_event_type_t;

/****************************** 质量验证结果 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_VERIFY_NONE = 0,      // 尚无结果，不应作为完成结果提交
    LINKG_SWITCH_POLICY_VERIFY_WIFI,          // Wi-Fi验证更优
    LINKG_SWITCH_POLICY_VERIFY_CELLULAR,      // Cellular验证更优
    LINKG_SWITCH_POLICY_VERIFY_INCONCLUSIVE   // 样本不足或结果不确定
} linkg_switch_policy_verify_result_t;

/****************************** 策略事件 ******************************/

/**
 * @brief 单次策略事件。
 *
 * peer_generation阻止旧Peer事件作用于重建的Peer；
 * 后续事件的operation_id进一步阻止旧事务结果推进新事务。
 * 单个Peer的operation_id必须在三个业务槽位之间唯一。
 */
typedef struct
{
    linkg_switch_policy_event_type_t type;            // 事件类型
    uint8_t                          peer_node_id;    // 直接AP Peer节点编号
    uint32_t                         peer_generation; // Peer运行代际
    uint64_t                         occurred_us;     // 事件生成的单调时间

    union
    {
        struct
        {
            uint32_t class_mask; // 本轮满足恢复候选条件的业务集合
        } recovery_ready;

        struct
        {
            uint32_t operation_id; // 计时到期对应的LOAD逻辑事务
        } load_timeout;

        struct
        {
            uint32_t                            operation_id; // 比较结果对应的逻辑事务
            linkg_switch_policy_verify_result_t result;       // 质量比较结果
        } verify_done;

    } payload;

} linkg_switch_policy_event_t;

#endif // SWITCH_POLICY_EVENT_H
