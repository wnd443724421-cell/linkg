/**
 * @file cellular_fsm_internal.h
 * @brief LinkG蜂窝连接状态机分阶段实现共享接口
 */

#ifndef CELLULAR_FSM_INTERNAL_H
#define CELLULAR_FSM_INTERNAL_H

#include "cellular_fsm.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 通用状态辅助 ******************************/

bool                _cellular_fsm_meta_current(const cellular_status_meta_t *meta);
bool                _cellular_fsm_action_error_fatal(int error);
cellular_fsm_step_t _cellular_fsm_step_wait(void);
cellular_fsm_step_t _cellular_fsm_step_done(cellular_runtime_state_t next_state);
cellular_fsm_step_t _cellular_fsm_step_failed(int error);
cellular_fsm_step_t _cellular_fsm_step_fatal(int error);
void                _cellular_fsm_request_refresh(cellular_fsm_t *fsm, cellular_status_refresh_mask_t requested);

/****************************** 会话清理及Host校验 ******************************/

int  _cellular_fsm_cleanup_data_session(cellular_fsm_t *fsm, at_channel_t *channel);
void _cellular_fsm_cleanup_host_network(void);
bool _cellular_fsm_host_ready(const cellular_status_info_t *info);
void _cellular_fsm_host_ipv4_maintain(const cellular_status_info_t *info, uint64_t now_ms);
bool _cellular_fsm_ipv6_in_prefix(const struct in6_addr *address, const struct in6_addr *prefix, uint8_t prefix_length);

/****************************** SIM状态步骤 ******************************/

cellular_fsm_step_t _cellular_fsm_state_wait_sim(const cellular_fsm_t *fsm);
cellular_fsm_step_t _cellular_fsm_state_check_sim(const cellular_fsm_t *fsm, const linkg_cellular_config_t *config, const cellular_status_info_t *info, uint64_t now_ms);
cellular_fsm_step_t _cellular_fsm_state_enter_pin(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms);
cellular_fsm_step_t _cellular_fsm_state_wait_pin(const cellular_status_info_t *info);
cellular_fsm_step_t _cellular_fsm_state_wait_puk(const cellular_status_info_t *info);

/****************************** 网络注册状态步骤 ******************************/

cellular_fsm_step_t _cellular_fsm_state_wait_registration(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms);

/****************************** PDP状态步骤 ******************************/

cellular_fsm_step_t _cellular_fsm_state_prepare_pdp(cellular_fsm_t *fsm, const linkg_cellular_config_t *config, at_channel_t *channel, uint64_t now_ms);
cellular_fsm_step_t _cellular_fsm_state_activate_pdp(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms);
cellular_fsm_step_t _cellular_fsm_state_wait_pdp(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms);

/****************************** NETDEV状态步骤 ******************************/

cellular_fsm_step_t _cellular_fsm_state_start_netdev(cellular_fsm_t *fsm, at_channel_t *channel, uint64_t now_ms);
cellular_fsm_step_t _cellular_fsm_state_wait_netdev(const cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms);

/****************************** Host状态步骤 ******************************/

cellular_fsm_step_t _cellular_fsm_state_prepare_host(cellular_fsm_t *fsm, uint64_t now_ms);
cellular_fsm_step_t _cellular_fsm_state_wait_host(cellular_fsm_t *fsm, const cellular_status_info_t *info, uint64_t now_ms);

#ifdef __cplusplus
}
#endif

#endif
