/**
 * @file wifi_wpa.c
 * @brief LinkG wpa_supplicant服务内部实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-26
 */

#define _POSIX_C_SOURCE           200809L                                                  // 启用POSIX.1-2008接口

#include "wifi_wpa.h"

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

#define WIFI_WPA_PROGRAM          "wpa_supplicant"                                         // wpa_supplicant程序名称
#define WIFI_WPA_TEMPLATE         "/app/current/wireless/wpa_supplicant.conf"              // wpa_supplicant配置模板
#define WIFI_WPA_RUNTIME_CONFIG   WIFI_SERVICE_RUNTIME_DIR "/wpa_supplicant.conf"          // wpa_supplicant运行配置
#define WIFI_WPA_CTRL_PATH        "/var/run/wpa_supplicant/" LINKG_RESOURCE_INTERFACE_WIFI // wpa_supplicant控制套接字
#define WIFI_WPA_QUOTED_VALUE_MAX (LINKG_WIFI_PASSWORD_MAX + 3U)                           // 带双引号配置值最大长度
#define WIFI_WPA_CTRL_REPLY_MAX   64U                                                      // 控制命令响应最大长度
#define WIFI_WPA_STATUS_REPLY_MAX 1024U                                                    // STATUS响应最大长度
#define WIFI_WPA_SCAN_COMMAND_MAX 48U                                                      // SCAN命令最大长度
#define WIFI_WPA_START_WAIT_MS    3000U                                                    // wpa_supplicant就绪等待时间

/****************************** 全局状态 ******************************/

static pid_t g_wpa_pid     = (pid_t)-1; // wpa_supplicant子进程PID
static bool  g_wpa_managed = false;     // 控制节点是否由本模块创建并持有

/****************************** 内部辅助 ******************************/

/**
 * @brief 去除控制命令响应末尾的换行符。
 */
static void _wifi_wpa_trim_reply(char *reply, size_t *reply_length)
{
    char tail;

    if (reply == NULL)
    {
        return;
    }

    if (reply_length == NULL)
    {
        return;
    }

    while (*reply_length > 0U)
    {
        tail = reply[*reply_length - 1U];
        if (tail != '\n' && tail != '\r')
        {
            break;
        }

        (*reply_length)--;
        reply[*reply_length] = '\0';
    }
}

/**
 * @brief 向wpa_supplicant发送控制命令并校验响应。
 */
static int _wifi_wpa_request_raw(const char *command, const char *expected_reply, bool log_failure)
{
    struct wpa_ctrl *control;
    char             reply[WIFI_WPA_CTRL_REPLY_MAX];
    size_t           reply_length;
    int              ret;

    if (command == NULL)
    {
        return -EINVAL;
    }

    if (command[0] == '\0')
    {
        return -EINVAL;
    }

    if (expected_reply == NULL)
    {
        return -EINVAL;
    }

    if (expected_reply[0] == '\0')
    {
        return -EINVAL;
    }

    control = wpa_ctrl_open(WIFI_WPA_CTRL_PATH);
    if (control == NULL)
    {
        if (log_failure)
        {
            WIFI_SERVICE_WARN("open wpa_supplicant control socket failed, path=%s", WIFI_WPA_CTRL_PATH);
        }

        return -ENOTCONN;
    }

    memset(reply, 0, sizeof(reply));
    reply_length = sizeof(reply) - 1U;

    ret = wpa_ctrl_request(control, command, strlen(command), reply, &reply_length, NULL);

    wpa_ctrl_close(control);

    // 等待响应超时
    if (ret == -2)
    {
        if (log_failure)
        {
            WIFI_SERVICE_WARN("wpa_supplicant command timed out, command=%s", command);
        }

        return -ETIMEDOUT;
    }

    // 发送或接收发生错误
    if (ret < 0)
    {
        if (log_failure)
        {
            WIFI_SERVICE_WARN("wpa_supplicant command failed, command=%s, error=%d", command, ret);
        }

        return -EIO;
    }

    // 响应长度异常
    if (reply_length >= sizeof(reply))
    {
        if (log_failure)
        {
            WIFI_SERVICE_WARN("wpa_supplicant reply is too long, command=%s", command);
        }

        return -EOVERFLOW;
    }

    // 响应末尾补字符串结束符，并去除末尾换行符。
    reply[reply_length] = '\0';
    _wifi_wpa_trim_reply(reply, &reply_length);

    /**
     * FAIL-BUSY表示wpa_supplicant已经有无线任务正在执行。
     * 它不是协议错误，调用方可等待现有任务结束后继续。
     */
    if (strcmp(reply, "FAIL-BUSY") == 0)
    {
        WIFI_SERVICE_DEBUG("wpa_supplicant command deferred because service is busy, command=%s",
                           command);
        return -EBUSY;
    }

    if (strcmp(reply, expected_reply) != 0)
    {
        if (log_failure)
        {
            WIFI_SERVICE_WARN("wpa_supplicant rejected command, command=%s, reply=%s",
                              command,
                              reply_length > 0U ? reply : "<empty>");
        }

        return -EPROTO;
    }

    return 0;
}

/**
 * @brief 等待wpa_supplicant控制接口就绪。
 */
static int _wifi_wpa_wait_ready(void)
{
    bool     running;
    uint64_t start_ms;
    uint64_t elapsed_ms;
    uint32_t sleep_ms;
    int      ret;

    start_ms = linkg_time_elapsed_ms();

    for (;;)
    {
        ret = linkg_os_process_running(&g_wpa_pid, &running);
        if (ret != 0)
        {
            return ret;
        }

        if (!running)
        {
            return -ECHILD;
        }

        ret = _wifi_wpa_request_raw("PING", "PONG", false);
        if (ret == 0)
        {
            return 0;
        }

        if (ret != -ENOTCONN &&
            ret != -ETIMEDOUT &&
            ret != -EIO)
        {
            return ret;
        }

        elapsed_ms = linkg_time_elapsed_ms() - start_ms;
        if (elapsed_ms >= WIFI_WPA_START_WAIT_MS)
        {
            return -ETIMEDOUT;
        }

        sleep_ms = (uint32_t)(WIFI_WPA_START_WAIT_MS - elapsed_ms);
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

/**
 * @brief 清理残留的wpa_supplicant控制套接字。
 */
static int _wifi_wpa_remove_ctrl(void)
{
    int saved_errno;

    if (unlink(WIFI_WPA_CTRL_PATH) == 0)
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

/**
 * @brief 向运行中的wpa_supplicant发送控制命令。
 */
static int _wifi_wpa_request(const char *command, bool log_failure)
{
    bool running;
    int  ret;

    if (command == NULL)
    {
        return -EINVAL;
    }

    if (command[0] == '\0')
    {
        return -EINVAL;
    }

    ret = linkg_os_process_running(&g_wpa_pid, &running);
    if (ret != 0)
    {
        return ret;
    }

    if (!running)
    {
        if (log_failure)
        {
            WIFI_SERVICE_WARN("wpa_supplicant is not running");
        }

        return -ENOTCONN;
    }

    ret = _wifi_wpa_request_raw(command, "OK", log_failure);
    if (ret == 0)
    {
        WIFI_SERVICE_DEBUG("wpa_supplicant command completed, command=%s", command);
    }

    return ret;
}

/****************************** 配置同步 ******************************/

/**
 * @brief 根据STA配置更新wpa_supplicant运行配置。
 */
int wifi_wpa_update_config(const linkg_wifi_config_t *config)
{
    char   *content = NULL;
    char    quoted_value[WIFI_WPA_QUOTED_VALUE_MAX];
    size_t  length = 0U;
    int     formatted_length;
    int     ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (!linkg_wifi_ssid_valid(config->sta.ssid))
    {
        WIFI_SERVICE_ERROR("invalid STA SSID");
        return -EINVAL;
    }

    if (!linkg_wifi_security_valid(config->sta.security))
    {
        WIFI_SERVICE_ERROR("invalid STA security, security=%d", (int)config->sta.security);
        return -EINVAL;
    }

    if (!linkg_wifi_password_valid(config->sta.security, config->sta.password))
    {
        WIFI_SERVICE_ERROR("invalid STA password, security=%d", (int)config->sta.security);
        return -EINVAL;
    }

    ret = linkg_file_read_all(WIFI_WPA_TEMPLATE, WIFI_SERVICE_TEMPLATE_MAX_SIZE, &content, &length);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("read wpa_supplicant template failed, path=%s, error=%d", WIFI_WPA_TEMPLATE, ret);
        return ret;
    }

    if (length == 0U)
    {
        WIFI_SERVICE_ERROR("wpa_supplicant template is empty, path=%s", WIFI_WPA_TEMPLATE);

        free(content);
        return -ENODATA;
    }

    formatted_length = snprintf(quoted_value, sizeof(quoted_value), "\"%s\"", config->sta.ssid);
    if (formatted_length < 0)
    {
        ret = -EIO;
        goto cleanup;
    }

    if ((size_t)formatted_length >= sizeof(quoted_value))
    {
        ret = -ENOSPC;
        goto cleanup;
    }

    ret = linkg_text_replace_key_value(&content, &length, "ssid", quoted_value);
    if (ret != 0)
    {
        goto cleanup;
    }

    switch (config->sta.security)
    {
        case LINKG_WIFI_SECURITY_OPEN:
            ret = linkg_text_replace_key_value(&content, &length, "key_mgmt", "NONE");
            break;

        case LINKG_WIFI_SECURITY_WPA2_PSK:
            ret = linkg_text_replace_key_value(&content, &length, "key_mgmt", "WPA-PSK");
            if (ret != 0)
            {
                goto cleanup;
            }

            formatted_length = snprintf(quoted_value, sizeof(quoted_value), "\"%s\"", config->sta.password);
            if (formatted_length < 0)
            {
                ret = -EIO;
                goto cleanup;
            }

            if ((size_t)formatted_length >= sizeof(quoted_value))
            {
                ret = -ENOSPC;
                goto cleanup;
            }

            ret = linkg_text_replace_key_value(&content, &length, "psk", quoted_value);
            break;

        default:
            ret = -EINVAL;
            break;
    }

    if (ret != 0)
    {
        goto cleanup;
    }

    ret = linkg_file_mkdirs(WIFI_SERVICE_RUNTIME_DIR, WIFI_SERVICE_RUNTIME_DIR_MODE);
    if (ret != 0)
    {
        goto cleanup;
    }

    ret = linkg_file_write_all_atomic(WIFI_WPA_RUNTIME_CONFIG, content, length, WIFI_SERVICE_CONFIG_MODE);
    if (ret != 0)
    {
        goto cleanup;
    }

    WIFI_SERVICE_DEBUG("wpa_supplicant configuration updated, runtime=%s", WIFI_WPA_RUNTIME_CONFIG);

cleanup:
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("update wpa_supplicant configuration failed, error=%d", ret);
    }

    free(content);

    return ret;
}

/****************************** 服务控制 ******************************/

/**
 * @brief 为STA关联准备无线参数。
 *
 * @note 固定窄带也先使用自动速率完成关联，WPA连接完成后再由速率维护模块锁档。
 */
static int _wifi_wpa_prepare_sta_radio(const linkg_wifi_config_t *config)
{
    const linkg_wifi_narrow_params_t *narrow;
    int                               ret;

    if (config->wideband.work_mode == LINKG_WIFI_WORK_MODE_WIDE)
    {
        return wifi_driver_set_narrow_bandwidth(false,
                                                LINKG_WIFI_NARROW_BANDWIDTH_MHZ);
    }

    if (config->wideband.work_mode != LINKG_WIFI_WORK_MODE_NARROW)
    {
        return -EINVAL;
    }

    narrow = &config->wideband.narrow_params;

    ret = wifi_driver_refresh_narrow_power_table();
    if (ret != 0)
    {
        return ret;
    }

    ret = wifi_driver_set_narrow_bandwidth(true, narrow->bandwidth);
    if (ret != 0)
    {
        return ret;
    }

    return wifi_driver_set_narrow_auto_rate(true);
}

/**
 * @brief 应用STA无线参数并启动wpa_supplicant服务。
 */
int wifi_wpa_start(const linkg_wifi_config_t *config)
{
    bool running;
    int  stop_ret;
    int  ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    ret = linkg_os_process_running(&g_wpa_pid, &running);
    if (ret != 0)
    {
        return ret;
    }

    if (running)
    {
        if (!g_wpa_managed)
        {
            WIFI_SERVICE_ERROR(
                "running wpa_supplicant is not owned by Wi-Fi module, pid=%ld",
                (long)g_wpa_pid);
            return -EBUSY;
        }

        WIFI_SERVICE_DEBUG("wpa_supplicant already running, pid=%ld", (long)g_wpa_pid);
        return 0;
    }

    ret = _wifi_wpa_request_raw("PING", "PONG", false);
    if (ret == 0)
    {
        g_wpa_managed = false;
        WIFI_SERVICE_ERROR("unmanaged wpa_supplicant is already using control socket, path=%s", WIFI_WPA_CTRL_PATH);
        return -EBUSY;
    }

    if (ret != -ENOTCONN && !g_wpa_managed)
    {
        WIFI_SERVICE_ERROR("wpa_supplicant control socket is abnormal, path=%s, error=%d",
                            WIFI_WPA_CTRL_PATH,
                            ret);
        return ret;
    }

    ret = _wifi_wpa_remove_ctrl();
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("remove stale wpa_supplicant control socket failed, path=%s, error=%d",
                           WIFI_WPA_CTRL_PATH,
                           ret);

        return ret;
    }

    g_wpa_managed = false;

    ret = _wifi_wpa_prepare_sta_radio(config);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("prepare STA radio configuration before wpa_supplicant failed, error=%d", ret);
        return ret;
    }

    ret = linkg_os_spawn(&g_wpa_pid,
                         WIFI_WPA_PROGRAM,
                         "-D",
                         "nl80211",
                         "-i",
                         LINKG_RESOURCE_INTERFACE_WIFI,
                         "-c",
                         WIFI_WPA_RUNTIME_CONFIG,
                         NULL);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("start wpa_supplicant failed, config=%s, error=%d", WIFI_WPA_RUNTIME_CONFIG, ret);
        return ret;
    }

    g_wpa_managed = true;

    ret = _wifi_wpa_wait_ready();
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("wpa_supplicant control interface did not become ready, error=%d", ret);

        stop_ret = linkg_os_process_stop(&g_wpa_pid, WIFI_SERVICE_STOP_WAIT_MS);
        if (stop_ret != 0)
        {
            WIFI_SERVICE_ERROR("rollback wpa_supplicant start failed, original_error=%d, rollback_error=%d",
                               ret,
                               stop_ret);
            return stop_ret;
        }

        stop_ret = _wifi_wpa_remove_ctrl();
        if (stop_ret != 0)
        {
            WIFI_SERVICE_WARN("remove wpa_supplicant control socket after rollback failed, path=%s, error=%d",
                              WIFI_WPA_CTRL_PATH,
                              stop_ret);
        }
        else
        {
            g_wpa_managed = false;
        }

        return ret;
    }

    WIFI_SERVICE_INFO("wpa_supplicant started, pid=%ld, interface=%s, config=%s",
                      (long)g_wpa_pid,
                      LINKG_RESOURCE_INTERFACE_WIFI,
                      WIFI_WPA_RUNTIME_CONFIG);

    return 0;
}

/**
 * @brief 停止wpa_supplicant服务。
 */
int wifi_wpa_stop(void)
{
    bool owned;
    int  ret;

    owned = g_wpa_managed;

    ret = linkg_os_process_stop(&g_wpa_pid, WIFI_SERVICE_STOP_WAIT_MS);
    if (ret != 0)
    {
        WIFI_SERVICE_ERROR("stop wpa_supplicant failed, error=%d", ret);
        return ret;
    }

    if (owned)
    {
        ret = _wifi_wpa_remove_ctrl();
        if (ret != 0)
        {
            WIFI_SERVICE_WARN("remove wpa_supplicant control socket failed, path=%s, error=%d",
                              WIFI_WPA_CTRL_PATH,
                              ret);
            return ret;
        }

        g_wpa_managed = false;
    }

    WIFI_SERVICE_INFO("wpa_supplicant stopped");

    return 0;
}

/**
 * @brief 检查受管wpa_supplicant进程及控制接口是否可响应。
 */
int wifi_wpa_ping(void)
{
    bool running;
    int  ret;

    ret = linkg_os_process_running(&g_wpa_pid, &running);
    if (ret != 0)
    {
        return ret;
    }

    if (!running || !g_wpa_managed)
    {
        return -ENOTCONN;
    }

    return _wifi_wpa_request_raw("PING", "PONG", false);
}

/****************************** 连接控制 ******************************/

/**
 * @brief 主动断开STA当前连接。
 */
int wifi_wpa_disconnect(void)
{
    int ret;

    ret = _wifi_wpa_request("DISCONNECT", true);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_SERVICE_DEBUG("STA disconnect request completed");

    return 0;
}

/**
 * @brief 请求STA重新连接目标AP。
 */
int wifi_wpa_reconnect(void)
{
    int ret;

    ret = _wifi_wpa_request("RECONNECT", true);
    if (ret != 0)
    {
        return ret;
    }

    WIFI_SERVICE_DEBUG("STA reconnect request completed");

    return 0;
}

/**
 * @brief 请求STA立即扫描。
 *
 * @note frequency_mhz为0时执行全频扫描，非0时仅扫描指定频率。
 */
int wifi_wpa_scan(uint32_t frequency_mhz)
{
    char command[WIFI_WPA_SCAN_COMMAND_MAX];
    int  formatted_length;
    int  ret;

    if (frequency_mhz == 0U)
    {
        ret = _wifi_wpa_request("SCAN", false);
    }
    else
    {
        formatted_length = snprintf(command,
                                    sizeof(command),
                                    "SCAN freq=%u",
                                    (unsigned int)frequency_mhz);
        if (formatted_length < 0)
        {
            return -EIO;
        }

        if ((size_t)formatted_length >= sizeof(command))
        {
            return -EOVERFLOW;
        }

        ret = _wifi_wpa_request(command, false);
    }

    if (ret == -EBUSY)
    {
        return ret;
    }

    if (ret != 0)
    {
        return ret;
    }

    WIFI_SERVICE_DEBUG("STA scan request completed, mode=%s, frequency_mhz=%u",
                       frequency_mhz == 0U ? "full" : "targeted",
                       (unsigned int)frequency_mhz);

    return 0;
}
