/**
 * @file cellular_fsm.h
 * @brief LinkG蜂窝网络连接状态机内部接口
 */

#ifndef CELLULAR_FSM_H
#define CELLULAR_FSM_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_cellular_config.h"

#include "at_channel.h"
#include "cellular_monitor.h"
#include "cellular_runtime.h"
#include "cellular_status.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 状态机结果 ******************************/

typedef struct
{
    cellular_runtime_step_result_t result;     // 当前状态处理结果
    cellular_runtime_state_t       next_state; // DONE时需要进入的后续状态
    int                            error;      // FAILED或FATAL时的错误码
} cellular_fsm_step_t;

/****************************** 状态机上下文 ******************************/

typedef struct
{
    cellular_runtime_t             runtime;                // Owner唯一运行状态
    cellular_status_refresh_mask_t requested_refresh;      // Owner下一轮需要强制确认的状态事实
    bool                           pdp_action_started;     // 当前SIM会话是否执行过PDP激活动作
    bool                           netdev_action_started;  // 当前SIM会话是否执行过QNETDEV启动动作
    bool                           verify_ipv4_done;       // 当前验证轮次IPv4是否已经通过
    bool                           verify_ipv6_done;       // 当前验证轮次IPv6是否已经通过
    uint32_t                       verify_failure_count;   // 当前地址族连续公网探测失败次数
} cellular_fsm_t;

/****************************** 生命周期 ******************************/

int  cellular_fsm_init(cellular_fsm_t *fsm, uint64_t now_ms);
void cellular_fsm_reset(cellular_fsm_t *fsm, uint64_t now_ms);

/****************************** Owner调度 ******************************/

uint64_t                       cellular_fsm_get_deadline(const cellular_fsm_t *fsm);
cellular_status_refresh_mask_t cellular_fsm_take_requested_refresh(cellular_fsm_t *fsm);
int                            cellular_fsm_enter(cellular_fsm_t *fsm, cellular_runtime_state_t state, uint64_t now_ms);
int                            cellular_fsm_sync_sim_session(cellular_fsm_t *fsm, at_channel_t *channel, const cellular_monitor_events_t *events, const cellular_status_info_t *info, uint64_t now_ms);
cellular_fsm_step_t            cellular_fsm_run(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, const cellular_status_info_t *info, uint64_t now_ms);
int                            cellular_fsm_handle_failure(cellular_fsm_t *fsm, at_channel_t *channel, int error, uint64_t now_ms);
int                            cellular_fsm_stop_session(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
