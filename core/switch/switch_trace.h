/**
 * @file switch_trace.h
 * @brief STA Wi-Fi质量诊断文本记录接口（测试用途）
 */
#ifndef SWITCH_TRACE_H
#define SWITCH_TRACE_H

#include "switch_observation.h"

// 仅由STA Switch Worker在Observation成功发布后调用。
void linkg_switch_trace_record(const linkg_switch_observation_t *observation);

#endif // SWITCH_TRACE_H
