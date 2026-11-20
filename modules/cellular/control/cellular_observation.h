/**
 * @file cellular_observation.h
 * @brief 小区变化日志与公网探测异常记录
 */

#ifndef CELLULAR_OBSERVATION_H
#define CELLULAR_OBSERVATION_H

#include <stddef.h>
#include <stdint.h>

#include "cellular_status.h"

typedef enum
{
    CELLULAR_OBSERVATION_NONE = 0,
    CELLULAR_OBSERVATION_FAILED,
    CELLULAR_OBSERVATION_RECOVERED
} cellular_observation_event_t;

cellular_observation_event_t cellular_observation_update_probe(linkg_cellular_probe_history_t *history, int result, uint64_t now_ms);
void cellular_observation_log_cell(const cellular_status_network_info_t *previous, const cellular_status_network_info_t *current);
void cellular_observation_wall_time(char *buffer, size_t size);

#endif
