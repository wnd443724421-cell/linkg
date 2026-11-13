/**
 * @file web_handler_wifi_scan.c
 * @brief LinkG信道干扰及附近AP扫描Web接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "web_internal.h"

#include <errno.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "linkg_network_scan.h"
#include "linkg_upgrade.h"
#include "linkg_uart_diagnostic.h"
#include "web_speedtest.h"

/****************************** 响应辅助 ******************************/

/**
 * @brief 将扫描阶段转换为协议文本。
 */
static const char *_linkg_web_scan_state(linkg_network_scan_state_t state)
{
    switch (state)
    {
        case LINKG_NETWORK_SCAN_IDLE:      return "idle";
        case LINKG_NETWORK_SCAN_STARTING:  return "starting";
        case LINKG_NETWORK_SCAN_RUNNING:   return "running";
        case LINKG_NETWORK_SCAN_RESTORING: return "restoring";
        case LINKG_NETWORK_SCAN_COMPLETED: return "completed";
        case LINKG_NETWORK_SCAN_CANCELLED: return "cancelled";
        default:                           return "failed";
    }
}

/**
 * @brief 排序噪声样本以计算中位数，不使用dBm算术平均作为功率平均。
 */
static int _linkg_web_scan_compare_noise(const void *left, const void *right)
{
    int32_t  a = *(const int32_t *)left;
    int32_t  b = *(const int32_t *)right;

    return (a > b) - (a < b);
}

/**
 * @brief 将有效与缺失的测量字段分别编码，缺失值明确返回null。
 */
static cJSON *_linkg_web_scan_channel(const linkg_wifi_scan_channel_t *channel)
{
    cJSON    *object;
    int32_t   noise[LINKG_WIFI_SCAN_ROUND_COUNT];
    uint32_t  count;
    double    median;

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return NULL;
    }
    count = channel->noise_samples;
    if (cJSON_AddNumberToObject(object, "channel", channel->channel) == NULL ||
        cJSON_AddNumberToObject(object, "frequency_mhz", channel->frequency_mhz) == NULL ||
        cJSON_AddNumberToObject(object, "samples", channel->samples) == NULL ||
        cJSON_AddNumberToObject(object, "active_ms", (double)channel->active_ms) == NULL ||
        cJSON_AddNumberToObject(object, "noise_samples", count) == NULL ||
        cJSON_AddBoolToObject(object, "valid", channel->samples > 0U) == NULL)
    {
        goto failed;
    }
    if (channel->samples > 0U && channel->active_ms > 0U)
    {
        if (cJSON_AddNumberToObject(object, "busy_percent", 100.0 * (double)channel->busy_ms / (double)channel->active_ms) == NULL ||
            cJSON_AddNumberToObject(object, "peak_busy_percent", channel->peak_busy_percent) == NULL)
        {
            goto failed;
        }
    }
    else if (cJSON_AddNullToObject(object, "busy_percent") == NULL ||
             cJSON_AddNullToObject(object, "peak_busy_percent") == NULL)
    {
        goto failed;
    }
    if (count > 0U && count <= LINKG_WIFI_SCAN_ROUND_COUNT)
    {
        memcpy(noise, channel->noise_dbm, count * sizeof(noise[0]));
        qsort(noise, count, sizeof(noise[0]), _linkg_web_scan_compare_noise);
        median = count % 2U != 0U ? (double)noise[count / 2U] :
                 ((double)noise[count / 2U - 1U] + (double)noise[count / 2U]) / 2.0;
        if (cJSON_AddNumberToObject(object, "noise_median_dbm", median) == NULL ||
            cJSON_AddNumberToObject(object, "noise_min_dbm", noise[0]) == NULL ||
            cJSON_AddNumberToObject(object, "noise_max_dbm", noise[count - 1U]) == NULL)
        {
            goto failed;
        }
    }
    else if (cJSON_AddNullToObject(object, "noise_median_dbm") == NULL ||
             cJSON_AddNullToObject(object, "noise_min_dbm") == NULL ||
             cJSON_AddNullToObject(object, "noise_max_dbm") == NULL)
    {
        goto failed;
    }
    return object;

failed:
    cJSON_Delete(object);
    return NULL;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 用户明确确认后启动异步扫描，旧页面必须刷新实例信息。
 */
int _linkg_web_handler_wifi_scan_start(const cJSON *param, char **response)
{
    const cJSON *instance;
    const char  *current;
    cJSON    *data;
    uint32_t  job_id;
    int       ret;

    linkg_upgrade_status_t          upgrade;
    linkg_web_speedtest_status_t    speedtest;
    linkg_uart_diagnostic_status_t  uart;

    memset(&upgrade, 0, sizeof(upgrade));
    memset(&speedtest, 0, sizeof(speedtest));
    memset(&uart, 0, sizeof(uart));
    if ((linkg_upgrade_get_status(&upgrade) == 0 && upgrade.state != LINKG_UPGRADE_STATE_IDLE && upgrade.state != LINKG_UPGRADE_STATE_FAILED) ||
        (_linkg_web_speedtest_get(0U, &speedtest) == 0 && speedtest.state >= LINKG_WEB_SPEEDTEST_STARTING && speedtest.state <= LINKG_WEB_SPEEDTEST_LISTENING) ||
        (linkg_uart_diagnostic_get_status(0U, &uart) == 0 && uart.state >= LINKG_UART_DIAGNOSTIC_STATE_STARTING && uart.state <= LINKG_UART_DIAGNOSTIC_STATE_RESTORING))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_SCAN_START, "其他诊断或升级正在执行，请等待完成后再扫描", response);
    }

    instance = cJSON_GetObjectItemCaseSensitive(param, "instance");
    current = _linkg_web_network_basic_instance();
    if (!cJSON_IsObject(param) || cJSON_GetArraySize(param) != 2 ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(param, "confirm")) ||
        !cJSON_IsString(instance) || current == NULL || strcmp(instance->valuestring, current) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_SCAN_START, "请刷新页面并确认扫描会暂时中断Wi-Fi通信", response);
    }
    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    ret = linkg_network_scan_start(&job_id);
    if (ret != 0)
    {
        cJSON_Delete(data);
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_SCAN_START, "Wi-Fi尚未就绪或正忙，无法启动扫描", response);
    }
    if (cJSON_AddNumberToObject(data, "job_id", job_id) == NULL)
    {
        cJSON_Delete(data);
        (void)linkg_network_scan_cancel(job_id);
        return -ENOMEM;
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_SCAN_START, data, "信道扫描已提交", response);
}

/**
 * @brief 返回扫描及恢复状态、信道统计和AP辅助列表。
 */
int _linkg_web_handler_wifi_scan_get(const cJSON *param, char **response)
{
    linkg_network_scan_status_t status;
    const linkg_wifi_scan_bss_t *entry;
    cJSON    *data;
    cJSON    *channels;
    cJSON    *bss;
    cJSON    *item;
    uint32_t  index;

    (void)param;
    (void)linkg_network_scan_get(&status);
    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    channels = cJSON_AddArrayToObject(data, "channels");
    bss = cJSON_AddArrayToObject(data, "bss");
    if (channels == NULL || bss == NULL ||
        cJSON_AddStringToObject(data, "state", _linkg_web_scan_state(status.state)) == NULL ||
        cJSON_AddStringToObject(data, "instance", _linkg_web_network_basic_instance()) == NULL ||
        cJSON_AddBoolToObject(data, "active", status.active) == NULL ||
        cJSON_AddNumberToObject(data, "job_id", status.job_id) == NULL ||
        cJSON_AddNumberToObject(data, "started_at", (double)status.started_at) == NULL ||
        cJSON_AddNumberToObject(data, "rounds", status.result.rounds) == NULL ||
        cJSON_AddNumberToObject(data, "total_rounds", LINKG_WIFI_SCAN_ROUND_COUNT) == NULL ||
        cJSON_AddNumberToObject(data, "bss_rounds", status.result.bss_rounds) == NULL ||
        cJSON_AddNumberToObject(data, "survey_failures", status.result.survey_failures) == NULL ||
        cJSON_AddNumberToObject(data, "stats_bandwidth_mhz", 20U) == NULL ||
        cJSON_AddNumberToObject(data, "scan_error", status.scan_error) == NULL ||
        cJSON_AddNumberToObject(data, "restore_error", status.restore_error) == NULL ||
        cJSON_AddBoolToObject(data, "bss_truncated", status.result.bss_truncated) == NULL)
    {
        goto failed;
    }
    for (index = 0U; index < LINKG_WIFI_SCAN_CHANNEL_COUNT; index++)
    {
        item = _linkg_web_scan_channel(&status.result.channels[index]);
        if (item == NULL)
        {
            goto failed;
        }
        if (!cJSON_AddItemToArray(channels, item))
        {
            cJSON_Delete(item);
            goto failed;
        }
    }
    for (index = 0U; index < status.result.bss_count; index++)
    {
        entry = &status.result.bss[index];
        item = cJSON_CreateObject();
        if (item == NULL)
        {
            goto failed;
        }
        if (!cJSON_AddItemToArray(bss, item))
        {
            cJSON_Delete(item);
            goto failed;
        }
        if (cJSON_AddStringToObject(item, "ssid", entry->ssid) == NULL ||
            cJSON_AddStringToObject(item, "bssid", entry->bssid) == NULL ||
            cJSON_AddNumberToObject(item, "frequency_mhz", entry->frequency_mhz) == NULL ||
            cJSON_AddNumberToObject(item, "signal_dbm", entry->signal_dbm) == NULL)
        {
            goto failed;
        }
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_SCAN_GET, data, "获取信道扫描状态成功", response);

failed:
    cJSON_Delete(data);
    return -ENOMEM;
}

/**
 * @brief 取消指定扫描；取消不会跳过原Wi-Fi服务恢复。
 */
int _linkg_web_handler_wifi_scan_stop(const cJSON *param, char **response)
{
    const cJSON *item;
    const cJSON *instance;
    const char *current;
    double  number;
    int     ret;

    instance = cJSON_GetObjectItemCaseSensitive(param, "instance");
    current = _linkg_web_network_basic_instance();
    item = cJSON_GetObjectItemCaseSensitive(param, "job_id");
    number = cJSON_IsNumber(item) ? item->valuedouble : 0.0;
    if (!cJSON_IsObject(param) || cJSON_GetArraySize(param) != 2 || !cJSON_IsString(instance) || current == NULL ||
        strcmp(instance->valuestring, current) != 0 || !isfinite(number) ||
        number < 1.0 || number > UINT32_MAX || number != (double)(uint32_t)number)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_SCAN_STOP, "无效的扫描任务编号", response);
    }
    ret = linkg_network_scan_cancel((uint32_t)number);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_WIFI_SCAN_STOP, "任务已结束或编号已变化，请刷新状态", response);
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_WIFI_SCAN_STOP, NULL, "已请求取消，仍需等待Wi-Fi服务恢复", response);
}
