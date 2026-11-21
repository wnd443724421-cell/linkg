/**
 * @file cellular_observation.c
 * @brief 小区变化日志与公网探测异常记录
 */

#include "cellular_observation.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "linkg_log.h"

/****************************** 日志时间 ******************************/

/**
 * @brief 记录系统日历时间，时长计算仍使用调用者传入的单调时钟。
 */
void cellular_observation_wall_time(char *buffer, size_t size)
{
    struct tm local;
    time_t    now;

    now = time(NULL);
    if (localtime_r(&now, &local) == NULL ||
        strftime(buffer, size, "%Y-%m-%dT%H:%M:%S%z", &local) == 0U)
    {
        (void)snprintf(buffer, size, "unknown");
    }
}

/****************************** 公网探测记录 ******************************/

/**
 * @brief 每个地址族的首轮失败开始计时，首轮成功结束；设施错误保留异常历史。
 */
cellular_observation_event_t cellular_observation_update_probe(linkg_cellular_probe_history_t *history, int result, uint64_t now_ms)
{
    history->updated_ms = now_ms;
    history->valid = result >= 0;

    if (result < 0)
    {
        return CELLULAR_OBSERVATION_NONE;
    }

    if (result == 0)
    {
        if (!history->active)
        {
            history->active       = true;
            history->started_ms   = now_ms;
            history->recovered_ms = 0U;
            history->duration_ms  = 0U;
            return CELLULAR_OBSERVATION_FAILED;
        }

        return CELLULAR_OBSERVATION_NONE;
    }

    if (history->active)
    {
        history->active       = false;
        history->recovered_ms = now_ms;
        history->duration_ms  = now_ms >= history->started_ms ? now_ms - history->started_ms : 0U;
        return CELLULAR_OBSERVATION_RECOVERED;
    }

    return CELLULAR_OBSERVATION_NONE;
}

/****************************** 小区观测日志 ******************************/

/**
 * @brief 仅在新成功采样确认小区身份变化时记录事实，采集失败不等同于无服务。
 */
void cellular_observation_log_cell(const cellular_status_network_info_t *previous, const cellular_status_network_info_t *current)
{
    char wall_time[40];
    bool had_cell;
    bool has_cell;
    bool same_cell;

    if (!current->radio_meta.confirmed || current->radio_meta.last_error != 0 ||
        current->radio_meta.updated_ms == previous->radio_meta.updated_ms)
    {
        return;
    }

    had_cell = previous->serving_cell_valid && previous->cell_id_valid;
    has_cell = current->serving_cell_valid && current->cell_id_valid;
    same_cell = had_cell && has_cell &&
                previous->network_type == current->network_type &&
                strcmp(previous->mcc, current->mcc) == 0 &&
                strcmp(previous->mnc, current->mnc) == 0 &&
                strcmp(previous->cell_id, current->cell_id) == 0;

    if (same_cell || (!had_cell && !has_cell))
    {
        return;
    }

    cellular_observation_wall_time(wall_time, sizeof(wall_time));
    LINKG_LOG_INFO("CELL-OBSERVE: wall=%s old_rat=%u old_plmn=%s%s old_cell=%s old_seen_ms=%llu new_rat=%u new_plmn=%s%s new_cell=%s observed_ms=%llu rsrp=%d rsrp_valid=%d",
                   wall_time,
                   (unsigned int)previous->network_type,
                   previous->mcc, previous->mnc,
                   had_cell ? previous->cell_id : "unknown",
                   (unsigned long long)previous->radio_meta.updated_ms,
                   (unsigned int)current->network_type,
                   current->mcc, current->mnc,
                   has_cell ? current->cell_id : "unknown",
                   (unsigned long long)current->radio_meta.updated_ms,
                   current->rsrp_dbm, current->rsrp_valid ? 1 : 0);
}
