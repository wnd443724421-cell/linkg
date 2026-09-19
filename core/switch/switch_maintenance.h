/**
 * @file switch_maintenance.h
 * @brief LinkG链路切换接入维护内部定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-19
 */

#ifndef SWITCH_MAINTENANCE_H
#define SWITCH_MAINTENANCE_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_link.h"

#include "switch_event.h"

#ifdef __cplusplus
extern "C"
{
#endif

/****************************** 本机维护运行状态 ******************************/

typedef struct
{
    bool                active;     // 当前本机是否存在正在执行的Access维护
    linkg_link_access_t access;     // 当前被本机禁止使用的Access
    uint32_t            message_id; // 当前本机Maintenance事务编号
    uint64_t            started_us; // 当前本机Maintenance开始时间
} linkg_switch_maintenance_local_runtime_t;

/****************************** 对端维护运行状态 ******************************/

typedef struct
{
    bool                active;          // 当前对端是否要求禁止使用指定Access
    linkg_link_access_t access;          // 当前对端正在维护的Access
    uint32_t            last_message_id; // 最近已经处理的Maintenance事务编号
    uint64_t            started_us;      // 当前有效远端Maintenance开始时间
    uint64_t            expires_us;      // 当前远端Maintenance最终保护超时时间
} linkg_switch_maintenance_remote_runtime_t;

/****************************** END发送运行状态 ******************************/

typedef struct
{
    bool                active;        // 当前是否正在等待该Peer确认Maintenance END
    linkg_link_access_t access;        // 当前等待确认的Maintenance Access
    uint32_t            message_id;    // 当前等待确认的Maintenance事务编号
    uint32_t            retry_count;   // 当前END累计发送次数
    uint64_t            next_retry_us; // 下一次END重试时间
    uint64_t            deadline_us;   // 当前END确认事务最终截止时间
} linkg_switch_maintenance_end_tx_runtime_t;

/****************************** Peer维护运行状态 ******************************/

typedef struct
{
    linkg_switch_maintenance_remote_runtime_t remote; // 当前对端Maintenance状态
    linkg_switch_maintenance_end_tx_runtime_t end_tx; // 当前本机END等待确认状态
} linkg_switch_maintenance_peer_runtime_t;

/****************************** 本机维护控制 ******************************/

int linkg_switch_maintenance_begin(linkg_link_access_t access);
int linkg_switch_maintenance_end(linkg_link_access_t access);

/****************************** 事件处理 ******************************/

int linkg_switch_maintenance_process_event(const linkg_switch_event_t *event, uint64_t now_us);

/****************************** 周期处理 ******************************/

int      linkg_switch_maintenance_process(uint64_t now_us);
uint64_t linkg_switch_maintenance_next_deadline_locked(void);

/****************************** 使用约束 ******************************/

bool linkg_switch_maintenance_access_blocked_locked(const linkg_switch_maintenance_peer_runtime_t *runtime, linkg_link_access_t access);

#ifdef __cplusplus
}
#endif

#endif
