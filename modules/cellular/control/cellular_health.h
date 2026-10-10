/**
 * @file cellular_health.h
 * @brief LinkG蜂窝ONLINE维护与双栈公网健康检测接口
 */

#ifndef CELLULAR_HEALTH_H
#define CELLULAR_HEALTH_H

#include <stdbool.h>
#include <stdint.h>

#include "cellular_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 健康状态 ******************************/

typedef struct
{
    bool     ipv4_valid;          // IPv4公网状态是否已完成可靠判定
    bool     ipv4_available;      // IPv4最近确认可达，短暂失败保持已有成功结果
    bool     ipv6_valid;          // IPv6公网状态是否已完成可靠判定
    bool     ipv6_available;      // IPv6最近确认可达，短暂失败保持已有成功结果
    uint32_t ipv4_failed_rounds;  // IPv4连续完整失败轮数
    uint32_t ipv6_failed_rounds;  // IPv6连续完整失败轮数
    uint64_t updated_ms;          // 最近一轮双栈检测完成时间
} cellular_health_info_t;

/****************************** 生命周期 ******************************/

int  cellular_health_init(void);
int  cellular_health_start(void);
int  cellular_health_stop(void);
void cellular_health_deinit(void);

/****************************** 状态协调 ******************************/

void cellular_health_set_online(bool online);
void cellular_health_reset_session(void);
int  cellular_health_get_info(cellular_health_info_t *info);
bool cellular_health_dual_unreachable(void);

/****************************** ONLINE状态维护 ******************************/

cellular_fsm_step_t _cellular_fsm_state_online(cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
