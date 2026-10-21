/**
 * @file switch_plan.h
 * @brief LinkG完整发送计划管理与PlanSync协议接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-21
 */

#ifndef SWITCH_PLAN_H
#define SWITCH_PLAN_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_switch.h"

#include "switch_event.h"
#include "switch_wire.h"

/****************************** Plan同步状态 ******************************/

typedef enum
{
    LINKG_SWITCH_PLAN_SYNC_IDLE = 0,   // 无正在进行的同步事务
    LINKG_SWITCH_PLAN_SYNC_WAIT_ACK,   // 等待当前PLAN_SYNC确认
    LINKG_SWITCH_PLAN_SYNC_RETRY_WAIT, // 等待下一轮协议恢复
    LINKG_SWITCH_PLAN_SYNC_CONFIRMED,  // 已获得远端确认
    LINKG_SWITCH_PLAN_SYNC_REJECTED,   // 恢复耗尽，远端拒绝
    LINKG_SWITCH_PLAN_SYNC_UNCERTAIN,  // 恢复耗尽，远端状态未知
    LINKG_SWITCH_PLAN_SYNC_CANCELLED   // 已被更新的本地Plan替换
} linkg_switch_plan_sync_state_t;

/****************************** 最近一次同步结果 ******************************/

typedef struct
{
    uint8_t                        peer_node_id;     // 目标AP节点编号
    uint32_t                       peer_generation;  // 目标Peer代际
    uint32_t                       sync_id;          // 本地逻辑同步事务编号
    uint32_t                       message_id;       // 最后一条相关Wire消息编号
    linkg_switch_plan_sync_state_t state;            // 终态：确认、拒绝、不确定或取消
    int32_t                        status;           // ACK状态或本地错误码
    uint64_t                       updated_us;       // 结果形成的单调时间
} linkg_switch_plan_sync_result_t;

/****************************** STA侧Plan同步运行状态 ******************************/

typedef struct
{
    // 当前同步事务标识
    uint32_t                        sync_id;         // 本地逻辑Plan同步编号，恢复轮次不变化
    uint32_t                        message_id;      // 当前PLAN_SYNC协议消息编号

    // 当前协议事务
    linkg_switch_plan_sync_state_t  state;           // 当前同步阶段或最近终态
    linkg_switch_wire_plan_sync_t   wire_plan;       // 当前要同步的完整三业务Plan
    int32_t                         last_status;     // 最近一次ACK或本地处理状态

    // 协议重试
    uint32_t                        retry_count;     // 当前消息发送尝试次数
    uint32_t                        recovery_count;  // 新Message ID恢复轮次数
    uint64_t                        next_retry_us;   // 下次发送/恢复时间
    uint64_t                        deadline_us;     // 当前消息轮次协议截止时间

    // 结果查询与Worker通知
    linkg_switch_plan_sync_result_t last_result;     // 最近一次完整同步的终态结果
    bool                            result_pending;  // 有新结果待Worker消费（最新结果覆盖旧结果）
} linkg_switch_plan_sync_runtime_t;

/****************************** 计划管理 ******************************/

int linkg_switch_plan_set(uint8_t peer_node_id, const linkg_send_plan_t *plan);
int linkg_switch_plan_get(uint8_t peer_node_id, linkg_send_plan_t *plan);
int linkg_switch_plan_remove(uint8_t peer_node_id);
int linkg_switch_plan_apply_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan);
int linkg_switch_plan_commit_local(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *plan, uint64_t now_us);
int linkg_switch_plan_update_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, bool synchronize, uint64_t now_us);

/**
 * @brief 条件提交完整Plan并获取稳定sync_id。
 * @note 返回0表示本地计划和协议重试事务已成功建立，不代表远端ACK。
 */
int linkg_switch_plan_submit_if_current(uint8_t peer_node_id, uint32_t peer_generation, const linkg_send_plan_t *previous, const linkg_send_plan_t *plan, uint64_t now_us, uint32_t *sync_id);

/****************************** 同步状态和结果 ******************************/

int linkg_switch_plan_sync_get(uint8_t peer_node_id, uint32_t peer_generation, linkg_switch_plan_sync_runtime_t *snapshot);
int linkg_switch_plan_take_result(linkg_switch_plan_sync_result_t *result);

/****************************** 事件处理 ******************************/

int linkg_switch_plan_process_event(const linkg_switch_event_t *event, uint64_t now_us);

/****************************** 周期处理 ******************************/

int      linkg_switch_plan_process(uint64_t now_us);
uint64_t linkg_switch_plan_next_deadline_locked(void);

#endif
