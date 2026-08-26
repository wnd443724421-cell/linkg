/**
 * @file wifi_runtime.h
 * @brief LinkG Wi-Fi内部运行状态协调接口
 */

#ifndef WIFI_RUNTIME_H
#define WIFI_RUNTIME_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_device_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 连接状态 ******************************/

typedef enum
{
    WIFI_RUNTIME_LINK_NOT_APPLICABLE = 0, // 当前角色不使用STA连接状态
    WIFI_RUNTIME_LINK_DISCONNECTED,       // STA当前未连接目标AP
    WIFI_RUNTIME_LINK_CONNECTED           // STA当前已经连接目标AP
} wifi_runtime_link_state_t;

/****************************** 恢复状态 ******************************/

typedef enum
{
    WIFI_RUNTIME_RECOVERY_IDLE = 0, // 当前无需恢复
    WIFI_RUNTIME_RECOVERY_REQUIRED, // 已请求由Wi-Fi Owner执行恢复
    WIFI_RUNTIME_RECOVERY_RUNNING,  // Wi-Fi Owner正在执行恢复
    WIFI_RUNTIME_RECOVERY_FAILED    // 最近一次恢复失败
} wifi_runtime_recovery_state_t;

typedef enum
{
    WIFI_RUNTIME_RECOVERY_REASON_NONE = 0,         // 当前没有恢复原因
    WIFI_RUNTIME_RECOVERY_REASON_WPA_UNRESPONSIVE, // wpa_supplicant连续健康检查失败
    WIFI_RUNTIME_RECOVERY_REASON_EVENT_CHANNEL,    // WPA事件监听通道不可恢复
    WIFI_RUNTIME_RECOVERY_REASON_SCAN_STALLED,     // STA扫描流程连续异常
    WIFI_RUNTIME_RECOVERY_REASON_RADIO_CONTROL     // 无线参数控制连续失败
} wifi_runtime_recovery_reason_t;

/****************************** 运行事件 ******************************/

typedef enum
{
    WIFI_RUNTIME_EVENT_NONE = 0,          // 无运行事件
    WIFI_RUNTIME_EVENT_STA_CONNECTED,     // STA连接已经建立
    WIFI_RUNTIME_EVENT_STA_DISCONNECTED,  // STA连接已经断开
    WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED, // 请求Wi-Fi Owner执行恢复
    WIFI_RUNTIME_EVENT_RECOVERY_STARTED,  // Wi-Fi Owner开始执行恢复
    WIFI_RUNTIME_EVENT_RECOVERY_SUCCEEDED,// Wi-Fi Owner恢复完成
    WIFI_RUNTIME_EVENT_RECOVERY_FAILED    // Wi-Fi Owner恢复失败
} wifi_runtime_event_type_t;

typedef struct
{
    wifi_runtime_event_type_t      type;            // 运行事件类型
    wifi_runtime_recovery_reason_t recovery_reason; // 恢复原因，仅恢复相关事件使用
    int                            error;           // 恢复错误码，0表示无错误
} wifi_runtime_event_t;

/****************************** 运行状态 ******************************/

/**
 * @brief Wi-Fi模块内部逻辑运行状态。
 *
 * @note 仅允许network-wifi Owner线程修改；其他模块只能通过Owner递交事件，
 *       不得直接并发写入该结构体。
 */
typedef struct
{
    linkg_device_role_t            role;            // 当前设备角色
    wifi_runtime_link_state_t      link_state;      // STA逻辑连接状态
    wifi_runtime_recovery_state_t  recovery_state;  // Wi-Fi服务恢复状态
    wifi_runtime_recovery_reason_t recovery_reason; // 当前恢复原因
    uint64_t                       generation;      // 逻辑状态变化代数
    uint64_t                       updated_ms;      // 最近一次逻辑状态变化时间
    int                            recovery_error;  // 最近一次恢复错误码
} wifi_runtime_t;

/****************************** 生命周期 ******************************/

int  wifi_runtime_init(wifi_runtime_t *runtime, linkg_device_role_t role, uint64_t now_ms);
void wifi_runtime_reset(wifi_runtime_t *runtime, uint64_t now_ms);

/****************************** 事件处理 ******************************/

int wifi_runtime_apply_event(wifi_runtime_t *runtime, const wifi_runtime_event_t *event, uint64_t now_ms);

/****************************** 状态查询 ******************************/

bool wifi_runtime_sta_connected(const wifi_runtime_t *runtime);
bool wifi_runtime_recovery_required(const wifi_runtime_t *runtime);

#ifdef __cplusplus
}
#endif

#endif
