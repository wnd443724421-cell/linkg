/**
 * @file test_wifi_ap_monitor.c
 * @brief AP监控周期及恢复阈值回归测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-24
 */

#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>

#include "linkg_log.h"

#include "wifi_hostapd.h"
#include "wifi_monitor.h"
#include "wifi_status.h"
#include "wifi_status_query.h"
#include "wifi_wpa.h"
#include "wifi_wpa_event.h"

/****************************** 测试上下文 ******************************/

static int          g_test_health_result; // 注入的hostapd健康查询结果
static unsigned int g_test_health_calls;  // 实际健康查询次数

/****************************** 测试替身 ******************************/

/**
 * @brief 模拟hostapd进程和控制接口查询结果。
 */
int wifi_hostapd_check_health(void)
{
    g_test_health_calls++;

    return g_test_health_result;
}

/**
 * @brief AP不应建立STA监听器。
 */
int wifi_wpa_event_listener_open(wifi_wpa_event_listener_t **listener)
{
    (void)listener;
    abort();
}

/**
 * @brief AP不应关闭STA监听器。
 */
void wifi_wpa_event_listener_close(wifi_wpa_event_listener_t *listener)
{
    (void)listener;
    abort();
}

/**
 * @brief AP不应中止STA监听器。
 */
void wifi_wpa_event_listener_abort(wifi_wpa_event_listener_t *listener)
{
    (void)listener;
    abort();
}

/**
 * @brief AP不应查询STA关联状态。
 */
int wifi_wpa_event_listener_get_link_state(wifi_wpa_event_listener_t *listener, wifi_wpa_link_state_t *state)
{
    (void)listener;
    (void)state;
    abort();
}

/**
 * @brief AP不应读取STA事件描述符。
 */
int wifi_wpa_event_listener_get_fd(const wifi_wpa_event_listener_t *listener)
{
    (void)listener;
    abort();
}

/**
 * @brief AP不应消费STA事件。
 */
int wifi_wpa_event_listener_receive(wifi_wpa_event_listener_t *listener, wifi_wpa_event_t *event)
{
    (void)listener;
    (void)event;
    abort();
}

/**
 * @brief AP不应主动执行STA扫描。
 */
int wifi_wpa_scan(uint32_t frequency_mhz)
{
    (void)frequency_mhz;
    abort();
}

/**
 * @brief AP不应检查WPA服务健康。
 */
int wifi_wpa_ping(void)
{
    abort();
}

/**
 * @brief AP健康检查不使用对端状态统计。
 */
int wifi_status_get_info(wifi_status_info_t *info)
{
    (void)info;
    abort();
}

/**
 * @brief AP健康检查不使用STA频率捕获。
 */
int wifi_status_get_frequency_mhz(const linkg_wifi_status_snapshot_t *snapshot, uint32_t *frequency_mhz)
{
    (void)snapshot;
    (void)frequency_mhz;
    abort();
}

/**
 * @brief 测试不依赖设备日志路径。
 */
void linkg_log_write_internal(linkg_log_level_t level, const char *file, int line, const char *format, ...)
{
    (void)level;
    (void)file;
    (void)line;
    (void)format;
}

/****************************** 回归测试 ******************************/

/**
 * @brief 验证AP监控期限、瞬时失败恢复及连续失败请求。
 */
static void _test_ap_health(void)
{
    wifi_runtime_event_t event;

    assert(wifi_monitor_init(LINKG_DEVICE_ROLE_AP) == 0);
    assert(wifi_monitor_start(100U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);
    assert(wifi_monitor_get_deadline() == 3100U);
    assert(wifi_monitor_get_event_fd() == -ENOTCONN);

    assert(wifi_monitor_process(3099U, &event) == 0);
    assert(g_test_health_calls == 0U);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);

    g_test_health_result = -ETIMEDOUT;
    assert(wifi_monitor_process(3100U, &event) == 0);
    assert(g_test_health_calls == 1U);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);
    assert(wifi_monitor_get_deadline() == 6100U);

    g_test_health_result = 0;
    assert(wifi_monitor_process(6100U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);

    g_test_health_result = -ENETDOWN;
    assert(wifi_monitor_process(9100U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);
    assert(wifi_monitor_process(12100U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED);
    assert(event.recovery_reason == WIFI_RUNTIME_RECOVERY_REASON_HOSTAPD_UNHEALTHY);
    assert(event.error == -ENETDOWN);
    assert(wifi_monitor_get_deadline() == 0U);
    assert(g_test_health_calls == 4U);

    assert(wifi_monitor_process(15100U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);
    assert(g_test_health_calls == 4U);

    wifi_monitor_stop();
    assert(wifi_monitor_get_deadline() == 0U);
    assert(wifi_monitor_process(16000U, &event) == -ENODEV);
    assert(wifi_monitor_start(17000U, &event) == 0);

    // 恢复后的第一次超时不继承上一轮失败计数。
    g_test_health_result = -ETIMEDOUT;
    assert(wifi_monitor_process(20000U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_NONE);

    // 确认进程退出立即请求恢复。
    g_test_health_result = -ECHILD;
    assert(wifi_monitor_process(23000U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED);
    assert(event.recovery_reason == WIFI_RUNTIME_RECOVERY_REASON_HOSTAPD_UNHEALTHY);
    assert(event.error == -ECHILD);

    wifi_monitor_stop();
    assert(wifi_monitor_start(24000U, &event) == 0);
    assert(wifi_monitor_process(27000U, &event) == 0);
    assert(event.type == WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED);
    assert(event.error == -ECHILD);

    wifi_monitor_deinit();
    assert(wifi_monitor_get_deadline() == 0U);
}

/****************************** 测试入口 ******************************/

/**
 * @brief 执行AP监控回归测试。
 */
int main(void)
{
    _test_ap_health();

    puts("wifi AP monitor tests passed");
    return 0;
}
