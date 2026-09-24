/**
 * @file switch_policy.h
 * @brief LinkG链路切换策略内部接口
 */

#ifndef SWITCH_POLICY_H
#define SWITCH_POLICY_H

#include <stdint.h>

/****************************** 策略检查 ******************************/

int      linkg_switch_policy_process(uint64_t now_us);
uint64_t linkg_switch_policy_next_deadline_locked(void);

#endif
