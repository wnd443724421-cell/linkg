/**
 * @file switch_event.h
 * @brief LinkG链路切换内部事件接口
 */

#ifndef SWITCH_EVENT_H
#define SWITCH_EVENT_H

#include <stdbool.h>
#include <stdint.h>

#include "switch_wire.h"

/****************************** 模块常量 ******************************/

#define LINKG_SWITCH_EVENT_QUEUE_CAPACITY  64U // Switch内部控制事件队列容量

/****************************** 事件类型 ******************************/

typedef enum
{
    LINKG_SWITCH_EVENT_NONE         = 0, // 无效事件
    LINKG_SWITCH_EVENT_PLAN_SYNC_RX,     // 收到STA发送计划同步
    LINKG_SWITCH_EVENT_PLAN_ACK_RX,      // 收到AP发送计划确认
    LINKG_SWITCH_EVENT_COUNT             // 事件类型数量
} linkg_switch_event_type_t;

/****************************** 事件消息 ******************************/

typedef struct
{
    linkg_switch_event_type_t type;         // 内部事件类型
    uint8_t                   peer_node_id; // 消息来源直接Peer节点编号
    uint32_t                  message_id;   // Switch Wire消息编号

    union
    {
        linkg_switch_wire_plan_sync_t plan_sync; // 收到的发送计划同步
        linkg_switch_wire_plan_ack_t  plan_ack;  // 收到的发送计划确认
    } payload;
} linkg_switch_event_t;

/****************************** 事件队列 ******************************/

typedef struct
{
    linkg_switch_event_t items[LINKG_SWITCH_EVENT_QUEUE_CAPACITY];// 待Worker处理的控制事件
    uint32_t             head;                                    // 当前出队位置
    uint32_t             tail;                                    // 当前入队位置
    uint32_t             count;                                   // 当前待处理事件数量
} linkg_switch_event_queue_t;

/****************************** 事件操作 ******************************/

int  linkg_switch_event_post(const linkg_switch_event_t *event);
bool linkg_switch_event_pop_locked(linkg_switch_event_t *event);
void linkg_switch_event_reset_locked(void);

#endif
