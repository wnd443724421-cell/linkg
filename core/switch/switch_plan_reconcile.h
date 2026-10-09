/**
 * @file switch_plan_reconcile.h
 * @brief LinkG发送计划资源一致性校准内部接口
 */

#ifndef SWITCH_PLAN_RECONCILE_H
#define SWITCH_PLAN_RECONCILE_H

#include <stdint.h>

/****************************** 周期处理 ******************************/

int linkg_switch_plan_reconcile_process(uint64_t now_us);

#endif
