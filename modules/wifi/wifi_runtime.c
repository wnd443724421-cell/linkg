/**
 * @file wifi_runtime.c
 * @brief LinkG Wi-Fi内部运行状态协调实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#include "wifi_runtime.h"

#include <errno.h>
#include <string.h>

/****************************** 内部辅助 ******************************/

/**
 * @brief 获取指定设备角色对应的初始连接状态。
 */
static wifi_runtime_link_state_t _wifi_runtime_initial_link_state(linkg_device_role_t role)
{
    if (role == LINKG_DEVICE_ROLE_STA)
    {
        return WIFI_RUNTIME_LINK_DISCONNECTED;
    }

    return WIFI_RUNTIME_LINK_NOT_APPLICABLE;
}

/**
 * @brief 提交一次实际发生变化的运行状态。
 */
static void _wifi_runtime_commit(wifi_runtime_t *runtime, uint64_t now_ms)
{
    runtime->generation++;
    runtime->updated_ms = now_ms;
}

/**
 * @brief 处理STA连接状态变化事件。
 */
static int _wifi_runtime_apply_link_event(wifi_runtime_t *runtime, wifi_runtime_event_type_t type, uint64_t now_ms)
{
    wifi_runtime_link_state_t next_state;

    if (runtime->role != LINKG_DEVICE_ROLE_STA)
    {
        return -EOPNOTSUPP;
    }

    switch (type)
    {
        case WIFI_RUNTIME_EVENT_STA_CONNECTED:
            next_state = WIFI_RUNTIME_LINK_CONNECTED;
            break;

        case WIFI_RUNTIME_EVENT_STA_DISCONNECTED:
            next_state = WIFI_RUNTIME_LINK_DISCONNECTED;
            break;

        default:
            return -EINVAL;
    }

    if (runtime->link_state == next_state)
    {
        return 0;
    }

    runtime->link_state = next_state;
    _wifi_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 记录Wi-Fi服务恢复请求。
 */
static int _wifi_runtime_apply_recovery_required(wifi_runtime_t *runtime, const wifi_runtime_event_t *event, uint64_t now_ms)
{
    if (event->recovery_reason == WIFI_RUNTIME_RECOVERY_REASON_NONE)
    {
        return -EINVAL;
    }

    if (runtime->recovery_state != WIFI_RUNTIME_RECOVERY_IDLE)
    {
        return runtime->recovery_state == WIFI_RUNTIME_RECOVERY_REQUIRED ? 0 : -EBUSY;
    }

    runtime->recovery_state  = WIFI_RUNTIME_RECOVERY_REQUIRED;
    runtime->recovery_reason = event->recovery_reason;
    runtime->recovery_error  = event->error;

    _wifi_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 标记Wi-Fi Owner已经开始执行恢复。
 */
static int _wifi_runtime_apply_recovery_started(wifi_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime->recovery_state != WIFI_RUNTIME_RECOVERY_REQUIRED)
    {
        return -EPROTO;
    }

    runtime->recovery_state = WIFI_RUNTIME_RECOVERY_RUNNING;

    _wifi_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 标记Wi-Fi Owner恢复成功。
 */
static int _wifi_runtime_apply_recovery_succeeded(wifi_runtime_t *runtime, uint64_t now_ms)
{
    if (runtime->recovery_state != WIFI_RUNTIME_RECOVERY_RUNNING)
    {
        return -EPROTO;
    }

    runtime->recovery_state  = WIFI_RUNTIME_RECOVERY_IDLE;
    runtime->recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_NONE;
    runtime->recovery_error  = 0;

    _wifi_runtime_commit(runtime, now_ms);

    return 0;
}

/**
 * @brief 标记Wi-Fi Owner恢复失败。
 */
static int _wifi_runtime_apply_recovery_failed(wifi_runtime_t *runtime, const wifi_runtime_event_t *event, uint64_t now_ms)
{
    if (runtime->recovery_state != WIFI_RUNTIME_RECOVERY_RUNNING)
    {
        return -EPROTO;
    }

    runtime->recovery_state = WIFI_RUNTIME_RECOVERY_FAILED;
    runtime->recovery_error = event->error != 0 ? event->error : -EIO;

    _wifi_runtime_commit(runtime, now_ms);

    return 0;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi内部运行状态。
 */
int wifi_runtime_init(wifi_runtime_t *runtime, linkg_device_role_t role, uint64_t now_ms)
{
    if (runtime == NULL)
    {
        return -EINVAL;
    }

    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    memset(runtime, 0, sizeof(*runtime));

    runtime->role            = role;
    runtime->link_state      = _wifi_runtime_initial_link_state(role);
    runtime->recovery_state  = WIFI_RUNTIME_RECOVERY_IDLE;
    runtime->recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_NONE;
    runtime->generation      = 1U;
    runtime->updated_ms      = now_ms;

    return 0;
}

/**
 * @brief 重置Wi-Fi内部动态运行状态并保留当前设备角色。
 */
void wifi_runtime_reset(wifi_runtime_t *runtime, uint64_t now_ms)
{
    linkg_device_role_t role;

    if (runtime == NULL)
    {
        return;
    }

    role = runtime->role;

    memset(runtime, 0, sizeof(*runtime));

    runtime->role            = role;
    runtime->link_state      = _wifi_runtime_initial_link_state(role);
    runtime->recovery_state  = WIFI_RUNTIME_RECOVERY_IDLE;
    runtime->recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_NONE;
    runtime->generation      = 1U;
    runtime->updated_ms      = now_ms;
}

/****************************** 事件处理 ******************************/

/**
 * @brief 将一个语义运行事件应用到Wi-Fi内部运行状态。
 *
 * @note 本函数只修改逻辑状态，不执行扫描、驱动配置、服务重启等副作用；
 *       具体动作由network-wifi Owner根据事件显式编排。
 */
int wifi_runtime_apply_event(wifi_runtime_t *runtime, const wifi_runtime_event_t *event, uint64_t now_ms)
{
    if (runtime == NULL || event == NULL)
    {
        return -EINVAL;
    }

    switch (event->type)
    {
        case WIFI_RUNTIME_EVENT_STA_CONNECTED:
        case WIFI_RUNTIME_EVENT_STA_DISCONNECTED:
            return _wifi_runtime_apply_link_event(runtime, event->type, now_ms);

        case WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED:
            return _wifi_runtime_apply_recovery_required(runtime, event, now_ms);

        case WIFI_RUNTIME_EVENT_RECOVERY_STARTED:
            return _wifi_runtime_apply_recovery_started(runtime, now_ms);

        case WIFI_RUNTIME_EVENT_RECOVERY_SUCCEEDED:
            return _wifi_runtime_apply_recovery_succeeded(runtime, now_ms);

        case WIFI_RUNTIME_EVENT_RECOVERY_FAILED:
            return _wifi_runtime_apply_recovery_failed(runtime, event, now_ms);

        case WIFI_RUNTIME_EVENT_NONE:
        default:
            return -EINVAL;
    }
}

/****************************** 状态查询 ******************************/

/**
 * @brief 判断STA当前逻辑状态是否已经连接。
 */
bool wifi_runtime_sta_connected(const wifi_runtime_t *runtime)
{
    if (runtime == NULL || runtime->role != LINKG_DEVICE_ROLE_STA)
    {
        return false;
    }

    return runtime->link_state == WIFI_RUNTIME_LINK_CONNECTED;
}

/**
 * @brief 判断当前是否存在等待Owner处理的Wi-Fi恢复请求。
 */
bool wifi_runtime_recovery_required(const wifi_runtime_t *runtime)
{
    if (runtime == NULL)
    {
        return false;
    }

    return runtime->recovery_state == WIFI_RUNTIME_RECOVERY_REQUIRED;
}
