
/**
 * @file switch_policy_operation.h
 * @brief LinkG业务切换策略事务管理接口
 */

#ifndef SWITCH_POLICY_OPERATION_H
#define SWITCH_POLICY_OPERATION_H

#include <stdint.h>

#include "switch_policy_event.h"

/****************************** Plan同步结果 ******************************/

typedef enum
{
    LINKG_SWITCH_POLICY_PLAN_RESULT_NONE = 0,    // 无有效同步结果
    LINKG_SWITCH_POLICY_PLAN_RESULT_CONFIRMED,   // 完整Plan已经得到远端确认
    LINKG_SWITCH_POLICY_PLAN_RESULT_REJECTED,    // 远端最终拒绝该Plan
    LINKG_SWITCH_POLICY_PLAN_RESULT_UNCERTAIN    // 无法确认远端Plan状态
} linkg_switch_policy_plan_result_t;

/****************************** 策略事件处理 ******************************/

int linkg_switch_policy_operation_process_event(const linkg_switch_policy_event_t *event, uint64_t now_us);

/****************************** Plan同步通知 ******************************/

int  linkg_switch_policy_operation_plan_started_locked(uint8_t peer_node_id, uint32_t peer_generation, uint32_t class_mask, uint32_t message_id, uint64_t now_us);
void linkg_switch_policy_operation_plan_result_locked(uint8_t peer_node_id, uint32_t peer_generation, uint32_t message_id, linkg_switch_policy_plan_result_t result, uint64_t now_us);

/****************************** 周期处理 ******************************/

int      linkg_switch_policy_operation_process(uint64_t now_us);
uint64_t linkg_switch_policy_operation_next_deadline_locked(void);

/****************************** 生命周期 ******************************/

void linkg_switch_policy_operation_reset_locked(uint8_t peer_node_id, uint32_t peer_generation);

#endif
