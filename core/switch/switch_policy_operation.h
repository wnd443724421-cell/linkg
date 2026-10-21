#include <stdint.h>

#include "switch_plan.h"
#include "switch_policy_event.h"

/****************************** 策略事件处理 ******************************/

int linkg_switch_policy_operation_process_event(const linkg_switch_policy_event_t *event, uint64_t now_us);

/****************************** Plan同步结果 ******************************/

int linkg_switch_policy_operation_on_plan_result(const linkg_switch_plan_sync_result_t *result);

/****************************** 周期处理 ******************************/

int      linkg_switch_policy_operation_process(uint64_t now_us);
uint64_t linkg_switch_policy_operation_next_deadline_locked(void);

/****************************** 生命周期 ******************************/

void linkg_switch_policy_operation_reset_locked(uint8_t peer_node_id, uint32_t peer_generation);
