/**
 * @file test_wifi_runtime.c
 * @brief LinkG Wi-Fi局部恢复状态回归测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-24
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "wifi_runtime.h"

/****************************** 测试辅助 ******************************/

/**
 * @brief 向运行状态提交指定事件。
 */
static int _test_event(wifi_runtime_t *runtime, wifi_runtime_event_type_t type, uint64_t now_ms)
{
    wifi_runtime_event_t event =
    {
        .type            = type,                                         // 运行事件
        .recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_WPA_UNRESPONSIVE, // 模拟WPA异常
        .error           = -EIO                                          // 模拟服务错误
    };

    return wifi_runtime_apply_event(runtime, &event, now_ms);
}

/**
 * @brief 已关联STA开始恢复后清除旧连接，成功不等于重新关联。
 */
static void _test_sta_recovery_success(void)
{
    wifi_runtime_t runtime;
    uint64_t       generation;

    assert(wifi_runtime_init(&runtime, LINKG_DEVICE_ROLE_STA, 10U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_STA_CONNECTED, 20U) == 0);
    assert(wifi_runtime_sta_connected(&runtime));
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED, 30U) == 0);

    generation = runtime.generation;
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_STARTED, 40U) == 0);
    assert(runtime.recovery_state == WIFI_RUNTIME_RECOVERY_RUNNING);
    assert(runtime.link_state == WIFI_RUNTIME_LINK_DISCONNECTED);
    assert(!wifi_runtime_sta_connected(&runtime));
    assert(runtime.generation == generation + 1U);
    assert(runtime.updated_ms == 40U);

    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_SUCCEEDED, 50U) == 0);
    assert(runtime.recovery_state == WIFI_RUNTIME_RECOVERY_IDLE);
    assert(!wifi_runtime_sta_connected(&runtime));

    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_STA_CONNECTED, 60U) == 0);
    assert(wifi_runtime_sta_connected(&runtime));

    // 再次恢复仍清除上一轮连接状态。
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED, 70U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_STARTED, 80U) == 0);
    assert(!wifi_runtime_sta_connected(&runtime));
}

/**
 * @brief 已关联及未关联STA恢复失败均保持断开。
 */
static void _test_sta_recovery_failure(void)
{
    wifi_runtime_t runtime;

    assert(wifi_runtime_init(&runtime, LINKG_DEVICE_ROLE_STA, 10U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_STA_CONNECTED, 20U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED, 30U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_STARTED, 40U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_FAILED, 50U) == 0);
    assert(runtime.recovery_state == WIFI_RUNTIME_RECOVERY_FAILED);
    assert(runtime.recovery_error == -EIO);
    assert(!wifi_runtime_sta_connected(&runtime));

    wifi_runtime_reset(&runtime, 60U);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED, 70U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_STARTED, 80U) == 0);
    assert(!wifi_runtime_sta_connected(&runtime));
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_FAILED, 90U) == 0);
    assert(!wifi_runtime_sta_connected(&runtime));
}

/**
 * @brief 非法恢复事件不改变已关联状态，AP不使用STA连接状态。
 */
static void _test_recovery_boundaries(void)
{
    wifi_runtime_t runtime;
    uint64_t       generation;

    assert(wifi_runtime_init(&runtime, LINKG_DEVICE_ROLE_STA, 10U) == 0);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_STA_CONNECTED, 20U) == 0);
    generation = runtime.generation;
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_STARTED, 30U) == -EPROTO);
    assert(wifi_runtime_sta_connected(&runtime));
    assert(runtime.generation == generation);

    assert(wifi_runtime_init(&runtime, LINKG_DEVICE_ROLE_AP, 40U) == 0);
    assert(wifi_runtime_apply_event(&runtime, &(wifi_runtime_event_t)
    {
        .type            = WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED,
        .recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_HOSTAPD_UNHEALTHY,
        .error           = -ENETDOWN
    }, 50U) == 0);
    assert(runtime.recovery_reason == WIFI_RUNTIME_RECOVERY_REASON_HOSTAPD_UNHEALTHY);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_STARTED, 60U) == 0);
    assert(runtime.link_state == WIFI_RUNTIME_LINK_NOT_APPLICABLE);
    assert(_test_event(&runtime, WIFI_RUNTIME_EVENT_RECOVERY_SUCCEEDED, 70U) == 0);
    assert(runtime.link_state == WIFI_RUNTIME_LINK_NOT_APPLICABLE);
}

/****************************** 测试入口 ******************************/

/**
 * @brief 执行Wi-Fi运行状态回归测试。
 */
int main(void)
{
    _test_sta_recovery_success();
    _test_sta_recovery_failure();
    _test_recovery_boundaries();

    puts("wifi runtime tests passed");
    return 0;
}
