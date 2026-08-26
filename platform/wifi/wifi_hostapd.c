/**
 * @file wifi_hostapd.c
 * @brief LinkG hostapd服务内部实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-26
 */

#define _POSIX_C_SOURCE               200809L                                            // 启用POSIX.1-2008接口

#include "wifi_hostapd.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "linkg_file.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"
#include "linkg_text.h"
#include "linkg_time.h"
#include "linkg_wifi_ops.h"
#include "wifi_driver_ops.h"
#include "wifi_service_internal.h"
#include "wpa_ctrl.h"

/****************************** 模块常量 ******************************/

#define WIFI_HOSTAPD_PROGRAM          "hostapd"                                          // hostapd程序名称
#define WIFI_HOSTAPD_TEMPLATE_DIR     "/app/current/wireless"                            // hostapd模板目录
#define WIFI_HOSTAPD_20MHZ_TEMPLATE   WIFI_HOSTAPD_TEMPLATE_DIR "/hostapd_20mhz.conf"    // 20MHz hostapd模板
#define WIFI_HOSTAPD_40MHZ_TEMPLATE   WIFI_HOSTAPD_TEMPLATE_DIR "/hostapd_40mhz.conf"    // 40MHz hostapd模板
#define WIFI_HOSTAPD_80MHZ_TEMPLATE   WIFI_HOSTAPD_TEMPLATE_DIR "/hostapd_80mhz.conf"    // 80MHz hostapd模板
#define WIFI_HOSTAPD_NARROW_TEMPLATE  WIFI_HOSTAPD_TEMPLATE_DIR "/hostapd_narrow.conf"   // 窄带hostapd模板
#define WIFI_HOSTAPD_RUNTIME_CONFIG   WIFI_SERVICE_RUNTIME_DIR "/hostapd.conf"           // hostapd运行配置
#define WIFI_HOSTAPD_CTRL_PATH        "/var/run/hostapd/" LINKG_RESOURCE_INTERFACE_WIFI  // hostapd控制套接字
#define WIFI_HOSTAPD_STATUS_REPLY_MAX 512U                                               // hostapd状态响应最大长度
#define WIFI_HOSTAPD_READY_WAIT_MS    8000U                                              // AP VAP整体就绪等待时间

/****************************** 全局状态 ******************************/

static pid_t g_hostapd_pid = (pid_t)-1; // hostapd子进程PID

/****************************** 带宽参数 ******************************/

/**
 * @brief 获取主信道对应的HT40能力配置。
 */
static const char *_wifi_hostapd_get_ht40_capab(uint16_t channel)
{
    switch (channel)
    {
        case 36U:
        case 44U:
        case 52U:
        case 60U:
        case 100U:
        case 108U:
        case 116U:
        case 124U:
        case 132U:
        case 140U:
        case 149U:
        case 157U:
        case 184U:
        case 192U:
            return "[HT40+]";

        case 40U:
        case 48U:
        case 56U:
        case 64U:
        case 104U:
        case 112U:
        case 120U:
        case 128U:
        case 136U:
        case 144U:
        case 153U:
        case 161U:
        case 188U:
        case 196U:
            return "[HT40-]";

        default:
            return NULL;
    }
}

/**
 * @brief 获取80MHz主信道对应的中心信道。
 */
static const char *_wifi_hostapd_get_80mhz_center_channel(uint16_t channel)
{
    if (channel >= 36U && channel <= 48U)
    {
        return "42";
    }

    if (channel >= 52U && channel <= 64U)
    {
        return "58";
    }

    if (channel >= 100U && channel <= 112U)
    {
        return "106";
    }

    if (channel >= 116U && channel <= 128U)
    {
        return "122";
    }

    if (channel >= 132U && channel <= 144U)
    {
        return "138";
    }

    if (channel >= 149U && channel <= 161U)
    {
        return "155";
    }

    if (channel >= 184U && channel <= 196U)
    {
        return "190";
    }

    return NULL;
}

/****************************** 配置生成 ******************************/

/**
 * @brief 获取当前AP配置对应的hostapd模板。
 */
static const char *_wifi_hostapd_get_template(const linkg_wifi_config_t *config)
{
    if (config == NULL)
    {
        return NULL;
    }

    if (config->wideband.work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        return WIFI_HOSTAPD_NARROW_TEMPLATE;
    }

    if (config->wideband.work_mode != LINKG_WIFI_WORK_MODE_WIDE)
    {
        return NULL;
    }

    switch (config->wideband.wide_params.ap_bandwidth)
    {
        case LINKG_WIFI_WIDE_BANDWIDTH_20_MHZ:
            return WIFI_HOSTAPD_20MHZ_TEMPLATE;

        case LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ:
            return WIFI_HOSTAPD_40MHZ_TEMPLATE;

        case LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ:
            return WIFI_HOSTAPD_80MHZ_TEMPLATE;

        default:
            return NULL;
    }
}

/**
 * @brief 更新hostapd带宽相关动态参数。
 */
static int _wifi_hostapd_update_bandwidth(char **content, size_t *length, const linkg_wifi_config_t *config)
{
    const char *ht_capab;
    const char *center_channel;
    int         ret;

    if (content == NULL)
    {
        return -EINVAL;
    }

    if (*content == NULL)
    {
        return -EINVAL;
    }

    if (length == NULL)
    {
        return -EINVAL;
    }

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->wideband.work_mode == LINKG_WIFI_WORK_MODE_NARROW)
    {
        return 0;
    }

    if (config->wideband.work_mode != LINKG_WIFI_WORK_MODE_WIDE)
    {
        return -EINVAL;
    }

    if (config->wideband.wide_params.ap_bandwidth == LINKG_WIFI_WIDE_BANDWIDTH_20_MHZ)
    {
        return 0;
    }

    if (config->wideband.wide_params.ap_bandwidth != LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ)
    {
        if (config->wideband.wide_params.ap_bandwidth != LINKG_WIFI_WIDE_BANDWIDTH_80_MHZ)
        {
            return -EINVAL;
        }
    }

    ht_capab = _wifi_hostapd_get_ht40_capab(config->ap.channel);
    if (ht_capab == NULL)
    {
        WIFI_SERVICE_ERROR("unsupported HT40 channel, channel=%u", (unsigned int)config->ap.channel);
        return -EINVAL;
    }

    ret = linkg_text_replace_key_value(content, length, "ht_capab", ht_capab);
    if (ret != 0)
    {
        return ret;
    }

    if (config->wideband.wide_params.ap_bandwidth == LINKG_WIFI_WIDE_BANDWIDTH_40_MHZ)
    {
        return 0;
    }

    center_channel = _wifi_hostapd_get_80mhz_center_channel(config->ap.channel);
    if (center_channel == NULL)
    {
        WIFI_SERVICE_ERROR("unsupported 80MHz channel, channel=%u", (unsigned int)config->ap.channel);
        return -EINVAL;
    }

    return linkg_text_replace_key_value(content, length, "vht_oper_centr_freq_seg0_idx", center_channel);
}

/**
 * @brief 更新hostapd安全配置。
 */
static int _wifi_hostapd_update_security(char **content, size_t *length, const linkg_wifi_ap_config_t *config)
{
    int ret;

    if (content == NULL)
    {
        return -EINVAL;
    }

    if (*content == NULL)
    {
        return -EINVAL;
    }

    if (length == NULL)
    {
        return -EINVAL;
    }

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (!linkg_wifi_security_valid(config->security))
    {
        WIFI_SERVICE_ERROR("invalid AP security, security=%d", (int)config->security);
        return -EINVAL;
    }

    if (!linkg_wifi_password_valid(config->security, config->password))
    {
        WIFI_SERVICE_ERROR("invalid AP password, security=%d", (int)config->security);
        return -EINVAL;
    }

    switch (config->security)
    {
        case LINKG_WIFI_SECURITY_OPEN:
            return linkg_text_replace_key_value(content, length, "wpa", "0");

        case LINKG_WIFI_SECURITY_WPA2_PSK:
            ret = linkg_text_replace_key_value(content, length, "wpa", "2");
            if (ret != 0)
            {
                return ret;
            }

            return linkg_text_replace_key_value(content, length, "wpa_passphrase", config->password);

        default:
            return -EINVAL;
    }
}

/****************************** 控制接口 ******************************/

/**
 * @brief 判断hostapd状态响应是否表示AP已启用。
 */
static bool _wifi_hostapd_status_enabled(const char *status)
{
    const char *state;

    if (status == NULL)
    {
        return false;
    }

    state = strstr(status, "state=ENABLED");
    if (state == NULL)
    {
        return false;
    }

    if (state != status && state[-1] != '\n')
    {
        return false;
    }

    state += strlen("state=ENABLED");

    return state[0] == '\0' || state[0] == '\n' || state[0] == '\r';
}

/**
 * @brief 查询hostapd是否已经进入AP启用状态。
 */
static int _wifi_hostapd_get_enabled(bool *enabled)
{
    struct wpa_ctrl *control;
    char             reply[WIFI_HOSTAPD_STATUS_REPLY_MAX];
    size_t           reply_length;
    int              ret;

    if (enabled == NULL)
    {
        return -EINVAL;
    }

    *enabled = false;

    control = wpa_ctrl_open(WIFI_HOSTAPD_CTRL_PATH);
    if (control == NULL)
    {
        return -ENOTCONN;
    }

    memset(reply, 0, sizeof(reply));
    reply_length = sizeof(reply) - 1U;

    ret = wpa_ctrl_request(control, "STATUS", strlen("STATUS"), reply, &reply_length, NULL);

    wpa_ctrl_close(control);

    if (ret == -2)
    {
        return -ETIMEDOUT;
    }

    if (ret < 0)
    {
        return -EIO;
    }

    if (reply_length >= sizeof(reply))
    {
        return -EOVERFLOW;
    }

    reply[reply_length] = '\0';
    *enabled = _wifi_hostapd_status_enabled(reply);

    return 0;
}

/**
 * @brief 清理残留的hostapd控制套接字。
 */
static int _wifi_hostapd_remove_ctrl(void)
{
    int saved_errno;

    if (unlink(WIFI_HOSTAPD_CTRL_PATH) == 0)
    {
        return 0;
    }

    saved_errno = errno;
    if (saved_errno == ENOENT)
    {
        return 0;
    }

    if (saved_errno == 0)
    {
        return -EIO;
    }

    return -saved_errno;
}

/****************************** 服务辅助 ******************************/

/**
 * @brief 等待hostapd完成AP VAP初始化并进入ENABLED状态。
 */
static int _wifi_hostapd_wait_ready(void)
{
    bool     enabled;
    bool     running;
    uint64_t start_ms;
    uint64_t elapsed_ms;
    uint32_t sleep_ms;
    int      ret;

    start_ms = linkg_time_elapsed_ms();

    for (;;)
    {
        ret = linkg_os_process_running(&g_hostapd_pid, &running);
        if (ret != 0)
        {
            return ret;
        }

        if (!running)
        {
            return -ECHILD;
        }

        ret = _wifi_hostapd_get_enabled(&enabled);
        if (ret == 0)
        {
            if (enabled)
            {
                return 0;
            }
        }
        else if (ret != -ENOTCONN &&
                 ret != -ETIMEDOUT &&
                 ret != -EIO)
        {
            return ret;
        }

        elapsed_ms = linkg_time_elapsed_ms() - start_ms;
        if (elapsed_ms >= WIFI_HOSTAPD_READY_WAIT_MS)
        {
            return -ETIMEDOUT;
        }

        sleep_ms = (uint32_t)(WIFI_HOSTAPD_READY_WAIT_MS - elapsed_ms);
        if (sleep_ms > WIFI_SERVICE_WAIT_INTERVAL_MS)
        {
            sleep_ms = WIFI_SERVICE_WAIT_INTERVAL_MS;
        }

        ret = linkg_time_sleep_ms(sleep_ms);
        if (ret != 0)
        {
            return ret;
        }
    }
}

/****************************** 配置同步 ******************************/

/**
 * @brief 根据AP配置更新hostapd运行配置。
 */
int wifi_hostapd_update_config(const linkg_wifi_config_t *config)
{
    const char *template_path;
    const char *hardware_mode;
    char       *content = NULL;
    char        channel_value[16];
    size_t      length = 0U;
    int         formatted_length;
    int         ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    template_path = _wifi_hostapd_get_template(config);
    if (template_path == NULL)
    {
        WIFI_SERVICE_ERROR("unsupported hostapd template configuration");
        return -EINVAL;
    }

    ret = linkg_file_read_all(template_path, WIFI_SERVICE_TEMPLATE_MAX_SIZE, &content, &length);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("read hostapd template failed, path=%s, error=%d", template_path, ret);
        return ret;
    }

    if (length == 0U)
    {
        WIFI_SERVICE_ERROR("hostapd template is empty, path=%s", template_path);
        ret = -ENODATA;
        goto cleanup;
    }

    ret = linkg_text_replace_key_value(&content, &length, "interface", LINKG_RESOURCE_INTERFACE_WIFI);
    if (ret != 0)
    {
        goto cleanup;
    }

    if (!linkg_wifi_ssid_valid(config->ap.ssid))
    {
        WIFI_SERVICE_ERROR("invalid AP SSID");
        ret = -EINVAL;
        goto cleanup;
    }

    ret = linkg_text_replace_key_value(&content, &length, "ssid", config->ap.ssid);
    if (ret != 0)
    {
        goto cleanup;
    }

    if (!linkg_wifi_channel_valid(config->wideband.work_mode, config->ap.channel))
    {
        WIFI_SERVICE_ERROR("invalid AP channel, work_mode=%d, channel=%u",
                           config->wideband.work_mode,
                           (unsigned int)config->ap.channel);

        ret = -EINVAL;
        goto cleanup;
    }

    formatted_length = snprintf(channel_value, sizeof(channel_value), "%u", (unsigned int)config->ap.channel);
    if (formatted_length < 0)
    {
        ret = -EIO;
        goto cleanup;
    }

    if ((size_t)formatted_length >= sizeof(channel_value))
    {
        ret = -ENOSPC;
        goto cleanup;
    }

    ret = linkg_text_replace_key_value(&content, &length, "channel", channel_value);
    if (ret != 0)
    {
        goto cleanup;
    }

    hardware_mode = config->ap.channel <= 14U ? "g" : "a";

    ret = linkg_text_replace_key_value(&content, &length, "hw_mode", hardware_mode);
    if (ret != 0)
    {
        goto cleanup;
    }

    ret = _wifi_hostapd_update_bandwidth(&content, &length, config);
    if (ret != 0)
    {
        goto cleanup;
    }

    ret = _wifi_hostapd_update_security(&content, &length, &config->ap);
    if (ret != 0)
    {
        goto cleanup;
    }

    ret = linkg_file_mkdirs(WIFI_SERVICE_RUNTIME_DIR, WIFI_SERVICE_RUNTIME_DIR_MODE);
    if (ret != 0)
    {
        goto cleanup;
    }

    ret = linkg_file_write_all_atomic(WIFI_HOSTAPD_RUNTIME_CONFIG, content, length, WIFI_SERVICE_CONFIG_MODE);
    if (ret != 0)
    {
        goto cleanup;
    }

    WIFI_SERVICE_DEBUG("hostapd configuration updated, template=%s, runtime=%s",
                       template_path,
                       WIFI_HOSTAPD_RUNTIME_CONFIG);

cleanup:
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("update hostapd configuration failed, template=%s, error=%d", template_path, ret);
    }

    free(content);

    return ret;
}

/****************************** 无线配置 ******************************/

/**
 * @brief 应用AP最终无线工作模式配置。
 */
static int _wifi_hostapd_apply_radio_config(const linkg_wifi_config_t *config)
{
    const linkg_wifi_narrow_params_t *narrow;
    int                               ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (config->wideband.work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        return wifi_driver_set_narrow_bandwidth(false, LINKG_WIFI_NARROW_BANDWIDTH_MHZ);
    }

    if (config->wideband.work_mode != LINKG_WIFI_WORK_MODE_NARROW)
    {
        return -EINVAL;
    }

    narrow = &config->wideband.narrow_params;

    ret = wifi_driver_set_narrow_bandwidth(true, narrow->bandwidth);
    if (ret != 0)
    {
        return ret;
    }

    switch (narrow->mode)
    {
        case LINKG_WIFI_NARROW_MODE_ADAPTIVE:
            return wifi_driver_set_narrow_auto_rate(true);

        case LINKG_WIFI_NARROW_MODE_FIXED:
            ret = wifi_driver_set_narrow_auto_rate(false);
            if (ret != 0)
            {
                return ret;
            }

            return wifi_driver_set_narrow_rate_level((uint8_t)narrow->manual_rate);

        default:
            return -EINVAL;
    }
}

/****************************** 服务控制 ******************************/

/**
 * @brief 启动hostapd服务并应用最终AP VAP无线配置。
 */
int wifi_hostapd_start(const linkg_wifi_config_t *config)
{
    bool enabled;
    bool running;
    int  cleanup_ret;
    int  stop_ret;
    int  ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_os_process_running(&g_hostapd_pid, &running);
    if (ret != 0)
    {
        return ret;
    }

    if (running)
    {
        WIFI_SERVICE_DEBUG("hostapd already running, pid=%ld", (long)g_hostapd_pid);
        return 0;
    }

    ret = _wifi_hostapd_get_enabled(&enabled);
    if (ret == 0)
    {
        WIFI_SERVICE_ERROR("unmanaged hostapd is already using control socket, path=%s",
                           WIFI_HOSTAPD_CTRL_PATH);
        return -EBUSY;
    }

    if (ret != -ENOTCONN)
    {
        WIFI_SERVICE_ERROR("hostapd control socket is abnormal, path=%s, error=%d",
                           WIFI_HOSTAPD_CTRL_PATH,
                           ret);
        return ret;
    }

    ret = _wifi_hostapd_remove_ctrl();
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("remove stale hostapd control socket failed, path=%s, error=%d",
                           WIFI_HOSTAPD_CTRL_PATH,
                           ret);
        return ret;
    }

    ret = linkg_os_spawn(&g_hostapd_pid, WIFI_HOSTAPD_PROGRAM, WIFI_HOSTAPD_RUNTIME_CONFIG, NULL);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("start hostapd failed, config=%s, error=%d", WIFI_HOSTAPD_RUNTIME_CONFIG, ret);
        return ret;
    }

    ret = _wifi_hostapd_wait_ready();
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("hostapd AP VAP did not become ready, error=%d", ret);
        goto rollback;
    }

    ret = _wifi_hostapd_apply_radio_config(config);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("apply AP radio configuration failed, error=%d", ret);
        goto rollback;
    }

    WIFI_SERVICE_INFO("hostapd ready, pid=%ld, interface=%s, config=%s",
                      (long)g_hostapd_pid,
                      LINKG_RESOURCE_INTERFACE_WIFI,
                      WIFI_HOSTAPD_RUNTIME_CONFIG);

    return 0;

rollback:
    stop_ret = linkg_os_process_stop(&g_hostapd_pid, WIFI_SERVICE_STOP_WAIT_MS);
    if (stop_ret != 0)
    {
        WIFI_SERVICE_ERROR("rollback hostapd start failed, original_error=%d, rollback_error=%d",
                           ret,
                           stop_ret);
        return stop_ret;
    }

    cleanup_ret = _wifi_hostapd_remove_ctrl();
    if (cleanup_ret != 0)
    {
        WIFI_SERVICE_WARN("remove hostapd control socket after rollback failed, path=%s, error=%d",
                          WIFI_HOSTAPD_CTRL_PATH,
                          cleanup_ret);
    }

    return ret;
}

/**
 * @brief 停止hostapd服务。
 */
int wifi_hostapd_stop(void)
{
    bool managed;
    int  ret;

    managed = g_hostapd_pid > 1;

    ret = linkg_os_process_stop(&g_hostapd_pid, WIFI_SERVICE_STOP_WAIT_MS);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("stop hostapd failed, error=%d", ret);
        return ret;
    }

    if (managed)
    {
        ret = _wifi_hostapd_remove_ctrl();
        if (ret != 0)
        {
            WIFI_SERVICE_WARN("remove hostapd control socket failed, path=%s, error=%d",
                              WIFI_HOSTAPD_CTRL_PATH,
                              ret);
        }
    }

    WIFI_SERVICE_INFO("hostapd stopped");

    return 0;
}
