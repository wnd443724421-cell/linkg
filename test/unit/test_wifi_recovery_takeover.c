/**
 * @file test_wifi_recovery_takeover.c
 * @brief STA服务恢复期间事件监听接管及动态配置互斥测试
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-24
 */

#include <assert.h>
#include <stdlib.h>
#include <stdio.h>

// 使用实际Owner实现访问内部恢复入口，未使用的生产函数由链接器回收。
#include "../../modules/wifi/linkg_wifi.c"

#include "wifi_hostapd.h"
#include "wifi_status_query.h"
#include "wifi_wpa.h"
#include "wifi_wpa_event.h"

/****************************** 测试上下文 ******************************/

struct wifi_wpa_event_listener
{
    int fd; // 模拟监听描述符
};

static wifi_wpa_event_listener_t g_test_listener = { .fd = 10 };
static uint64_t                  g_test_now_ms;       // 注入的单调时间
static int                       g_test_open_error;   // 注入的监听建立错误
static unsigned int              g_test_radio_updates; // 动态配置实际执行次数

/****************************** 测试替身 ******************************/

/**
 * @brief 注入WPA监听建立失败。
 */
int wifi_wpa_event_listener_open(wifi_wpa_event_listener_t **listener)
{
    if (g_test_open_error != 0)
    {
        return g_test_open_error;
    }

    *listener = &g_test_listener;
    return 0;
}

/**
 * @brief 释放模拟监听器。
 */
void wifi_wpa_event_listener_close(wifi_wpa_event_listener_t *listener)
{
    assert(listener == &g_test_listener);
}

/**
 * @brief 中止模拟监听器。
 */
void wifi_wpa_event_listener_abort(wifi_wpa_event_listener_t *listener)
{
    assert(listener == &g_test_listener);
}

/**
 * @brief 模拟新的真实关联状态。
 */
int wifi_wpa_event_listener_get_link_state(wifi_wpa_event_listener_t *listener, wifi_wpa_link_state_t *state)
{
    assert(listener == &g_test_listener);
    *state = WIFI_WPA_LINK_STATE_CONNECTED;
    return 0;
}

/**
 * @brief 返回模拟监听描述符。
 */
int wifi_wpa_event_listener_get_fd(const wifi_wpa_event_listener_t *listener)
{
    assert(listener == &g_test_listener);
    return listener->fd;
}

/**
 * @brief 本测试不消费真实Socket事件。
 */
int wifi_wpa_event_listener_receive(wifi_wpa_event_listener_t *listener, wifi_wpa_event_t *event)
{
    (void)listener;
    (void)event;
    abort();
}

/**
 * @brief 模拟WPA控制接口可用。
 */
int wifi_wpa_ping(void)
{
    return 0;
}

/**
 * @brief 本测试不进入主动扫描流程。
 */
int wifi_wpa_scan(uint32_t frequency_mhz)
{
    (void)frequency_mhz;
    abort();
}

/**
 * @brief 本测试不执行AP健康检查。
 */
int wifi_hostapd_check_health(void)
{
    abort();
}

/**
 * @brief 模拟STA服务停止。
 */
int wifi_service_ops_stop(linkg_device_role_t role)
{
    assert(role == LINKG_DEVICE_ROLE_STA);
    return 0;
}

/**
 * @brief 模拟STA服务成功启动。
 */
int wifi_service_ops_start(linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    assert(role == LINKG_DEVICE_ROLE_STA);
    assert(config != NULL);
    return 0;
}

/**
 * @brief 模拟Radio运行状态同步。
 */
int wifi_radio_sync_runtime(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    (void)runtime;
    (void)now_ms;
    return 0;
}

/**
 * @brief 模拟服务启动时无线参数已重新应用。
 */
void wifi_radio_notify_reapplied(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    (void)runtime;
    (void)now_ms;
}

/**
 * @brief 记录动态配置实际进入Radio的次数。
 */
int wifi_radio_set_narrow_config(linkg_wifi_narrow_mode_t mode, uint16_t rate, const wifi_runtime_t *runtime, uint64_t now_ms)
{
    (void)mode;
    (void)rate;
    (void)now_ms;
    assert(runtime->recovery_state == WIFI_RUNTIME_RECOVERY_IDLE);
    g_test_radio_updates++;
    return 0;
}

/**
 * @brief 模拟Status配置更新。
 */
int wifi_status_set_narrow_config(linkg_wifi_narrow_mode_t mode, uint16_t rate)
{
    (void)mode;
    (void)rate;
    return 0;
}

/**
 * @brief 模拟状态采集暂不可用。
 */
int wifi_status_get_info(wifi_status_info_t *info)
{
    (void)info;
    return -EAGAIN;
}

/**
 * @brief 模拟频率尚未采集。
 */
int wifi_status_get_frequency_mhz(const linkg_wifi_status_snapshot_t *snapshot, uint32_t *frequency_mhz)
{
    (void)snapshot;
    (void)frequency_mhz;
    return -EAGAIN;
}

/**
 * @brief 返回测试单调时间。
 */
uint64_t linkg_time_elapsed_ms(void)
{
    return g_test_now_ms;
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
 * @brief 建立已连接STA，注入WPA恢复后监听器建立失败。
 */
static void _test_begin_recovery(void)
{
    wifi_runtime_event_t event;

    g_test_now_ms = 100U;
    g_test_open_error = 0;
    g_test_radio_updates = 0U;
    g_wifi.role = LINKG_DEVICE_ROLE_STA;
    g_wifi.lifecycle = LINKG_WIFI_LIFECYCLE_RUNNING;
    g_wifi.config.enabled = true;
    g_wifi.config.wideband.work_mode = LINKG_WIFI_WORK_MODE_NARROW;
    g_wifi.config.wideband.narrow_params.mode = LINKG_WIFI_NARROW_MODE_FIXED;
    g_wifi.config.wideband.narrow_params.manual_rate = 2U;

    assert(wifi_runtime_init(&g_wifi.runtime, g_wifi.role, g_test_now_ms) == 0);
    assert(wifi_monitor_init(g_wifi.role) == 0);
    assert(wifi_monitor_start(g_test_now_ms, &event) == 0);
    assert(_linkg_wifi_apply_link_event(&event, g_test_now_ms) == 0);
    assert(wifi_runtime_sta_connected(&g_wifi.runtime));

    g_test_open_error = -EIO;
    event.type = WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED;
    event.recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_WPA_UNRESPONSIVE;
    event.error = -EIO;
    assert(_linkg_wifi_handle_runtime_event(&event, g_test_now_ms) == 0);
    assert(g_wifi.runtime.recovery_state == WIFI_RUNTIME_RECOVERY_RUNNING);
    assert(!wifi_runtime_sta_connected(&g_wifi.runtime));
    assert(wifi_monitor_get_event_fd() == -ENOTCONN);
    assert(linkg_wifi_set_narrow_config(LINKG_WIFI_NARROW_MODE_ADAPTIVE, 0U) == -EBUSY);
    assert(g_test_radio_updates == 0U);
}

/**
 * @brief 验证监听重建后才放行动态配置。
 */
static void _test_takeover_success(void)
{
    _test_begin_recovery();

    assert(_linkg_wifi_process_monitor(g_test_now_ms) == 0);
    assert(g_wifi.runtime.recovery_state == WIFI_RUNTIME_RECOVERY_RUNNING);
    assert(linkg_wifi_set_narrow_config(LINKG_WIFI_NARROW_MODE_ADAPTIVE, 0U) == -EBUSY);

    g_test_open_error = 0;
    g_test_now_ms += 3000U;
    assert(_linkg_wifi_process_monitor(g_test_now_ms) == 0);
    assert(wifi_monitor_get_event_fd() == g_test_listener.fd);
    assert(wifi_runtime_sta_connected(&g_wifi.runtime));
    assert(g_wifi.runtime.recovery_state == WIFI_RUNTIME_RECOVERY_IDLE);
    assert(linkg_wifi_set_narrow_config(LINKG_WIFI_NARROW_MODE_ADAPTIVE, 0U) == 0);
    assert(g_test_radio_updates == 1U);

    wifi_monitor_deinit();
}

/**
 * @brief 验证连续监听重建失败仍禁止动态配置。
 */
static void _test_takeover_failure(void)
{
    _test_begin_recovery();

    assert(_linkg_wifi_process_monitor(g_test_now_ms) == 0);
    g_test_now_ms += 3000U;
    assert(_linkg_wifi_process_monitor(g_test_now_ms) == 0);
    g_test_now_ms += 3000U;
    assert(_linkg_wifi_process_monitor(g_test_now_ms) == -EIO);
    assert(g_wifi.runtime.recovery_state == WIFI_RUNTIME_RECOVERY_FAILED);
    assert(linkg_wifi_set_narrow_config(LINKG_WIFI_NARROW_MODE_ADAPTIVE, 0U) == -EBUSY);
    assert(g_test_radio_updates == 0U);

    wifi_monitor_deinit();
}

/****************************** 测试入口 ******************************/

/**
 * @brief 执行STA恢复接管回归测试。
 */
int main(void)
{
    _test_takeover_success();
    _test_takeover_failure();

    puts("wifi recovery takeover tests passed");
    return 0;
}
