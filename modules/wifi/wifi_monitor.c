/**
 * @file wifi_monitor.c
 * @brief LinkG Wi-Fi STA连接状态机实现
 * @author Dawn
 * @version 5.0.0
 * @date 2026-08-26
 */

#include "wifi_monitor.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "wifi_internal.h"
#include "wifi_status.h"
#include "wifi_status_query.h"
#include "wifi_wpa.h"
#include "wifi_wpa_event.h"

/****************************** 监控参数 ******************************/

#define WIFI_MONITOR_COLD_CONNECT_WAIT_MS    15000U  // 冷启动等待WPA自动连接时间
#define WIFI_MONITOR_SCAN_INTERVAL_MS        3000U   // 两轮主动扫描之间的等待时间
#define WIFI_MONITOR_SCAN_RESULT_TIMEOUT_MS  8000U   // 主动扫描结果等待超时时间
#define WIFI_MONITOR_TARGETED_SCAN_COUNT     3U      // 每轮全频扫描前的定向扫描次数
#define WIFI_MONITOR_SCAN_FAILURE_LIMIT      3U      // 连续扫描失败触发恢复的阈值
#define WIFI_MONITOR_FREQUENCY_CAPTURE_MS    1000U   // 连接后延迟采集工作频率时间
#define WIFI_MONITOR_WPA_HEALTH_INTERVAL_MS  3000U   // WPA服务健康检查周期
#define WIFI_MONITOR_LISTENER_FAILURE_LIMIT  3U      // 连续事件监听恢复失败阈值

/****************************** 内部状态 ******************************/

typedef enum
{
    WIFI_MONITOR_STA_COLD_WAIT = 0, // 冷启动等待WPA自动连接
    WIFI_MONITOR_STA_CONNECTED,     // STA当前已经连接
    WIFI_MONITOR_STA_SCAN_WAIT,     // 等待发起下一轮主动扫描
    WIFI_MONITOR_STA_SCAN_RUNNING,  // 已发起扫描并等待扫描结果
    WIFI_MONITOR_STA_RECOVERY_WAIT  // 已请求Owner恢复Wi-Fi服务
} wifi_monitor_sta_state_t;

typedef struct
{
    wifi_wpa_event_listener_t *listener;                  // WPA事件监听器
    wifi_monitor_sta_state_t   sta_state;                 // 当前STA连接监控状态
    uint64_t                   state_deadline_ms;         // 当前状态下一动作期限
    uint64_t                   connected_event_ms;        // 最近CONNECTED事件时间
    uint64_t                   frequency_capture_ms;      // 下一次工作频率采集时间
    uint64_t                   wpa_health_check_ms;       // 下一次WPA健康检查时间
    uint32_t                   last_frequency_mhz;        // 最近一次成功连接频率
    uint8_t                    targeted_scan_count;       // 当前连续定向扫描次数
    uint8_t                    scan_failure_count;        // 当前连续扫描失败次数
    uint8_t                    listener_failure_count;    // 连续事件监听恢复失败次数
    int                        listener_fd;               // WPA事件监听描述符
    bool                       frequency_capture_pending; // 是否等待采集连接频率
    bool                       initialized;               // 模块是否已经初始化
    bool                       started;                   // 状态机是否已经启动
} wifi_monitor_context_t;

/****************************** 全局上下文 ******************************/

static wifi_monitor_context_t g_wifi_monitor =
{
    .listener_fd = -1 // 尚未建立WPA事件监听
};

/****************************** 事件辅助 ******************************/

/**
 * @brief 清空一个Wi-Fi运行事件。
 */
static void _wifi_monitor_clear_event(wifi_runtime_event_t *event)
{
    if (event == NULL)
    {
        return;
    }

    memset(event, 0, sizeof(*event));
    event->type            = WIFI_RUNTIME_EVENT_NONE;
    event->recovery_reason = WIFI_RUNTIME_RECOVERY_REASON_NONE;
}

/**
 * @brief 构造一个STA连接状态变化事件。
 */
static void _wifi_monitor_set_link_event(wifi_runtime_event_t *event, bool connected)
{
    if (event == NULL)
    {
        return;
    }

    event->type = connected
        ? WIFI_RUNTIME_EVENT_STA_CONNECTED
        : WIFI_RUNTIME_EVENT_STA_DISCONNECTED;
}

/**
 * @brief 构造一个需要Owner执行的服务恢复事件。
 */
static void _wifi_monitor_set_recovery_event(wifi_runtime_event_t *event, wifi_runtime_recovery_reason_t reason, int error)
{
    if (event == NULL)
    {
        return;
    }

    event->type            = WIFI_RUNTIME_EVENT_RECOVERY_REQUIRED;
    event->recovery_reason = reason;
    event->error           = error;
}

/****************************** 状态辅助 ******************************/

/**
 * @brief 获取STA监控状态名称。
 */
static const char *_wifi_monitor_state_name(wifi_monitor_sta_state_t state)
{
    switch (state)
    {
        case WIFI_MONITOR_STA_COLD_WAIT:
            return "COLD_WAIT";

        case WIFI_MONITOR_STA_CONNECTED:
            return "CONNECTED";

        case WIFI_MONITOR_STA_SCAN_WAIT:
            return "SCAN_WAIT";

        case WIFI_MONITOR_STA_SCAN_RUNNING:
            return "SCAN_RUNNING";

        case WIFI_MONITOR_STA_RECOVERY_WAIT:
            return "RECOVERY_WAIT";

        default:
            return "UNKNOWN";
    }
}

/**
 * @brief 更新STA连接监控状态及对应期限。
 */
static void _wifi_monitor_set_state(wifi_monitor_sta_state_t state, uint64_t deadline_ms)
{
    wifi_monitor_sta_state_t previous;

    previous = g_wifi_monitor.sta_state;

    g_wifi_monitor.sta_state         = state;
    g_wifi_monitor.state_deadline_ms = deadline_ms;

    if (previous != state)
    {
        WIFI_DEBUG("STA monitor state changed, old=%s, new=%s",
                   _wifi_monitor_state_name(previous),
                   _wifi_monitor_state_name(state));
    }
}

/**
 * @brief 重置STA连接状态机动态运行字段。
 */
static void _wifi_monitor_reset_runtime(void)
{
    g_wifi_monitor.listener                   = NULL;
    g_wifi_monitor.listener_fd                = -1;
    g_wifi_monitor.sta_state                  = WIFI_MONITOR_STA_COLD_WAIT;
    g_wifi_monitor.state_deadline_ms          = 0U;
    g_wifi_monitor.connected_event_ms         = 0U;
    g_wifi_monitor.frequency_capture_ms       = 0U;
    g_wifi_monitor.wpa_health_check_ms        = 0U;
    g_wifi_monitor.last_frequency_mhz         = 0U;
    g_wifi_monitor.targeted_scan_count        = 0U;
    g_wifi_monitor.scan_failure_count         = 0U;
    g_wifi_monitor.listener_failure_count     = 0U;
    g_wifi_monitor.frequency_capture_pending  = false;
}

/**
 * @brief 将状态机切换到STA已连接状态。
 */
static bool _wifi_monitor_set_connected(uint64_t now_ms)
{
    bool changed;

    changed = g_wifi_monitor.sta_state != WIFI_MONITOR_STA_CONNECTED;

    g_wifi_monitor.targeted_scan_count       = 0U;
    g_wifi_monitor.scan_failure_count        = 0U;
    g_wifi_monitor.connected_event_ms        = now_ms;
    g_wifi_monitor.frequency_capture_ms      = now_ms + WIFI_MONITOR_FREQUENCY_CAPTURE_MS;
    g_wifi_monitor.frequency_capture_pending = true;

    _wifi_monitor_set_state(WIFI_MONITOR_STA_CONNECTED, 0U);

    if (changed)
    {
        WIFI_INFO("STA link connected");
    }

    return changed;
}

/**
 * @brief 将状态机切换到断链扫描恢复流程。
 */
static bool _wifi_monitor_start_scan_cycle(uint64_t now_ms)
{
    bool changed;

    changed = g_wifi_monitor.sta_state == WIFI_MONITOR_STA_CONNECTED;

    g_wifi_monitor.targeted_scan_count       = 0U;
    g_wifi_monitor.scan_failure_count        = 0U;
    g_wifi_monitor.frequency_capture_ms      = 0U;
    g_wifi_monitor.frequency_capture_pending = false;

    _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_WAIT,
                            now_ms + WIFI_MONITOR_SCAN_INTERVAL_MS);

    if (changed)
    {
        WIFI_INFO("STA link disconnected, recovery=scan");
    }

    return changed;
}

/**
 * @brief 将状态机暂停在等待Owner恢复服务的状态。
 */
static void _wifi_monitor_wait_recovery(void)
{
    g_wifi_monitor.state_deadline_ms          = 0U;
    g_wifi_monitor.frequency_capture_ms       = 0U;
    g_wifi_monitor.wpa_health_check_ms        = 0U;
    g_wifi_monitor.frequency_capture_pending  = false;

    _wifi_monitor_set_state(WIFI_MONITOR_STA_RECOVERY_WAIT, 0U);
}

/****************************** 事件监听器 ******************************/

/**
 * @brief 关闭当前WPA事件监听器。
 */
static void _wifi_monitor_close_listener(bool abnormal)
{
    wifi_wpa_event_listener_t *listener;

    listener = g_wifi_monitor.listener;

    g_wifi_monitor.listener    = NULL;
    g_wifi_monitor.listener_fd = -1;

    if (listener == NULL)
    {
        return;
    }

    if (abnormal)
    {
        wifi_wpa_event_listener_abort(listener);
        return;
    }

    wifi_wpa_event_listener_close(listener);
}

/**
 * @brief 建立WPA事件监听并通过STATUS同步当前连接状态。
 */
static int _wifi_monitor_open_listener(wifi_wpa_link_state_t *link_state)
{
    wifi_wpa_event_listener_t *listener;
    int                        listener_fd;
    int                        ret;

    if (link_state == NULL)
    {
        return -EINVAL;
    }

    *link_state = WIFI_WPA_LINK_STATE_DISCONNECTED;

    ret = wifi_wpa_event_listener_open(&listener);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_wpa_event_listener_get_link_state(listener, link_state);
    if (ret != 0)
    {
        wifi_wpa_event_listener_abort(listener);
        return ret;
    }

    listener_fd = wifi_wpa_event_listener_get_fd(listener);
    if (listener_fd < 0)
    {
        wifi_wpa_event_listener_abort(listener);
        return listener_fd;
    }

    g_wifi_monitor.listener               = listener;
    g_wifi_monitor.listener_fd            = listener_fd;
    g_wifi_monitor.listener_failure_count = 0U;

    return 0;
}

/**
 * @brief 记录一次WPA事件监听恢复失败并在达到阈值时请求服务恢复。
 */
static void _wifi_monitor_record_listener_failure(int error, wifi_runtime_event_t *event)
{
    if (g_wifi_monitor.listener_failure_count < UINT8_MAX)
    {
        g_wifi_monitor.listener_failure_count++;
    }

    if (g_wifi_monitor.listener_failure_count < WIFI_MONITOR_LISTENER_FAILURE_LIMIT)
    {
        WIFI_DEBUG("restore WPA event listener failed, consecutive=%u, error=%d",
                   (unsigned int)g_wifi_monitor.listener_failure_count,
                   error);
        return;
    }

    WIFI_WARN("WPA event listener recovery threshold reached, consecutive=%u, error=%d",
              (unsigned int)g_wifi_monitor.listener_failure_count,
              error);

    _wifi_monitor_wait_recovery();
    _wifi_monitor_set_recovery_event(event,
                                     WIFI_RUNTIME_RECOVERY_REASON_EVENT_CHANNEL,
                                     error);
}

/****************************** 扫描恢复 ******************************/

/**
 * @brief 选择下一次主动扫描使用的频率，0表示全频扫描。
 */
static uint32_t _wifi_monitor_select_scan_frequency(void)
{
    if (g_wifi_monitor.last_frequency_mhz != 0U &&
        g_wifi_monitor.targeted_scan_count < WIFI_MONITOR_TARGETED_SCAN_COUNT)
    {
        return g_wifi_monitor.last_frequency_mhz;
    }

    return 0U;
}

/**
 * @brief 记录一次扫描失败并在达到阈值时请求Owner恢复服务。
 */
static void _wifi_monitor_record_scan_failure(uint64_t now_ms, int error, const char *reason, wifi_runtime_event_t *event)
{
    if (g_wifi_monitor.scan_failure_count < UINT8_MAX)
    {
        g_wifi_monitor.scan_failure_count++;
    }

    if (g_wifi_monitor.scan_failure_count >= WIFI_MONITOR_SCAN_FAILURE_LIMIT)
    {
        WIFI_WARN("STA scan failure threshold reached, consecutive=%u, reason=%s, error=%d",
                  (unsigned int)g_wifi_monitor.scan_failure_count,
                  reason,
                  error);

        _wifi_monitor_wait_recovery();
        _wifi_monitor_set_recovery_event(event,
                                         WIFI_RUNTIME_RECOVERY_REASON_SCAN_STALLED,
                                         error);
        return;
    }

    WIFI_DEBUG("STA scan failed, consecutive=%u, reason=%s, error=%d",
               (unsigned int)g_wifi_monitor.scan_failure_count,
               reason,
               error);

    _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_WAIT,
                            now_ms + WIFI_MONITOR_SCAN_INTERVAL_MS);
}

/**
 * @brief 发起一次定向或全频STA扫描。
 */
static void _wifi_monitor_request_scan(uint64_t now_ms, wifi_runtime_event_t *event)
{
    uint32_t frequency_mhz;
    int      ret;

    frequency_mhz = _wifi_monitor_select_scan_frequency();
    ret = wifi_wpa_scan(frequency_mhz);

    if (ret == -EBUSY)
    {
        WIFI_DEBUG("STA scan already running, waiting for current scan result");
        _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_RUNNING,
                                now_ms + WIFI_MONITOR_SCAN_RESULT_TIMEOUT_MS);
        return;
    }

    if (ret != 0)
    {
        _wifi_monitor_record_scan_failure(now_ms,
                                          ret,
                                          "SCAN command failed",
                                          event);
        return;
    }

    if (frequency_mhz == 0U)
    {
        g_wifi_monitor.targeted_scan_count = 0U;
    }
    else
    {
        g_wifi_monitor.targeted_scan_count++;
    }

    _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_RUNNING,
                            now_ms + WIFI_MONITOR_SCAN_RESULT_TIMEOUT_MS);

    WIFI_DEBUG("STA scan requested, mode=%s, frequency_mhz=%u, targeted_index=%u",
               frequency_mhz == 0U ? "full" : "targeted",
               (unsigned int)frequency_mhz,
               (unsigned int)g_wifi_monitor.targeted_scan_count);
}

/****************************** 连接频率 ******************************/

/**
 * @brief 在连接稳定后从统一Wi-Fi状态记录最近工作频率。
 */
static void _wifi_monitor_capture_frequency(uint64_t now_ms)
{
    wifi_status_info_t info;
    uint32_t           frequency_mhz;
    int                ret;

    if (!g_wifi_monitor.frequency_capture_pending ||
        now_ms < g_wifi_monitor.frequency_capture_ms)
    {
        return;
    }

    memset(&info, 0, sizeof(info));

    ret = wifi_status_get_info(&info);
    if (ret != 0 ||
        !info.valid ||
        info.snapshot.local.interface_state != LINKG_WIFI_INTERFACE_STATE_CONNECTED ||
        info.last_success_ms <= g_wifi_monitor.connected_event_ms)
    {
        g_wifi_monitor.frequency_capture_ms = now_ms + WIFI_MONITOR_FREQUENCY_CAPTURE_MS;
        return;
    }

    ret = wifi_status_get_frequency_mhz(&info.snapshot, &frequency_mhz);
    if (ret != 0)
    {
        g_wifi_monitor.frequency_capture_ms = now_ms + WIFI_MONITOR_FREQUENCY_CAPTURE_MS;
        return;
    }

    g_wifi_monitor.last_frequency_mhz        = frequency_mhz;
    g_wifi_monitor.frequency_capture_pending = false;
    g_wifi_monitor.frequency_capture_ms      = 0U;

    WIFI_DEBUG("STA connected frequency recorded, frequency_mhz=%u",
               (unsigned int)frequency_mhz);
}

/****************************** WPA状态 ******************************/

/**
 * @brief 根据STATUS同步结果更新连接状态机并产生必要的运行事件。
 */
static void _wifi_monitor_apply_link_state(wifi_wpa_link_state_t link_state, uint64_t now_ms, wifi_runtime_event_t *event)
{
    bool changed;

    if (link_state == WIFI_WPA_LINK_STATE_CONNECTED)
    {
        changed = _wifi_monitor_set_connected(now_ms);
        if (changed)
        {
            _wifi_monitor_set_link_event(event, true);
        }

        return;
    }

    if (g_wifi_monitor.sta_state == WIFI_MONITOR_STA_CONNECTED)
    {
        changed = _wifi_monitor_start_scan_cycle(now_ms);
        if (changed)
        {
            _wifi_monitor_set_link_event(event, false);
        }
    }
}

/**
 * @brief 处理一次到期的wpa_supplicant健康检查。
 */
static void _wifi_monitor_process_wpa_health(uint64_t now_ms, wifi_runtime_event_t *event)
{
    wifi_wpa_link_state_t link_state;
    int                   first_error;
    int                   ret;

    if (now_ms < g_wifi_monitor.wpa_health_check_ms ||
        g_wifi_monitor.sta_state == WIFI_MONITOR_STA_RECOVERY_WAIT)
    {
        return;
    }

    g_wifi_monitor.wpa_health_check_ms =
        now_ms + WIFI_MONITOR_WPA_HEALTH_INTERVAL_MS;

    first_error = 0;
    ret = wifi_wpa_ping();

    if (ret != 0)
    {
        first_error = ret;
        ret = wifi_wpa_ping();

        if (ret == 0)
        {
            WIFI_DEBUG("wpa_supplicant transient health check failure recovered, first_error=%d",
                       first_error);
        }
    }

    if (ret != 0)
    {
        WIFI_WARN("wpa_supplicant health check failed twice, first_error=%d, second_error=%d",
                  first_error,
                  ret);

        _wifi_monitor_close_listener(true);
        _wifi_monitor_wait_recovery();
        _wifi_monitor_set_recovery_event(event,
                                         WIFI_RUNTIME_RECOVERY_REASON_WPA_UNRESPONSIVE,
                                         ret);
        return;
    }

    if (g_wifi_monitor.listener != NULL && g_wifi_monitor.listener_fd >= 0)
    {
        return;
    }

    ret = _wifi_monitor_open_listener(&link_state);
    if (ret != 0)
    {
        _wifi_monitor_record_listener_failure(ret, event);
        return;
    }

    WIFI_INFO("wpa_supplicant event listener restored, link_state=%s",
              link_state == WIFI_WPA_LINK_STATE_CONNECTED
                  ? "CONNECTED"
                  : "DISCONNECTED");

    _wifi_monitor_apply_link_state(link_state, now_ms, event);

    if (link_state == WIFI_WPA_LINK_STATE_DISCONNECTED &&
        g_wifi_monitor.sta_state != WIFI_MONITOR_STA_COLD_WAIT &&
        g_wifi_monitor.sta_state != WIFI_MONITOR_STA_CONNECTED &&
        g_wifi_monitor.sta_state != WIFI_MONITOR_STA_RECOVERY_WAIT)
    {
        _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_WAIT,
                                now_ms + WIFI_MONITOR_SCAN_INTERVAL_MS);
    }
}

/****************************** WPA事件 ******************************/

/**
 * @brief 处理一条wpa_supplicant语义事件。
 */
static void _wifi_monitor_handle_wpa_event(wifi_wpa_event_t wpa_event, uint64_t now_ms, wifi_runtime_event_t *event)
{
    bool changed;

    switch (wpa_event)
    {
        case WIFI_WPA_EVENT_CONNECTED:
            changed = _wifi_monitor_set_connected(now_ms);
            if (changed)
            {
                _wifi_monitor_set_link_event(event, true);
            }
            break;

        case WIFI_WPA_EVENT_DISCONNECTED:
            if (g_wifi_monitor.sta_state == WIFI_MONITOR_STA_CONNECTED)
            {
                changed = _wifi_monitor_start_scan_cycle(now_ms);
                if (changed)
                {
                    _wifi_monitor_set_link_event(event, false);
                }
            }
            break;

        case WIFI_WPA_EVENT_SCAN_RESULTS:
        case WIFI_WPA_EVENT_NETWORK_NOT_FOUND:
            if (g_wifi_monitor.sta_state == WIFI_MONITOR_STA_SCAN_RUNNING)
            {
                g_wifi_monitor.scan_failure_count = 0U;
                _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_WAIT,
                                        now_ms + WIFI_MONITOR_SCAN_INTERVAL_MS);
            }
            break;

        case WIFI_WPA_EVENT_SCAN_FAILED:
            if (g_wifi_monitor.sta_state == WIFI_MONITOR_STA_SCAN_RUNNING)
            {
                _wifi_monitor_record_scan_failure(now_ms,
                                                  -EIO,
                                                  "SCAN_FAILED event",
                                                  event);
            }
            break;

        case WIFI_WPA_EVENT_SCAN_STARTED:
            if (g_wifi_monitor.sta_state == WIFI_MONITOR_STA_SCAN_WAIT)
            {
                _wifi_monitor_set_state(WIFI_MONITOR_STA_SCAN_RUNNING,
                                        now_ms + WIFI_MONITOR_SCAN_RESULT_TIMEOUT_MS);
            }
            break;

        case WIFI_WPA_EVENT_TERMINATING:
            WIFI_WARN("wpa_supplicant terminating, recovery=requested");
            _wifi_monitor_close_listener(true);
            _wifi_monitor_wait_recovery();
            _wifi_monitor_set_recovery_event(event,
                                             WIFI_RUNTIME_RECOVERY_REASON_WPA_UNRESPONSIVE,
                                             -ESHUTDOWN);
            break;

        case WIFI_WPA_EVENT_UNKNOWN:
        default:
            break;
    }
}

/****************************** 定时辅助 ******************************/

/**
 * @brief 返回两个非零期限中更早的一个。
 */
static uint64_t _wifi_monitor_min_deadline(uint64_t left, uint64_t right)
{
    if (left == 0U)
    {
        return right;
    }

    if (right == 0U)
    {
        return left;
    }

    return left < right ? left : right;
}

/**
 * @brief 处理STA连接状态机本身的到期动作。
 */
static void _wifi_monitor_process_state_deadline(uint64_t now_ms, wifi_runtime_event_t *event)
{
    if (g_wifi_monitor.state_deadline_ms == 0U ||
        now_ms < g_wifi_monitor.state_deadline_ms)
    {
        return;
    }

    switch (g_wifi_monitor.sta_state)
    {
        case WIFI_MONITOR_STA_COLD_WAIT:
        case WIFI_MONITOR_STA_SCAN_WAIT:
            _wifi_monitor_request_scan(now_ms, event);
            break;

        case WIFI_MONITOR_STA_SCAN_RUNNING:
            _wifi_monitor_record_scan_failure(now_ms,
                                              -ETIMEDOUT,
                                              "scan result timeout",
                                              event);
            break;

        case WIFI_MONITOR_STA_CONNECTED:
        case WIFI_MONITOR_STA_RECOVERY_WAIT:
        default:
            g_wifi_monitor.state_deadline_ms = 0U;
            break;
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化STA连接监控状态机。
 *
 * @note 本模块不创建线程，只能由network-wifi Owner线程调用。
 */
int wifi_monitor_init(void)
{
    if (g_wifi_monitor.initialized)
    {
        return -EALREADY;
    }

    memset(&g_wifi_monitor, 0, sizeof(g_wifi_monitor));

    g_wifi_monitor.listener_fd = -1;
    g_wifi_monitor.initialized = true;

    return 0;
}

/**
 * @brief 启动STA连接监控并建立WPA事件顺序屏障。
 */
int wifi_monitor_start(uint64_t now_ms, wifi_runtime_event_t *event)
{
    wifi_wpa_link_state_t link_state;
    int                   ret;

    if (!g_wifi_monitor.initialized)
    {
        return -ENODEV;
    }

    if (g_wifi_monitor.started)
    {
        return -EALREADY;
    }

    if (event == NULL)
    {
        return -EINVAL;
    }

    _wifi_monitor_clear_event(event);
    _wifi_monitor_reset_runtime();

    g_wifi_monitor.started             = true;
    g_wifi_monitor.wpa_health_check_ms = now_ms + WIFI_MONITOR_WPA_HEALTH_INTERVAL_MS;

    ret = _wifi_monitor_open_listener(&link_state);
    if (ret != 0)
    {
        WIFI_WARN("open initial wpa_supplicant event listener failed, error=%d", ret);

        g_wifi_monitor.wpa_health_check_ms = now_ms;
        _wifi_monitor_set_state(WIFI_MONITOR_STA_COLD_WAIT,
                                now_ms + WIFI_MONITOR_COLD_CONNECT_WAIT_MS);
        return 0;
    }

    if (link_state == WIFI_WPA_LINK_STATE_CONNECTED)
    {
        _wifi_monitor_set_connected(now_ms);
        _wifi_monitor_set_link_event(event, true);
    }
    else
    {
        _wifi_monitor_set_state(WIFI_MONITOR_STA_COLD_WAIT,
                                now_ms + WIFI_MONITOR_COLD_CONNECT_WAIT_MS);
        WIFI_INFO("STA link initialized, state=DISCONNECTED");
    }

    return 0;
}

/**
 * @brief 停止STA连接状态机并关闭WPA事件监听器。
 */
void wifi_monitor_stop(void)
{
    if (!g_wifi_monitor.initialized || !g_wifi_monitor.started)
    {
        return;
    }

    _wifi_monitor_close_listener(false);
    _wifi_monitor_reset_runtime();

    g_wifi_monitor.started = false;
}

/**
 * @brief 释放STA连接监控状态机。
 */
void wifi_monitor_deinit(void)
{
    if (!g_wifi_monitor.initialized)
    {
        return;
    }

    wifi_monitor_stop();
    memset(&g_wifi_monitor, 0, sizeof(g_wifi_monitor));
    g_wifi_monitor.listener_fd = -1;
}

/****************************** 事件监听 ******************************/

/**
 * @brief 获取当前WPA事件监听文件描述符。
 */
int wifi_monitor_get_event_fd(void)
{
    if (!g_wifi_monitor.initialized || !g_wifi_monitor.started)
    {
        return -ENODEV;
    }

    if (g_wifi_monitor.listener == NULL || g_wifi_monitor.listener_fd < 0)
    {
        return -ENOTCONN;
    }

    return g_wifi_monitor.listener_fd;
}

/**
 * @brief 接收并处理一条WPA事件，必要时输出Wi-Fi运行事件。
 */
int wifi_monitor_handle_event_fd(uint64_t now_ms, wifi_runtime_event_t *event)
{
    wifi_wpa_event_t wpa_event;
    int              ret;

    if (!g_wifi_monitor.initialized || !g_wifi_monitor.started)
    {
        return -ENODEV;
    }

    if (event == NULL)
    {
        return -EINVAL;
    }

    _wifi_monitor_clear_event(event);

    if (g_wifi_monitor.listener == NULL || g_wifi_monitor.listener_fd < 0)
    {
        return -ENOTCONN;
    }

    ret = wifi_wpa_event_listener_receive(g_wifi_monitor.listener, &wpa_event);
    if (ret != 0)
    {
        WIFI_WARN("receive wpa_supplicant event failed, error=%d", ret);

        _wifi_monitor_close_listener(true);
        g_wifi_monitor.wpa_health_check_ms = now_ms;

        return 0;
    }

    _wifi_monitor_handle_wpa_event(wpa_event, now_ms, event);

    return 0;
}

/**
 * @brief 处理Owner检测到的WPA事件通道异常。
 */
int wifi_monitor_handle_event_channel_error(uint64_t now_ms, wifi_runtime_event_t *event)
{
    if (!g_wifi_monitor.initialized || !g_wifi_monitor.started)
    {
        return -ENODEV;
    }

    if (event == NULL)
    {
        return -EINVAL;
    }

    _wifi_monitor_clear_event(event);

    _wifi_monitor_close_listener(true);
    g_wifi_monitor.wpa_health_check_ms = now_ms;

    return 0;
}

/****************************** 定时处理 ******************************/

/**
 * @brief 获取STA连接状态机的最近到期期限。
 */
uint64_t wifi_monitor_get_deadline(void)
{
    uint64_t deadline;

    if (!g_wifi_monitor.initialized || !g_wifi_monitor.started)
    {
        return 0U;
    }

    deadline = _wifi_monitor_min_deadline(g_wifi_monitor.state_deadline_ms,
                                          g_wifi_monitor.frequency_capture_ms);

    return _wifi_monitor_min_deadline(deadline,
                                      g_wifi_monitor.wpa_health_check_ms);
}

/**
 * @brief 处理所有已经到期的STA连接维护动作。
 */
int wifi_monitor_process(uint64_t now_ms, wifi_runtime_event_t *event)
{
    if (!g_wifi_monitor.initialized || !g_wifi_monitor.started)
    {
        return -ENODEV;
    }

    if (event == NULL)
    {
        return -EINVAL;
    }

    _wifi_monitor_clear_event(event);

    if (g_wifi_monitor.sta_state == WIFI_MONITOR_STA_RECOVERY_WAIT)
    {
        return 0;
    }

    _wifi_monitor_process_wpa_health(now_ms, event);
    if (event->type != WIFI_RUNTIME_EVENT_NONE)
    {
        return 0;
    }

    _wifi_monitor_process_state_deadline(now_ms, event);
    if (event->type != WIFI_RUNTIME_EVENT_NONE)
    {
        return 0;
    }

    _wifi_monitor_capture_frequency(now_ms);

    return 0;
}
