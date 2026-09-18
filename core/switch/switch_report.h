/**
 * @file switch_report.h
 * @brief LinkG链路切换Wi-Fi质量上报内部接口
 */

#ifndef SWITCH_REPORT_H
#define SWITCH_REPORT_H

#include <stdint.h>

#include "switch_wire.h"

/****************************** 周期处理 ******************************/

int      linkg_switch_report_process(uint64_t now_us);
uint64_t linkg_switch_report_next_deadline_locked(void);

/****************************** 数据接收 ******************************/

int linkg_switch_report_receive_wifi_quality(uint8_t peer_node_id, uint32_t message_id, const linkg_switch_wire_wifi_quality_report_t *report);

#endif
