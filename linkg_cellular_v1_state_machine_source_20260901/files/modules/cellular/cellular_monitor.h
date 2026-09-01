/**
 * @file cellular_monitor.h
 * @brief LinkG蜂窝网络异步事件监控接口
 */

#ifndef CELLULAR_MONITOR_H
#define CELLULAR_MONITOR_H

#include <stdbool.h>
#include <stdint.h>

#include "at_channel.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 事件标志 ******************************/

typedef uint32_t cellular_monitor_event_mask_t; // 蜂窝异步事件标志集合

#define CELLULAR_MONITOR_EVENT_NONE                   0U               // 当前没有待处理异步事件
#define CELLULAR_MONITOR_EVENT_SIM_PRESENCE_CHANGED   (1U << 0)        // SIM物理插拔状态发生变化
#define CELLULAR_MONITOR_EVENT_SIM_STATE_CHANGED      (1U << 1)        // SIM逻辑状态发生变化
#define CELLULAR_MONITOR_EVENT_REGISTRATION_CHANGED   (1U << 2)        // 移动网络注册状态发生变化
#define CELLULAR_MONITOR_EVENT_RADIO_CHANGED          (1U << 3)        // 服务网络或无线质量发生变化
#define CELLULAR_MONITOR_EVENT_PDP_CHANGED            (1U << 4)        // PDP数据会话状态发生变化
#define CELLULAR_MONITOR_EVENT_NETDEV_CHANGED         (1U << 5)        // USB网络设备连接状态发生变化
#define CELLULAR_MONITOR_EVENT_MODEM_FUNCTION_CHANGED (1U << 6)        // Modem功能状态发生变化
#define CELLULAR_MONITOR_EVENT_MODEM_POWERED_DOWN     (1U << 7)        // Modem已经进入掉电状态
#define CELLULAR_MONITOR_EVENT_ALL                    ((1U << 8) - 1U) // 全部受支持蜂窝异步事件

/****************************** SIM物理状态 ******************************/

typedef enum
{
    CELLULAR_MONITOR_SIM_PRESENCE_UNKNOWN = 0, // SIM物理插拔状态未知
    CELLULAR_MONITOR_SIM_PRESENCE_REMOVED,     // SIM当前已经拔出
    CELLULAR_MONITOR_SIM_PRESENCE_INSERTED     // SIM当前已经插入
} cellular_monitor_sim_presence_t;

/****************************** 事件快照 ******************************/

typedef struct
{
    cellular_monitor_event_mask_t   mask;                    // 本次合并取得的待处理事件集合
    cellular_monitor_sim_presence_t sim_presence;            // 最近一次QSIMSTAT提供的SIM物理状态
    uint64_t                        generation;              // 已识别异步事件累计代数
    uint64_t                        updated_ms;              // 最近一次已识别异步事件时间
    uint64_t                        sim_presence_updated_ms; // 最近一次QSIMSTAT事件时间
    bool                            sim_presence_valid;      // 最近一次QSIMSTAT是否提供确定插拔状态
} cellular_monitor_events_t;

/****************************** 生命周期 ******************************/

int  cellular_monitor_init(void);
int  cellular_monitor_start(at_channel_t *channel);
int  cellular_monitor_stop(void);
void cellular_monitor_deinit(void);

/****************************** 事件读取 ******************************/

int cellular_monitor_get_event_fd(void);
int cellular_monitor_take_events(cellular_monitor_events_t *events);

#ifdef __cplusplus
}
#endif

#endif
