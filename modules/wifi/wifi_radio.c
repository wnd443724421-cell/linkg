/**
 * @file wifi_radio.c
 * @brief LinkG Wi-Fi无线参数运行维护实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-26
 */

#include "wifi_radio.h"

#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "wifi_driver_ops.h"
#include "wifi_internal.h"
#include "wifi_status.h"
#include "wifi_status_query.h"

/****************************** 维护参数 ******************************/

#define WIFI_RADIO_CHECK_INTERVAL_MS        1000U  // 无线参数状态检查周期
#define WIFI_RADIO_FIXED_STABILIZE_MS       300U   // STA连接后固定速率稳定等待时间
#define WIFI_RADIO_MISMATCH_LIMIT           3U     // 连续速率不一致纠正阈值
#define WIFI_RADIO_REAPPLY_COOLDOWN_MS      3000U  // 无线参数重新应用后的冷却时间
#define WIFI_RADIO_STATUS_FRESH_MAX_AGE_MS  2500U  // 用于参数收敛的状态最大年龄

/****************************** 内部状态 ******************************/

typedef enum
{
    WIFI_RADIO_STATE_IDLE = 0,    // 当前无需无线参数维护
    WIFI_RADIO_STATE_AUTO,        // STA固定窄带断开态已恢复自动速率
    WIFI_RADIO_STATE_AUTO_RETRY,  // 恢复自动速率失败，等待重试
    WIFI_RADIO_STATE_STABILIZING, // STA连接后等待稳定再锁定固定速率
    WIFI_RADIO_STATE_FIXED,       // 当前处于固定窄带速率维护
    WIFI_RADIO_STATE_COOLDOWN     // 无线参数刚被重新应用，暂缓重复纠正
} wifi_radio_state_t;

typedef struct
{
    linkg_device_role_t      role;              // 当前设备角色
    linkg_wifi_work_mode_t   work_mode;         // 当前宽窄带工作模式
    linkg_wifi_narrow_mode_t narrow_mode;       // 窄带速率控制模式
    uint16_t                 target_rate_level; // 固定窄带目标速率档位
} wifi_radio_config_t;

typedef struct
{
    wifi_radio_config_t       config;                                               // 无线运行维护配置
    wifi_runtime_link_state_t last_link_state;                                      // 上次同步的STA连接状态
    wifi_radio_state_t        state;                                                // 当前无线参数维护状态
    uint64_t                  next_check_ms;                                        // 下一次维护动作期限
    uint8_t                   ap_peer_count;                                        // 上次观察到的AP对端数量
    uint8_t                   ap_peer_macs[LINKG_WIFI_AP_PEER_MAX][LINKG_WIFI_MAC_LENGTH]; // AP对端MAC集合
    uint8_t                   mismatch_count;                                       // 连续固定速率不一致次数
    uint8_t                   peer_refresh_failure_count;                           // 连续功率表刷新失败次数
    uint8_t                   rate_repair_failure_count;                            // 连续固定速率恢复失败次数
    bool                      initialized;                                          // 模块是否已经初始化
    bool                      started;                                              // 模块是否已经启动
} wifi_radio_context_t;

/****************************** 全局上下文 ******************************/

static wifi_radio_context_t g_wifi_radio;

/****************************** 配置辅助 ******************************/

/**
 * @brief 判断当前配置是否启用窄带无线维护。
 */
static bool _wifi_radio_narrow_enabled(void)
{
    return g_wifi_radio.config.work_mode == LINKG_WIFI_WORK_MODE_NARROW;
}

/**
 * @brief 判断当前配置是否启用固定窄带速率维护。
 */
static bool _wifi_radio_fixed_enabled(void)
{
    return _wifi_radio_narrow_enabled() &&
           g_wifi_radio.config.narrow_mode == LINKG_WIFI_NARROW_MODE_FIXED;
}

/**
 * @brief 从Wi-Fi配置提取无线运行维护所需字段。
 */
static int _wifi_radio_config_set(wifi_radio_config_t *radio, linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    if (radio == NULL || config == NULL)
    {
        return -EINVAL;
    }

    if (role != LINKG_DEVICE_ROLE_AP && role != LINKG_DEVICE_ROLE_STA)
    {
        return -EINVAL;
    }

    if (config->wideband.work_mode != LINKG_WIFI_WORK_MODE_NARROW &&
        config->wideband.work_mode != LINKG_WIFI_WORK_MODE_WIDE)
    {
        return -EINVAL;
    }

    memset(radio, 0, sizeof(*radio));

    radio->role      = role;
    radio->work_mode = config->wideband.work_mode;

    if (radio->work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        if (config->wideband.narrow_params.mode != LINKG_WIFI_NARROW_MODE_FIXED &&
            config->wideband.narrow_params.mode != LINKG_WIFI_NARROW_MODE_ADAPTIVE)
        {
            return -EINVAL;
        }

        if (config->wideband.narrow_params.manual_rate > LINKG_WIFI_NARROW_RATE_MAX)
        {
            return -ERANGE;
        }

        radio->narrow_mode       = config->wideband.narrow_params.mode;
        radio->target_rate_level = config->wideband.narrow_params.manual_rate;
    }

    return 0;
}

/****************************** 时间辅助 ******************************/

/**
 * @brief 安排下一次无线参数维护动作并清除固定速率失配计数。
 */
static void _wifi_radio_schedule(uint64_t now_ms, uint32_t delay_ms)
{
    g_wifi_radio.mismatch_count = 0U;
    g_wifi_radio.next_check_ms  = now_ms + delay_ms;
}

/****************************** 驱动控制 ******************************/

/**
 * @brief 恢复STA关联阶段使用的自动窄带速率。
 */
static int _wifi_radio_apply_auto_rate(void)
{
    return wifi_driver_set_narrow_auto_rate(true);
}

/**
 * @brief 下发固定窄带速率，并在锁档失败时尽量恢复自动速率。
 */
static int _wifi_radio_apply_fixed_rate(void)
{
    int rollback_ret;
    int ret;

    if (g_wifi_radio.config.target_rate_level > UINT8_MAX)
    {
        return -ERANGE;
    }

    ret = wifi_driver_set_narrow_auto_rate(false);
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_driver_set_narrow_rate_level((uint8_t)g_wifi_radio.config.target_rate_level);
    if (ret == 0)
    {
        return 0;
    }

    rollback_ret = wifi_driver_set_narrow_auto_rate(true);
    if (rollback_ret != 0)
    {
        WIFI_WARN("restore auto narrow rate after fixed-rate failure failed, error=%d",
                  rollback_ret);
    }

    return ret;
}

/**
 * @brief 记录固定速率重新应用失败。
 */
static void _wifi_radio_record_rate_failure(int error)
{
    if (g_wifi_radio.rate_repair_failure_count < UINT8_MAX)
    {
        g_wifi_radio.rate_repair_failure_count++;
    }

    if (g_wifi_radio.rate_repair_failure_count == 1U)
    {
        WIFI_WARN("apply fixed narrow rate failed, target=%u, error=%d",
                  (unsigned int)g_wifi_radio.config.target_rate_level,
                  error);
        return;
    }

    WIFI_DEBUG("apply fixed narrow rate still failing, target=%u, consecutive=%u, error=%d",
               (unsigned int)g_wifi_radio.config.target_rate_level,
               (unsigned int)g_wifi_radio.rate_repair_failure_count,
               error);
}

/****************************** 状态读取 ******************************/

/**
 * @brief 获取当前可用于无线参数收敛的最新Wi-Fi状态。
 */
static int _wifi_radio_get_status(uint64_t now_ms, wifi_status_info_t *info)
{
    int ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    memset(info, 0, sizeof(*info));

    ret = wifi_status_get_info(info);
    if (ret != 0)
    {
        return ret;
    }

    if (!info->valid)
    {
        return -EAGAIN;
    }

    if (!wifi_status_snapshot_is_fresh(&info->snapshot,
                                       now_ms,
                                       WIFI_RADIO_STATUS_FRESH_MAX_AGE_MS))
    {
        return -EAGAIN;
    }

    if (info->snapshot.local.role != g_wifi_radio.config.role)
    {
        return -EPROTO;
    }

    if (info->snapshot.local.radio.work_mode != g_wifi_radio.config.work_mode)
    {
        return -EPROTO;
    }

    return 0;
}

/**
 * @brief 从统一状态快照读取当前实际窄带速率档位。
 */
static int _wifi_radio_get_current_rate(const wifi_status_info_t *info, uint16_t *rate_level)
{
    if (info == NULL || rate_level == NULL)
    {
        return -EINVAL;
    }

    if (!info->snapshot.local.radio.params.narrow.current_rate_valid)
    {
        return -ENODATA;
    }

    *rate_level = info->snapshot.local.radio.params.narrow.current_rate_level;

    return 0;
}

/****************************** AP对端维护 ******************************/

/**
 * @brief 判断指定AP对端是否已经存在于上次状态快照。
 */
static bool _wifi_radio_ap_peer_is_known(const uint8_t mac[LINKG_WIFI_MAC_LENGTH])
{
    uint8_t index;

    for (index = 0U; index < g_wifi_radio.ap_peer_count; index++)
    {
        if (memcmp(g_wifi_radio.ap_peer_macs[index],
                   mac,
                   LINKG_WIFI_MAC_LENGTH) == 0)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 保存当前AP对端集合。
 */
static void _wifi_radio_store_ap_peers(const linkg_wifi_ap_status_t *ap)
{
    uint8_t peer_count;
    uint8_t index;

    memset(g_wifi_radio.ap_peer_macs, 0, sizeof(g_wifi_radio.ap_peer_macs));

    peer_count = ap->peer_count;
    if (peer_count > LINKG_WIFI_AP_PEER_MAX)
    {
        peer_count = LINKG_WIFI_AP_PEER_MAX;
    }

    for (index = 0U; index < peer_count; index++)
    {
        if (ap->peers[index].valid)
        {
            memcpy(g_wifi_radio.ap_peer_macs[index],
                   ap->peers[index].mac,
                   LINKG_WIFI_MAC_LENGTH);
        }
    }

    g_wifi_radio.ap_peer_count = peer_count;
}

/**
 * @brief 同步AP对端集合，并在发现新对端时刷新窄带功率表。
 * @return 1表示发现新对端并刷新成功，0表示无新对端，负errno表示刷新失败
 */
static int _wifi_radio_sync_ap_peers(const wifi_status_info_t *info)
{
    const linkg_wifi_ap_status_t *ap;
    uint8_t                       peer_count;
    uint8_t                       index;
    bool                          new_peer_found;
    int                           ret;

    if (info == NULL)
    {
        return -EINVAL;
    }

    if (g_wifi_radio.config.role != LINKG_DEVICE_ROLE_AP)
    {
        return 0;
    }

    ap = &info->snapshot.role.ap;

    peer_count = ap->peer_count;
    if (peer_count > LINKG_WIFI_AP_PEER_MAX)
    {
        peer_count = LINKG_WIFI_AP_PEER_MAX;
    }

    new_peer_found = false;

    for (index = 0U; index < peer_count; index++)
    {
        if (ap->peers[index].valid &&
            !_wifi_radio_ap_peer_is_known(ap->peers[index].mac))
        {
            new_peer_found = true;
            break;
        }
    }

    if (new_peer_found)
    {
        ret = wifi_driver_refresh_narrow_power_table();
        if (ret != 0)
        {
            if (g_wifi_radio.peer_refresh_failure_count < UINT8_MAX)
            {
                g_wifi_radio.peer_refresh_failure_count++;
            }

            if (g_wifi_radio.peer_refresh_failure_count == 1U)
            {
                WIFI_WARN("refresh narrow power table for new AP peer failed, error=%d",
                          ret);
            }
            else
            {
                WIFI_DEBUG("refresh narrow power table still failing, consecutive=%u, error=%d",
                           (unsigned int)g_wifi_radio.peer_refresh_failure_count,
                           ret);
            }

            return ret;
        }

        g_wifi_radio.peer_refresh_failure_count = 0U;
        WIFI_DEBUG("narrow power table refreshed for new AP peer, peers=%u",
                   (unsigned int)peer_count);
    }

    _wifi_radio_store_ap_peers(ap);

    return new_peer_found ? 1 : 0;
}

/****************************** 固定速率维护 ******************************/

/**
 * @brief 校验并维护当前固定窄带速率。
 */
static int _wifi_radio_maintain_fixed_rate(const wifi_status_info_t *info, uint64_t now_ms)
{
    uint16_t current_rate;
    int      ret;

    ret = _wifi_radio_get_current_rate(info, &current_rate);
    if (ret != 0)
    {
        g_wifi_radio.mismatch_count = 0U;
        _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
        return 0;
    }

    if (current_rate == g_wifi_radio.config.target_rate_level)
    {
        g_wifi_radio.mismatch_count = 0U;
        g_wifi_radio.rate_repair_failure_count = 0U;
        _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
        return 0;
    }

    if (g_wifi_radio.mismatch_count < UINT8_MAX)
    {
        g_wifi_radio.mismatch_count++;
    }

    if (g_wifi_radio.mismatch_count < WIFI_RADIO_MISMATCH_LIMIT)
    {
        WIFI_DEBUG("narrow rate mismatch pending confirmation, target=%u, current=%u, consecutive=%u",
                   (unsigned int)g_wifi_radio.config.target_rate_level,
                   (unsigned int)current_rate,
                   (unsigned int)g_wifi_radio.mismatch_count);

        g_wifi_radio.next_check_ms = now_ms + WIFI_RADIO_CHECK_INTERVAL_MS;
        return 0;
    }

    ret = _wifi_radio_apply_fixed_rate();
    if (ret != 0)
    {
        _wifi_radio_record_rate_failure(ret);
        _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
        return ret;
    }

    g_wifi_radio.rate_repair_failure_count = 0U;
    g_wifi_radio.state                     = WIFI_RADIO_STATE_COOLDOWN;

    _wifi_radio_schedule(now_ms, WIFI_RADIO_REAPPLY_COOLDOWN_MS);

    WIFI_WARN("narrow rate mismatch corrected, target=%u, previous=%u",
              (unsigned int)g_wifi_radio.config.target_rate_level,
              (unsigned int)current_rate);

    return 0;
}

/****************************** STA运行状态 ******************************/

/**
 * @brief 根据STA连接状态更新无线参数目标状态。
 */
static int _wifi_radio_sync_sta_link(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    int power_save_ret;
    int ret;

    if (runtime->link_state == g_wifi_radio.last_link_state)
    {
        return 0;
    }

    g_wifi_radio.last_link_state = runtime->link_state;
    g_wifi_radio.mismatch_count  = 0U;

    if (runtime->link_state == WIFI_RUNTIME_LINK_DISCONNECTED)
    {
        if (!_wifi_radio_fixed_enabled())
        {
            g_wifi_radio.state         = WIFI_RADIO_STATE_IDLE;
            g_wifi_radio.next_check_ms = 0U;
            return 0;
        }

        ret = _wifi_radio_apply_auto_rate();
        if (ret != 0)
        {
            g_wifi_radio.state = WIFI_RADIO_STATE_AUTO_RETRY;
            _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
            WIFI_WARN("restore automatic narrow rate after STA disconnect failed, error=%d",
                      ret);
            return ret;
        }

        g_wifi_radio.state         = WIFI_RADIO_STATE_AUTO;
        g_wifi_radio.next_check_ms = 0U;

        WIFI_DEBUG("STA disconnected, narrow rate mode=auto");
        return 0;
    }

    if (runtime->link_state != WIFI_RUNTIME_LINK_CONNECTED)
    {
        return -EPROTO;
    }

    power_save_ret = wifi_driver_set_sta_power_save(false);
    if (power_save_ret != 0)
    {
        WIFI_WARN("disable STA power save after connection failed, error=%d",
                  power_save_ret);
    }

    if (!_wifi_radio_fixed_enabled())
    {
        g_wifi_radio.state         = WIFI_RADIO_STATE_IDLE;
        g_wifi_radio.next_check_ms = 0U;
        return power_save_ret;
    }

    g_wifi_radio.state = WIFI_RADIO_STATE_STABILIZING;
    _wifi_radio_schedule(now_ms, WIFI_RADIO_FIXED_STABILIZE_MS);

    WIFI_DEBUG("STA connected, fixed narrow rate pending stabilization, target=%u",
               (unsigned int)g_wifi_radio.config.target_rate_level);

    return power_save_ret;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化Wi-Fi无线参数运行维护模块。
 */
int wifi_radio_init(linkg_device_role_t role, const linkg_wifi_config_t *config)
{
    wifi_radio_config_t radio;
    int                 ret;

    if (g_wifi_radio.initialized)
    {
        return -EALREADY;
    }

    ret = _wifi_radio_config_set(&radio, role, config);
    if (ret != 0)
    {
        return ret;
    }

    memset(&g_wifi_radio, 0, sizeof(g_wifi_radio));

    g_wifi_radio.config          = radio;
    g_wifi_radio.last_link_state = WIFI_RUNTIME_LINK_NOT_APPLICABLE;
    g_wifi_radio.state           = WIFI_RADIO_STATE_IDLE;
    g_wifi_radio.initialized     = true;

    return 0;
}

/**
 * @brief 启动Wi-Fi无线参数运行维护模块。
 */
int wifi_radio_start(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    int ret;

    if (!g_wifi_radio.initialized)
    {
        return -ENODEV;
    }

    if (g_wifi_radio.started)
    {
        return -EALREADY;
    }

    if (runtime == NULL || runtime->role != g_wifi_radio.config.role)
    {
        return -EINVAL;
    }

    g_wifi_radio.started                   = true;
    g_wifi_radio.state                     = WIFI_RADIO_STATE_IDLE;
    g_wifi_radio.next_check_ms             = 0U;
    g_wifi_radio.ap_peer_count             = 0U;
    g_wifi_radio.mismatch_count            = 0U;
    g_wifi_radio.peer_refresh_failure_count = 0U;
    g_wifi_radio.rate_repair_failure_count  = 0U;
    g_wifi_radio.last_link_state            = WIFI_RUNTIME_LINK_NOT_APPLICABLE;

    memset(g_wifi_radio.ap_peer_macs, 0, sizeof(g_wifi_radio.ap_peer_macs));

    if (!_wifi_radio_narrow_enabled())
    {
        return 0;
    }

    if (g_wifi_radio.config.role == LINKG_DEVICE_ROLE_AP)
    {
        g_wifi_radio.state = _wifi_radio_fixed_enabled() ? WIFI_RADIO_STATE_FIXED : WIFI_RADIO_STATE_IDLE;

        _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
        return 0;
    }

    ret = _wifi_radio_sync_sta_link(runtime, now_ms);
    if (ret != 0)
    {
        g_wifi_radio.started          = false;
        g_wifi_radio.state            = WIFI_RADIO_STATE_IDLE;
        g_wifi_radio.next_check_ms    = 0U;
        g_wifi_radio.last_link_state  = WIFI_RUNTIME_LINK_NOT_APPLICABLE;
        return ret;
    }

    return 0;
}

/**
 * @brief 停止Wi-Fi无线参数运行维护模块。
 */
int wifi_radio_stop(void)
{
    if (!g_wifi_radio.initialized)
    {
        return 0;
    }

    if (!g_wifi_radio.started)
    {
        return 0;
    }

    g_wifi_radio.started          = false;
    g_wifi_radio.state            = WIFI_RADIO_STATE_IDLE;
    g_wifi_radio.next_check_ms    = 0U;
    g_wifi_radio.mismatch_count   = 0U;
    g_wifi_radio.last_link_state  = WIFI_RUNTIME_LINK_NOT_APPLICABLE;

    return 0;
}

/**
 * @brief 释放Wi-Fi无线参数运行维护模块。
 */
void wifi_radio_deinit(void)
{
    if (!g_wifi_radio.initialized)
    {
        return;
    }

    (void)wifi_radio_stop();
    memset(&g_wifi_radio, 0, sizeof(g_wifi_radio));
}

/****************************** 运行状态同步 ******************************/

/**
 * @brief 根据Owner维护的Wi-Fi运行状态更新无线参数目标状态。
 */
int wifi_radio_sync_runtime(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    if (!g_wifi_radio.initialized || !g_wifi_radio.started)
    {
        return -ENODEV;
    }

    if (runtime == NULL || runtime->role != g_wifi_radio.config.role)
    {
        return -EINVAL;
    }

    if (g_wifi_radio.config.role != LINKG_DEVICE_ROLE_STA)
    {
        return 0;
    }

    return _wifi_radio_sync_sta_link(runtime, now_ms);
}

/**
 * @brief 通知无线配置刚被Owner或服务启动流程重新应用。
 */
void wifi_radio_notify_reapplied(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    if (!g_wifi_radio.initialized ||
        !g_wifi_radio.started ||
        !_wifi_radio_narrow_enabled() ||
        runtime == NULL ||
        runtime->role != g_wifi_radio.config.role)
    {
        return;
    }

    g_wifi_radio.mismatch_count            = 0U;
    g_wifi_radio.peer_refresh_failure_count = 0U;

    if (g_wifi_radio.config.role == LINKG_DEVICE_ROLE_AP)
    {
        g_wifi_radio.ap_peer_count = 0U;
        memset(g_wifi_radio.ap_peer_macs, 0, sizeof(g_wifi_radio.ap_peer_macs));
    }

    if (g_wifi_radio.config.role == LINKG_DEVICE_ROLE_STA &&
        runtime->link_state == WIFI_RUNTIME_LINK_DISCONNECTED)
    {
        g_wifi_radio.state         = _wifi_radio_fixed_enabled()
            ? WIFI_RADIO_STATE_AUTO
            : WIFI_RADIO_STATE_IDLE;
        g_wifi_radio.next_check_ms = 0U;
        g_wifi_radio.last_link_state = runtime->link_state;
        return;
    }

    g_wifi_radio.state = WIFI_RADIO_STATE_COOLDOWN;
    _wifi_radio_schedule(now_ms, WIFI_RADIO_REAPPLY_COOLDOWN_MS);
}

/****************************** 定时处理 ******************************/

/**
 * @brief 获取下一次无线参数维护期限。
 */
uint64_t wifi_radio_get_deadline(void)
{
    if (!g_wifi_radio.initialized || !g_wifi_radio.started)
    {
        return 0U;
    }

    return g_wifi_radio.next_check_ms;
}

/**
 * @brief 处理所有已经到期的无线参数维护动作。
 */
int wifi_radio_process(const wifi_runtime_t *runtime, uint64_t now_ms)
{
    wifi_status_info_t info;
    int                peer_sync_ret;
    int                ret;

    if (!g_wifi_radio.initialized || !g_wifi_radio.started)
    {
        return -ENODEV;
    }

    if (runtime == NULL || runtime->role != g_wifi_radio.config.role)
    {
        return -EINVAL;
    }

    if (!_wifi_radio_narrow_enabled() ||
        g_wifi_radio.next_check_ms == 0U ||
        now_ms < g_wifi_radio.next_check_ms)
    {
        return 0;
    }

    if (g_wifi_radio.config.role == LINKG_DEVICE_ROLE_STA)
    {
        ret = _wifi_radio_sync_sta_link(runtime, now_ms);
        if (ret != 0)
        {
            return ret;
        }

        if (runtime->link_state != WIFI_RUNTIME_LINK_CONNECTED)
        {
            if (g_wifi_radio.state != WIFI_RADIO_STATE_AUTO_RETRY)
            {
                g_wifi_radio.next_check_ms = 0U;
                return 0;
            }

            ret = _wifi_radio_apply_auto_rate();
            if (ret != 0)
            {
                _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
                return ret;
            }

            g_wifi_radio.state         = WIFI_RADIO_STATE_AUTO;
            g_wifi_radio.next_check_ms = 0U;
            return 0;
        }

        if (!_wifi_radio_fixed_enabled())
        {
            g_wifi_radio.next_check_ms = 0U;
            return 0;
        }

        if (g_wifi_radio.state == WIFI_RADIO_STATE_STABILIZING)
        {
            ret = _wifi_radio_apply_fixed_rate();
            if (ret != 0)
            {
                _wifi_radio_record_rate_failure(ret);
                _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
                return ret;
            }

            g_wifi_radio.rate_repair_failure_count = 0U;
            g_wifi_radio.state                     = WIFI_RADIO_STATE_COOLDOWN;
            _wifi_radio_schedule(now_ms, WIFI_RADIO_REAPPLY_COOLDOWN_MS);

            WIFI_INFO("STA fixed narrow rate applied, target=%u",
                      (unsigned int)g_wifi_radio.config.target_rate_level);
            return 0;
        }

        if (g_wifi_radio.state == WIFI_RADIO_STATE_COOLDOWN)
        {
            g_wifi_radio.state = WIFI_RADIO_STATE_FIXED;
            _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
            return 0;
        }
    }

    ret = _wifi_radio_get_status(now_ms, &info);
    if (ret != 0)
    {
        g_wifi_radio.mismatch_count = 0U;
        _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);

        if (ret == -EAGAIN)
        {
            return 0;
        }

        return ret;
    }

    if (g_wifi_radio.config.role == LINKG_DEVICE_ROLE_AP)
    {
        peer_sync_ret = _wifi_radio_sync_ap_peers(&info);
        if (peer_sync_ret < 0)
        {
            _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
            return peer_sync_ret;
        }

        if (peer_sync_ret > 0 && _wifi_radio_fixed_enabled())
        {
            ret = _wifi_radio_apply_fixed_rate();
            if (ret != 0)
            {
                _wifi_radio_record_rate_failure(ret);
                _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
                return ret;
            }

            g_wifi_radio.rate_repair_failure_count = 0U;
            g_wifi_radio.state                     = WIFI_RADIO_STATE_COOLDOWN;
            _wifi_radio_schedule(now_ms, WIFI_RADIO_REAPPLY_COOLDOWN_MS);

            WIFI_INFO("AP narrow maintenance completed, trigger=new_peer, rate_mode=fixed, target=%u",
                      (unsigned int)g_wifi_radio.config.target_rate_level);
            return 0;
        }

        if (peer_sync_ret > 0)
        {
            g_wifi_radio.state = WIFI_RADIO_STATE_COOLDOWN;
            _wifi_radio_schedule(now_ms, WIFI_RADIO_REAPPLY_COOLDOWN_MS);

            WIFI_INFO("AP narrow maintenance completed, trigger=new_peer, rate_mode=auto");
            return 0;
        }

        if (g_wifi_radio.state == WIFI_RADIO_STATE_COOLDOWN)
        {
            g_wifi_radio.state = _wifi_radio_fixed_enabled()
                ? WIFI_RADIO_STATE_FIXED
                : WIFI_RADIO_STATE_IDLE;

            _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);
            return 0;
        }
    }

    if (_wifi_radio_fixed_enabled())
    {
        return _wifi_radio_maintain_fixed_rate(&info, now_ms);
    }

    _wifi_radio_schedule(now_ms, WIFI_RADIO_CHECK_INTERVAL_MS);

    return 0;
}
