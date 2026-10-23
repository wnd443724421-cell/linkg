/**
 * @file linkg_gpio_manager.c
 * @brief LinkG GPIO硬件控制、RSSI指示灯及长按恢复管理
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-12
 */

#include "linkg_gpio_manager.h"

#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "linkg_cellular.h"
#include "linkg_config.h"
#include "linkg_gpio.h"
#include "linkg_lifecycle.h"
#include "linkg_log.h"
#include "linkg_system_resources.h"
#include "linkg_thread.h"
#include "linkg_time.h"
#include "linkg_wifi.h"
#include "linkg_node.h"

/****************************** 调度常量 ******************************/

#define LINKG_GPIO_THREAD_NAME             "linkg_gpio" // GPIO后台线程名称
#define LINKG_GPIO_POLL_INTERVAL_MS        100U         // 按键及指示灯处理周期
#define LINKG_GPIO_SIGNAL_INTERVAL_MS      2000U        // RSSI及可达状态读取周期
#define LINKG_GPIO_WIFI_MAX_AGE_MS         5000U        // Wi-Fi快照最大有效年龄
#define LINKG_GPIO_CELLULAR_MAX_AGE_MS     15000U       // 蜂窝无线状态最大有效年龄
#define LINKG_GPIO_ERROR_LOG_INTERVAL_MS   5000U        // IO错误日志最小间隔

/****************************** 灯语常量 ******************************/

#define LINKG_GPIO_RUN_AP_PERIOD_MS        2000U        // AP运行灯周期：2000ms
#define LINKG_GPIO_RUN_AP_ON_MS            1500U        // AP运行灯亮1500ms，灭500ms
#define LINKG_GPIO_RUN_STA_PERIOD_MS       1000U        // STA运行灯周期：1000ms
#define LINKG_GPIO_RUN_STA_ON_MS           300U         // STA运行灯亮300ms，灭700ms
#define LINKG_GPIO_LED_SLOT_MS             100U         // 快速闪烁单次亮灯时间
#define LINKG_GPIO_LED_FAST_PERIOD_MS      200U         // 快速闪烁周期：亮100ms，灭100ms

/****************************** 复位常量 ******************************/

#define LINKG_GPIO_BUTTON_DEBOUNCE_SAMPLES 2U           // 连续两个一致样本确认状态改变
#define LINKG_GPIO_BUTTON_RESTORE_MIN_MS   5000U        // 长按5秒后松手可恢复
#define LINKG_GPIO_BUTTON_RESTORE_MAX_MS   10000U       // 达到10秒则本次按键失效

/****************************** 内部类型 ******************************/

typedef enum
{
    LINKG_GPIO_STATE_UNINITIALIZED = 0, // 尚未申请硬件资源
    LINKG_GPIO_STATE_INITIALIZED,       // GPIO已申请，管理线程未运行
    LINKG_GPIO_STATE_RUNNING,           // 管理线程正在运行
    LINKG_GPIO_STATE_STOPPING           // 正在停止管理线程
} linkg_gpio_manager_state_t;

typedef enum
{
    LINKG_GPIO_LED_OFF = 0, // 熄灭
    LINKG_GPIO_LED_RED,     // 红色
    LINKG_GPIO_LED_YELLOW,  // 黄色
    LINKG_GPIO_LED_GREEN,   // 绿色
    LINKG_GPIO_LED_BLUE,    // 蓝色
    LINKG_GPIO_LED_WHITE,   // 白色
    LINKG_GPIO_LED_RED_BLINK // 红色快速闪烁
} linkg_gpio_led_level_t;

typedef struct
{
    linux_gpio_t *red;    // 红色控制IO
    linux_gpio_t *green;  // 绿色控制IO
    linux_gpio_t *blue;   // 蓝色控制IO
    bool          last_r; // 最近一次成功输出的红色状态
    bool          last_g; // 最近一次成功输出的绿色状态
    bool          last_b; // 最近一次成功输出的蓝色状态
    bool          valid;  // 缓存是否有效
} linkg_gpio_rgb_t;

typedef struct
{
    bool     stable_pressed;       // 消抖后的按下状态
    bool     candidate_pressed;    // 正在等待确认的按键状态
    uint8_t  candidate_count;      // 候选状态连续采样次数
    bool     blocked_until_release;// 读取失败后禁止本次按压生效
    bool     over_time;            // 持续按下超过最大有效时长
    uint64_t started_ms;           // 确认按下的单调时间戳
} linkg_gpio_button_t;

typedef struct
{
    pthread_mutex_t            lock;                 // GPIO句柄及运行状态互斥锁
    linkg_thread_t             thread;               // 100ms管理线程
    linkg_gpio_manager_state_t state;                // 当前生命周期
    linkg_device_role_t        role;                 // AP/STA运行灯模式
    linux_gpio_t              *sbus;                 // PD16：MCU_MODEL（可选）
    linux_gpio_t              *reset_key;            // PD8：KEY_1
    linux_gpio_t              *hwid1;                // PB8：硬件版本位1
    linux_gpio_t              *hwid2;                // PB9：硬件版本位2
    linux_gpio_t              *sys_run;              // PD1：运行灯
    linkg_gpio_rgb_t           wifi;                 // Wi-Fi RSSI RGB
    linkg_gpio_rgb_t           cellular;             // 蜂窝 RSSI RGB
    linkg_gpio_led_level_t     wifi_level;           // Wi-Fi最新灯语
    linkg_gpio_led_level_t     cellular_level;       // 蜂窝最新灯语
    linkg_gpio_button_t        button;               // 长按复位状态机
    uint64_t                   led_origin_ms;        // 运行灯相位起始时刻
    uint64_t                   last_log_ms;          // GPIO错误节流时间
    bool                       run_led_valid;        // 运行灯输出缓存有效
    bool                       run_led_on;           // 最近一次成功的运行灯状态
} linkg_gpio_manager_context_t;

/****************************** 全局上下文 ******************************/

static linkg_gpio_manager_context_t g_gpio =
{
    .lock  = PTHREAD_MUTEX_INITIALIZER,
    .state = LINKG_GPIO_STATE_UNINITIALIZED
};

/****************************** 硬件辅助 ******************************/

/**
 * @brief 按给定电平申请一个引脚，申请失败时输出引脚名称。
 */
static int _linkg_gpio_open_pin(const char *name, linux_gpio_direction_t direction, bool initial_value, linux_gpio_t **gpio)
{
    int ret;

    ret = linux_gpio_open(name, direction, initial_value, gpio);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("GPIO: open %s failed, error=%d", name, ret);
    }

    return ret;
}

/**
 * @brief 关闭指定GPIO句柄，并清空调用方的资源指针。
 */
static void _linkg_gpio_close_pin(linux_gpio_t **gpio)
{
    if (gpio == NULL || *gpio == NULL)
    {
        return;
    }

    linux_gpio_close(*gpio);
    *gpio = NULL;
}

/**
 * @brief 释放所有引脚，由持锁的生命周期函数调用。
 */
static void _linkg_gpio_release_locked(void)
{
    _linkg_gpio_close_pin(&g_gpio.sbus);
    _linkg_gpio_close_pin(&g_gpio.reset_key);
    _linkg_gpio_close_pin(&g_gpio.hwid1);
    _linkg_gpio_close_pin(&g_gpio.hwid2);
    _linkg_gpio_close_pin(&g_gpio.sys_run);
    _linkg_gpio_close_pin(&g_gpio.wifi.red);
    _linkg_gpio_close_pin(&g_gpio.wifi.green);
    _linkg_gpio_close_pin(&g_gpio.wifi.blue);
    _linkg_gpio_close_pin(&g_gpio.cellular.red);
    _linkg_gpio_close_pin(&g_gpio.cellular.green);
    _linkg_gpio_close_pin(&g_gpio.cellular.blue);

    g_gpio.wifi.valid     = false;
    g_gpio.cellular.valid = false;
    g_gpio.run_led_valid  = false;
}

/**
 * @brief 按低电平点亮规则更新RGB，只有输出状态变化时才访问Sysfs。
 */
static int _linkg_gpio_write_rgb_locked(linkg_gpio_rgb_t *rgb, bool red, bool green, bool blue)
{
    int first_error;
    int ret;

    first_error = 0;

    if (!rgb->valid || rgb->last_r != red)
    {
        ret = linux_gpio_write(rgb->red, !red);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (!rgb->valid || rgb->last_g != green)
    {
        ret = linux_gpio_write(rgb->green, !green);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (!rgb->valid || rgb->last_b != blue)
    {
        ret = linux_gpio_write(rgb->blue, !blue);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (first_error == 0)
    {
        rgb->last_r = red;
        rgb->last_g = green;
        rgb->last_b = blue;
        rgb->valid  = true;
    }
    else
    {
        rgb->valid = false;
    }

    return first_error;
}

/**
 * @brief 更新系统运行灯，低电平点亮。
 */
static int _linkg_gpio_write_run_locked(bool on)
{
    int ret;

    if (g_gpio.run_led_valid && g_gpio.run_led_on == on)
    {
        return 0;
    }

    ret = linux_gpio_write(g_gpio.sys_run, !on);
    if (ret == 0)
    {
        g_gpio.run_led_on    = on;
        g_gpio.run_led_valid = true;
    }
    else
    {
        g_gpio.run_led_valid = false;
    }

    return ret;
}

/**
 * @brief 统一恢复GPIO安全输出状态，调用前必须持有状态锁。
 */
static int _linkg_gpio_safe_outputs_locked(void)
{
    int first_error;
    int ret;

    first_error = 0;

    ret = _linkg_gpio_write_rgb_locked(&g_gpio.wifi, false, false, false);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_gpio_write_rgb_locked(&g_gpio.cellular, false, false, false);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_gpio_write_run_locked(false);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    if (g_gpio.sbus != NULL)
    {
        ret = linux_gpio_write(g_gpio.sbus, false);
        if (ret != 0 && first_error == 0)
        {
            first_error = ret;
        }
    }

    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/****************************** 灯语转换 ******************************/

/**
 * @brief 将有效Wi-Fi RSSI映射为对应宽窄带颜色档。
 */
static linkg_gpio_led_level_t _linkg_gpio_wifi_rssi_level(int32_t rssi, bool wideband)
{
    if (wideband)
    {
        if (rssi >= -50)
        {
            return LINKG_GPIO_LED_WHITE;
        }
        if (rssi >= -60)
        {
            return LINKG_GPIO_LED_BLUE;
        }
        if (rssi >= -75)
        {
            return LINKG_GPIO_LED_GREEN;
        }
        if (rssi >= -85)
        {
            return LINKG_GPIO_LED_YELLOW;
        }
        return LINKG_GPIO_LED_RED;
    }

    if (rssi >= -60)
    {
        return LINKG_GPIO_LED_WHITE;
    }
    if (rssi >= -75)
    {
        return LINKG_GPIO_LED_BLUE;
    }
    if (rssi >= -85)
    {
        return LINKG_GPIO_LED_GREEN;
    }
    if (rssi >= -95)
    {
        return LINKG_GPIO_LED_YELLOW;
    }
    return LINKG_GPIO_LED_RED;
}

/**
 * @brief 将Web界面定义的蜂窝RSRP阈值映射成颜色档。
 */
static linkg_gpio_led_level_t _linkg_gpio_cellular_rsrp_level(int32_t rsrp)
{
    if (rsrp >= -80)
    {
        return LINKG_GPIO_LED_WHITE;
    }
    if (rsrp >= -90)
    {
        return LINKG_GPIO_LED_BLUE;
    }
    if (rsrp >= -100)
    {
        return LINKG_GPIO_LED_GREEN;
    }
    if (rsrp >= -110)
    {
        return LINKG_GPIO_LED_YELLOW;
    }
    return LINKG_GPIO_LED_RED;
}

/**
 * @brief 检查最近一次采样时间戳是否有效且未过期。
 */
static bool _linkg_gpio_is_fresh(uint64_t updated_ms, uint64_t now_ms, uint64_t max_age_ms)
{
    return updated_ms != 0U && now_ms >= updated_ms && now_ms - updated_ms <= max_age_ms;
}

/**
 * @brief 检查Wi-Fi对端RSSI是否具有有效、未过期的驱动统计。
 *
 * RSSI允许为0dBm，但必须确认对端已关联且统计有效。
 * -127为驱动使用的无效RSSI标记。
 */
static bool _linkg_gpio_peer_has_rssi(const linkg_wifi_peer_status_t *peer, uint64_t now_ms)
{
    return peer != NULL &&
           peer->valid &&
           peer->state == LINKG_WIFI_PEER_STATE_CONNECTED &&
           peer->statistics_valid &&
           _linkg_gpio_is_fresh(peer->statistics_updated_ms, now_ms, LINKG_GPIO_WIFI_MAX_AGE_MS) &&
           peer->rssi_dbm > -127 &&
           peer->rssi_dbm < 0;
}

/****************************** Wi-Fi状态 ******************************/

/**
 * @brief 根据Node直接对端状态和Wi-Fi运行快照计算RSSI灯语。
 *
 * Node负责确认LinkG直接对端是否在线。
 * Wi-Fi负责确认物理关联状态和RSSI有效性。
 *
 * STA使用唯一AP的RSSI。
 * AP使用当前有效关联STA中的最低RSSI。
 *
 * 没有在线对端或Wi-Fi未关联时红色闪烁。
 * 对端已关联但RSSI统计无效时红色常亮。
 */
static linkg_gpio_led_level_t _linkg_gpio_query_wifi_level(linkg_device_role_t role, uint64_t now_ms)
{
    linkg_wifi_status_snapshot_t   snapshot;
    linkg_wifi_config_t            config;
    const linkg_wifi_peer_status_t *peer;
    uint32_t                       node_peer_count;
    bool                           has_rssi;
    bool                           wideband;
    int32_t                        minimum_rssi;
    size_t                         index;
    int                            ret;

    ret = linkg_config_get_wifi(&config);
    if (ret != 0)
    {
        return LINKG_GPIO_LED_RED;
    }

    if (!config.enabled)
    {
        return LINKG_GPIO_LED_OFF;
    }

    memset(&snapshot, 0, sizeof(snapshot));

    ret = linkg_wifi_get_status(&snapshot);
    if (ret != 0)
    {
        return LINKG_GPIO_LED_RED_BLINK;
    }

    now_ms = linkg_time_elapsed_ms();

    if (snapshot.local.role != role ||
        !_linkg_gpio_is_fresh(snapshot.local.updated_ms, now_ms, LINKG_GPIO_WIFI_MAX_AGE_MS))
    {
        return LINKG_GPIO_LED_RED;
    }

    if (snapshot.local.radio.work_mode != LINKG_WIFI_WORK_MODE_WIDE &&
        snapshot.local.radio.work_mode != LINKG_WIFI_WORK_MODE_NARROW)
    {
        return LINKG_GPIO_LED_RED;
    }

    wideband = snapshot.local.radio.work_mode == LINKG_WIFI_WORK_MODE_WIDE;

    // Node模块统一管理AP/STA直接对端的注册与注销。
    node_peer_count = 0U;

    ret = linkg_node_get_peer_count(&node_peer_count);
    if (ret != 0)
    {
        return LINKG_GPIO_LED_RED;
    }

    if (node_peer_count == 0U)
    {
        return LINKG_GPIO_LED_RED_BLINK;
    }

    // STA只有一个直接AP，读取其当前物理关联状态和RSSI。
    if (role == LINKG_DEVICE_ROLE_STA)
    {
        peer = &snapshot.role.sta.peer;

        if (!peer->valid || peer->state != LINKG_WIFI_PEER_STATE_CONNECTED)
        {
            return LINKG_GPIO_LED_RED_BLINK;
        }

        if (!_linkg_gpio_peer_has_rssi(peer, now_ms))
        {
            return LINKG_GPIO_LED_RED;
        }

        return _linkg_gpio_wifi_rssi_level(peer->rssi_dbm, wideband);
    }

    if (role != LINKG_DEVICE_ROLE_AP)
    {
        return LINKG_GPIO_LED_RED;
    }

    // AP没有任何物理关联STA，表示当前Wi-Fi不存在连接。
    if (snapshot.role.ap.connected_count == 0U)
    {
        return LINKG_GPIO_LED_RED_BLINK;
    }

    // AP选取所有有效关联STA中信号最差的RSSI。
    has_rssi     = false;
    minimum_rssi = 0;

    for (index = 0U; index < snapshot.role.ap.peer_count && index < LINKG_WIFI_AP_PEER_MAX; index++)
    {
        peer = &snapshot.role.ap.peers[index];

        if (!_linkg_gpio_peer_has_rssi(peer, now_ms))
        {
            continue;
        }

        if (!has_rssi || peer->rssi_dbm < minimum_rssi)
        {
            minimum_rssi = peer->rssi_dbm;
            has_rssi     = true;
        }
    }

    if (!has_rssi)
    {
        return LINKG_GPIO_LED_RED;
    }

    return _linkg_gpio_wifi_rssi_level(minimum_rssi, wideband);
}

/****************************** 蜂窝状态 ******************************/

/**
 * @brief 使用Cellular健康状态与RSRP快照计算蜂窝RSSI灯语。
 *
 * IPv6公网可达是正常常亮的必要条件。IPv4单独不可达不触发红闪。
 */
static linkg_gpio_led_level_t _linkg_gpio_query_cellular_level(uint64_t now_ms)
{
    linkg_cellular_status_snapshot_t snapshot;
    linkg_cellular_config_t          config;
    bool                            data_ready;
    bool                            ipv6_available;
    int                             ret;

    ret = linkg_config_get_cellular(&config);
    if (ret != 0)
    {
        return LINKG_GPIO_LED_RED;
    }

    if (!config.enabled)
    {
        return LINKG_GPIO_LED_OFF;
    }

    data_ready      = false;
    ipv6_available  = false;

    ret = linkg_cellular_get_internet_available(LINKG_CELLULAR_AVAILABLE_DATA, &data_ready);
    if (ret != 0 || !data_ready)
    {
        return LINKG_GPIO_LED_RED_BLINK;
    }

    ret = linkg_cellular_get_internet_available(LINKG_CELLULAR_AVAILABLE_IPV6, &ipv6_available);
    if (ret != 0 || !ipv6_available)
    {
        return LINKG_GPIO_LED_RED_BLINK;
    }

    memset(&snapshot, 0, sizeof(snapshot));
    ret = linkg_cellular_get_status(&snapshot);
    if (ret != 0)
    {
        return LINKG_GPIO_LED_RED;
    }

    now_ms = linkg_time_elapsed_ms();

    if (!snapshot.network.serving_cell.valid ||
        !snapshot.network.serving_cell.rsrp_valid ||
        !_linkg_gpio_is_fresh(snapshot.network.serving_cell.updated_ms, now_ms, LINKG_GPIO_CELLULAR_MAX_AGE_MS) ||
        snapshot.network.serving_cell.rsrp_dbm < -150 ||
        snapshot.network.serving_cell.rsrp_dbm >= 0)
    {
        return LINKG_GPIO_LED_RED;
    }

    return _linkg_gpio_cellular_rsrp_level(snapshot.network.serving_cell.rsrp_dbm);
}

/****************************** 按键检测 ******************************/

/**
 * @brief 重置按键计时，不允许在本次读取失败后误触发恢复。
 */
static void _linkg_gpio_button_read_failed_locked(void)
{
    memset(&g_gpio.button, 0, sizeof(g_gpio.button));
    g_gpio.button.blocked_until_release = true;
}

/**
 * @brief 更新带消抖和超时保护的长按状态，松手确认后返回true一次。
 */
static bool _linkg_gpio_check_button_locked(uint64_t now_ms)
{
    linkg_gpio_button_t *button;
    bool                 physical_pressed;
    uint64_t             held_ms;
    bool                 reset_requested;
    int                  ret;

    button = &g_gpio.button;
    ret = linux_gpio_read(g_gpio.reset_key, &physical_pressed);
    if (ret != 0)
    {
        _linkg_gpio_button_read_failed_locked();
        return false;
    }

    // KEY_1低电平有效。
    physical_pressed = !physical_pressed;

    if (button->blocked_until_release)
    {
        if (!physical_pressed)
        {
            memset(button, 0, sizeof(*button));
        }
        return false;
    }

    reset_requested = false;

    if (physical_pressed == button->stable_pressed)
    {
        button->candidate_count = 0U;
    }
    else
    {
        if (physical_pressed != button->candidate_pressed)
        {
            button->candidate_pressed = physical_pressed;
            button->candidate_count   = 1U;
        }
        else if (button->candidate_count < LINKG_GPIO_BUTTON_DEBOUNCE_SAMPLES)
        {
            button->candidate_count++;
        }

        if (button->candidate_count >= LINKG_GPIO_BUTTON_DEBOUNCE_SAMPLES)
        {
            button->candidate_count = 0U;
            button->stable_pressed  = physical_pressed;

            if (physical_pressed)
            {
                button->started_ms = now_ms;
                button->over_time  = false;
            }
            else
            {
                held_ms = now_ms - button->started_ms;
                reset_requested = !button->over_time &&
                                  held_ms >= LINKG_GPIO_BUTTON_RESTORE_MIN_MS &&
                                  held_ms < LINKG_GPIO_BUTTON_RESTORE_MAX_MS;
                button->started_ms = 0U;
                button->over_time  = false;
            }
        }
    }

    if (button->stable_pressed && now_ms - button->started_ms >= LINKG_GPIO_BUTTON_RESTORE_MAX_MS)
    {
        button->over_time = true;
    }

    return reset_requested;
}

/**
 * @brief 在按键有效长按窗口内使用系统运行灯快闪提示。
 */
static bool _linkg_gpio_button_fast_blink_locked(uint64_t now_ms)
{
    uint64_t held_ms;

    if (!g_gpio.button.stable_pressed || g_gpio.button.over_time)
    {
        return false;
    }

    held_ms = now_ms - g_gpio.button.started_ms;
    return held_ms >= LINKG_GPIO_BUTTON_RESTORE_MIN_MS &&
           held_ms < LINKG_GPIO_BUTTON_RESTORE_MAX_MS;
}

/****************************** 周期LED更新 ******************************/

/**
 * @brief 将灯语等级转换成RGB目标状态。
 */
static int _linkg_gpio_apply_level_locked(linkg_gpio_rgb_t *rgb, linkg_gpio_led_level_t level, uint64_t now_ms)
{
    bool red;
    bool green;
    bool blue;

    red   = false;
    green = false;
    blue  = false;

    switch (level)
    {
        case LINKG_GPIO_LED_RED:
            red = true;
            break;

        case LINKG_GPIO_LED_YELLOW:
            red   = true;
            green = true;
            break;

        case LINKG_GPIO_LED_GREEN:
            green = true;
            break;

        case LINKG_GPIO_LED_BLUE:
            blue = true;
            break;

        case LINKG_GPIO_LED_WHITE:
            red   = true;
            green = true;
            blue  = true;
            break;

        case LINKG_GPIO_LED_RED_BLINK:
            red = (now_ms % LINKG_GPIO_LED_FAST_PERIOD_MS) < LINKG_GPIO_LED_SLOT_MS;
            break;

        case LINKG_GPIO_LED_OFF:
        default:
            break;
    }

    return _linkg_gpio_write_rgb_locked(rgb, red, green, blue);
}

/**
 * @brief 应用AP长亮短灭、STA短亮长灭及复位按键快闪灯语。
 */
static int _linkg_gpio_apply_run_led_locked(uint64_t now_ms)
{
    uint64_t phase_ms;
    uint32_t period_ms;
    uint32_t on_ms;
    bool     on;

    // 有效长按期间优先显示复位提示，保持原有100ms快闪。
    if (_linkg_gpio_button_fast_blink_locked(now_ms))
    {
        on = (now_ms % LINKG_GPIO_LED_FAST_PERIOD_MS) < LINKG_GPIO_LED_SLOT_MS;
        return _linkg_gpio_write_run_locked(on);
    }

    if (g_gpio.role == LINKG_DEVICE_ROLE_AP)
    {
        period_ms = LINKG_GPIO_RUN_AP_PERIOD_MS;
        on_ms     = LINKG_GPIO_RUN_AP_ON_MS;
    }
    else
    {
        period_ms = LINKG_GPIO_RUN_STA_PERIOD_MS;
        on_ms     = LINKG_GPIO_RUN_STA_ON_MS;
    }

    phase_ms = (now_ms - g_gpio.led_origin_ms) % period_ms;
    on       = phase_ms < on_ms;

    return _linkg_gpio_write_run_locked(on);
}

/**
 * @brief 按照最新缓存的灯语更新三组LED，调用方持锁。
 */
static void _linkg_gpio_apply_leds_locked(uint64_t now_ms)
{
    int first_error;
    int ret;

    first_error = 0;

    ret = _linkg_gpio_apply_run_led_locked(now_ms);
    if (ret != 0)
    {
        first_error = ret;
    }

    ret = _linkg_gpio_apply_level_locked(&g_gpio.wifi, g_gpio.wifi_level, now_ms);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_gpio_apply_level_locked(&g_gpio.cellular, g_gpio.cellular_level, now_ms);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    if (first_error != 0 &&
        (g_gpio.last_log_ms == 0U || now_ms - g_gpio.last_log_ms >= LINKG_GPIO_ERROR_LOG_INTERVAL_MS))
    {
        LINKG_LOG_WARN("GPIO: update LED failed, error=%d", first_error);
        g_gpio.last_log_ms = now_ms;
    }
}

/****************************** 后台线程 ******************************/

/**
 * @brief GPIO工作线程，所有外部状态查询均在不持有GPIO锁时完成。
 */
static void _linkg_gpio_worker(linkg_thread_t *thread, void *user_data)
{
    struct pollfd           descriptor;
    linkg_device_role_t     role;
    linkg_gpio_led_level_t  wifi_level;
    linkg_gpio_led_level_t  cellular_level;
    uint64_t                now_ms;
    uint64_t                last_refresh_ms;
    bool                    refresh_pending;
    bool                    restore_requested;
    unsigned int            attempt;
    int                     ret;

    (void)user_data;

    descriptor.fd      = linkg_thread_get_wakeup_fd(thread);
    descriptor.events  = POLLIN;
    descriptor.revents = 0;
    last_refresh_ms    = 0U;
    refresh_pending    = true;

    while (linkg_thread_is_running(thread))
    {
        if (linkg_lifecycle_is_exit_requested())
        {
            break;
        }

        now_ms = linkg_time_elapsed_ms();

        if (refresh_pending || now_ms < last_refresh_ms || now_ms - last_refresh_ms >= LINKG_GPIO_SIGNAL_INTERVAL_MS)
        {
            pthread_mutex_lock(&g_gpio.lock);
            role = g_gpio.role;
            pthread_mutex_unlock(&g_gpio.lock);

            wifi_level     = _linkg_gpio_query_wifi_level(role, now_ms);
            cellular_level = _linkg_gpio_query_cellular_level(now_ms);

            pthread_mutex_lock(&g_gpio.lock);
            if (g_gpio.state == LINKG_GPIO_STATE_RUNNING)
            {
                // 防止查询期间运行角色被外部接口更新后覆盖对应状态。
                refresh_pending = g_gpio.role != role;
                if (!refresh_pending)
                {
                    g_gpio.wifi_level = wifi_level;
                }

                g_gpio.cellular_level = cellular_level;
            }
            pthread_mutex_unlock(&g_gpio.lock);

            last_refresh_ms = now_ms;
        }

        restore_requested = false;

        pthread_mutex_lock(&g_gpio.lock);
        if (g_gpio.state == LINKG_GPIO_STATE_RUNNING)
        {
            restore_requested = _linkg_gpio_check_button_locked(now_ms);
            _linkg_gpio_apply_leds_locked(now_ms);
        }
        pthread_mutex_unlock(&g_gpio.lock);

        if (restore_requested && !linkg_lifecycle_is_exit_requested())
        {
            // 每次松手最多恢复两次；失败不触发重启，也不在按住期间重复触发。
            ret = -EIO;
            for (attempt = 0U; attempt < 2U; attempt++)
            {
                ret = linkg_config_restore_factory();
                if (ret == 0)
                {
                    break;
                }

                LINKG_LOG_WARN("GPIO: factory restore attempt %u failed, error=%d", attempt + 1U, ret);
            }

            if (ret == 0)
            {
                LINKG_LOG_WARN("GPIO: factory configuration restored, requesting system reboot");
                ret = linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_REBOOT_SYSTEM);
                if (ret != 0)
                {
                    LINKG_LOG_ERROR("GPIO: request system reboot failed, error=%d", ret);
                }
                break;
            }
        }

        if (!linkg_thread_is_running(thread))
        {
            break;
        }

        descriptor.revents = 0;
        do
        {
            ret = poll(&descriptor, 1U, (int)LINKG_GPIO_POLL_INTERVAL_MS);
        }
        while (ret < 0 && errno == EINTR && linkg_thread_is_running(thread));

        if (ret > 0 && (descriptor.revents & POLLIN) != 0)
        {
            (void)linkg_thread_clear_wakeup(thread);
        }
        else if (ret < 0 && linkg_thread_is_running(thread))
        {
            LINKG_LOG_WARN("GPIO: worker poll failed, error=%d", -errno);
            break;
        }
    }
}

/****************************** 生命周期 ******************************/

/**
 * @brief 申请全部GPIO并建立后台线程对象，不启动线程。
 */
int linkg_gpio_manager_init(void)
{
    int ret;

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state != LINKG_GPIO_STATE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return -EALREADY;
    }

    g_gpio.role = LINKG_DEVICE_ROLE_UNKNOWN;

    // SBUS控制为高电平有效，初始化默认输出低电平，关闭SBUS通路。
    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_SBUS_SWITCH, LINUX_GPIO_DIRECTION_OUTPUT, false, &g_gpio.sbus);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_LED_RUN, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.sys_run);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_WIFI_RSSI_RED, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.wifi.red);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_WIFI_RSSI_GREEN, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.wifi.green);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_WIFI_RSSI_BLUE, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.wifi.blue);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_CELLULAR_RSSI_RED, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.cellular.red);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_CELLULAR_RSSI_GREEN, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.cellular.green);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_CELLULAR_RSSI_BLUE, LINUX_GPIO_DIRECTION_OUTPUT, true, &g_gpio.cellular.blue);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_RESET_KEY, LINUX_GPIO_DIRECTION_INPUT, false, &g_gpio.reset_key);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_HWID1, LINUX_GPIO_DIRECTION_INPUT, false, &g_gpio.hwid1);
    if (ret != 0)
    {
        goto fail;
    }

    ret = _linkg_gpio_open_pin(LINKG_RESOURCE_GPIO_HWID2, LINUX_GPIO_DIRECTION_INPUT, false, &g_gpio.hwid2);
    if (ret != 0)
    {
        goto fail;
    }

    ret = linkg_thread_init(&g_gpio.thread, LINKG_GPIO_THREAD_NAME, _linkg_gpio_worker, NULL);
    if (ret != 0)
    {
        goto fail;
    }

    g_gpio.wifi_level     = LINKG_GPIO_LED_OFF;
    g_gpio.cellular_level = LINKG_GPIO_LED_OFF;
    g_gpio.state          = LINKG_GPIO_STATE_INITIALIZED;

    pthread_mutex_unlock(&g_gpio.lock);
    LINKG_LOG_INFO("GPIO: initialized, SBUS active high");
    return 0;

fail:
    _linkg_gpio_release_locked();
    g_gpio.state = LINKG_GPIO_STATE_UNINITIALIZED;
    pthread_mutex_unlock(&g_gpio.lock);
    return ret;
}

/**
 * @brief 读取当前设备角色并启动管理线程。
 */
int linkg_gpio_manager_start(void)
{
    linkg_device_config_t device;
    int                   ret;

    ret = linkg_config_get_device(&device);
    if (ret != 0)
    {
        return ret;
    }

    if (device.role != LINKG_DEVICE_ROLE_AP && device.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state != LINKG_GPIO_STATE_INITIALIZED)
    {
        ret = g_gpio.state == LINKG_GPIO_STATE_RUNNING ? -EALREADY : -ENODEV;
        pthread_mutex_unlock(&g_gpio.lock);
        return ret;
    }

    memset(&g_gpio.button, 0, sizeof(g_gpio.button));
    g_gpio.role             = device.role;
    g_gpio.led_origin_ms    = linkg_time_elapsed_ms();
    g_gpio.wifi_level       = LINKG_GPIO_LED_OFF;
    g_gpio.cellular_level   = LINKG_GPIO_LED_OFF;
    g_gpio.state            = LINKG_GPIO_STATE_RUNNING;

    pthread_mutex_unlock(&g_gpio.lock);

    ret = linkg_thread_start(&g_gpio.thread);
    if (ret != 0)
    {
        pthread_mutex_lock(&g_gpio.lock);
        g_gpio.state = LINKG_GPIO_STATE_INITIALIZED;
        (void)_linkg_gpio_safe_outputs_locked();
        pthread_mutex_unlock(&g_gpio.lock);
        return ret;
    }

    LINKG_LOG_INFO("GPIO: started, role=%d", (int)device.role);
    return 0;
}

/**
 * @brief 停止管理线程并把全部控制引脚恢复为安全电平。
 */
int linkg_gpio_manager_stop(void)
{
    int ret;
    int safe_ret;

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state == LINKG_GPIO_STATE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return 0;
    }

    if (g_gpio.state == LINKG_GPIO_STATE_INITIALIZED)
    {
        safe_ret = _linkg_gpio_safe_outputs_locked();
        pthread_mutex_unlock(&g_gpio.lock);
        return safe_ret;
    }

    g_gpio.state = LINKG_GPIO_STATE_STOPPING;
    pthread_mutex_unlock(&g_gpio.lock);

    ret = linkg_thread_stop(&g_gpio.thread);
    if (ret != 0)
    {
        return ret;
    }

    pthread_mutex_lock(&g_gpio.lock);
    safe_ret = _linkg_gpio_safe_outputs_locked();
    memset(&g_gpio.button, 0, sizeof(g_gpio.button));
    g_gpio.state = LINKG_GPIO_STATE_INITIALIZED;
    pthread_mutex_unlock(&g_gpio.lock);

    if (safe_ret != 0)
    {
        LINKG_LOG_WARN("GPIO: safe outputs failed during stop, error=%d", safe_ret);
    }

    LINKG_LOG_INFO("GPIO: stopped");
    return safe_ret;
}

/**
 * @brief 反初始化GPIO线程与全部GPIO硬件资源。
 */
int linkg_gpio_manager_deinit(void)
{
    int ret;

    ret = linkg_gpio_manager_stop();
    if (ret != 0 && linkg_thread_is_started(&g_gpio.thread))
    {
        return ret;
    }

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state == LINKG_GPIO_STATE_UNINITIALIZED)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return 0;
    }

    if (g_gpio.state != LINKG_GPIO_STATE_INITIALIZED)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return -EBUSY;
    }

    linkg_thread_deinit(&g_gpio.thread);
    _linkg_gpio_release_locked();

    g_gpio.state             = LINKG_GPIO_STATE_UNINITIALIZED;
    g_gpio.role              = LINKG_DEVICE_ROLE_UNKNOWN;
    g_gpio.wifi_level        = LINKG_GPIO_LED_OFF;
    g_gpio.cellular_level    = LINKG_GPIO_LED_OFF;
    memset(&g_gpio.button, 0, sizeof(g_gpio.button));

    pthread_mutex_unlock(&g_gpio.lock);
    LINKG_LOG_INFO("GPIO: deinitialized");
    return ret;
}

/****************************** 运行配置 ******************************/

/**
 * @brief 外部通知AP/STA角色变化，重置运行灯相位。
 */
int linkg_gpio_manager_set_role(linkg_device_role_t role)
{
    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state != LINKG_GPIO_STATE_INITIALIZED && g_gpio.state != LINKG_GPIO_STATE_RUNNING)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return -ENODEV;
    }

    g_gpio.role          = role;
    g_gpio.led_origin_ms = linkg_time_elapsed_ms();
    g_gpio.wifi_level    = LINKG_GPIO_LED_RED_BLINK;

    pthread_mutex_unlock(&g_gpio.lock);
    return 0;
}

/****************************** 硬件控制 ******************************/

/**
 * @brief 将逻辑SBUS使能转换成PD16的MCU_MODEL电平。
 */
int linkg_gpio_manager_set_sbus_enabled(bool enabled)
{
    int ret;

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state != LINKG_GPIO_STATE_INITIALIZED && g_gpio.state != LINKG_GPIO_STATE_RUNNING)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return -ENODEV;
    }

    if (g_gpio.sbus == NULL)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return -ENODEV;
    }

    ret = linux_gpio_write(g_gpio.sbus, enabled);

    pthread_mutex_unlock(&g_gpio.lock);
    return ret;
}

/****************************** 硬件版本 ******************************/

/**
 * @brief 分别读取只读硬件版本引脚PB8、PB9。
 */
int linkg_gpio_manager_get_hardware_bits(bool *hwid1, bool *hwid2)
{
    bool first;
    bool second;
    int  ret;

    if (hwid1 == NULL || hwid2 == NULL)
    {
        return -EINVAL;
    }

    pthread_mutex_lock(&g_gpio.lock);

    if (g_gpio.state != LINKG_GPIO_STATE_INITIALIZED && g_gpio.state != LINKG_GPIO_STATE_RUNNING)
    {
        pthread_mutex_unlock(&g_gpio.lock);
        return -ENODEV;
    }

    ret = linux_gpio_read(g_gpio.hwid1, &first);
    if (ret == 0)
    {
        ret = linux_gpio_read(g_gpio.hwid2, &second);
    }

    if (ret == 0)
    {
        *hwid1 = first;
        *hwid2 = second;
    }

    pthread_mutex_unlock(&g_gpio.lock);
    return ret;
}

/**
 * @brief 合并硬件版本编码，bit1=HWID1，bit0=HWID2。
 */
int linkg_gpio_manager_get_hardware_id(uint8_t *hardware_id)
{
    bool first;
    bool second;
    int  ret;

    if (hardware_id == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_gpio_manager_get_hardware_bits(&first, &second);
    if (ret != 0)
    {
        return ret;
    }

    *hardware_id = (uint8_t)((first ? 2U : 0U) | (second ? 1U : 0U));
    return 0;
}
