/**
 * @file switch_plan.h
 * @brief LinkG链路切换发送计划内部接口
 */

#ifndef SWITCH_PLAN_H
#define SWITCH_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_switch.h"

#include "switch_event.h"
#include "switch_wire.h"

/****************************** 同步状态 ******************************/

typedef struct
{
    bool                          active;           // 当前是否存在远端发送计划同步事务
    bool                          recovery_pending; // 是否等待有限次数的退避恢复
    bool                          confirmed;        // 当前完整计划是否收到成功ACK
    bool                          remote_rejected;  // 最近失败是否为明确的远端拒绝，而非本地等待超时
    uint32_t                      message_id;       // 当前同步事务消息编号
    linkg_switch_wire_plan_sync_t wire_plan;        // 当前正在同步的逻辑发送计划
    uint32_t                      retry_count;      // 当前发送尝试次数
    uint32_t                      recovery_count;   // 当前计划已启动的恢复事务次数
    uint64_t                      next_retry_us;    // 下一次同步重试时间
    uint64_t                      deadline_us;      // 当前同步事务最终截止时间
    int32_t                       last_status;      // 最近一次ACK或本地超时结果
} linkg_switch_plan_sync_runtime_t;

/****************************** 计划管理 ******************************/

int linkg_switch_plan_set(uint8_t peer_node_id, const linkg_send_plan_t *plan);
int linkg_switch_plan_get(uint8_t peer_node_id, linkg_send_plan_t *plan);
int linkg_switch_plan_remove(uint8_t peer_node_id);
int linkg_switch_plan_apply_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan);
int linkg_switch_plan_commit_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan, uint64_t now_us);
int linkg_switch_plan_update_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, bool synchronize, uint64_t now_us);

/****************************** 事件处理 ******************************/

int linkg_switch_plan_process_event(const linkg_switch_event_t *event, uint64_t now_us);

/****************************** 周期处理 ******************************/

int      linkg_switch_plan_process(uint64_t now_us);
uint64_t linkg_switch_plan_next_deadline_locked(void);

#endif
