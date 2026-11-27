/**
 * @file web_handler_rf_test.c
 * @brief 蜂窝射频强发诊断Web接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-27
 */

#include "web_internal.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include "linkg_rf_test.h"
#include "linkg_upgrade.h"
#include "linkg_uart_diagnostic.h"
#include "web_speedtest.h"

/****************************** 参数校验 ******************************/

/**
 * @brief 安全转换整数参数。
 */
static bool _rf_web_uint(const cJSON *param, const char *name, uint32_t *out)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(param, name);
    double       value;

    if (!cJSON_IsNumber(item))
    {
        return false;
    }
    value = item->valuedouble;
    if (!isfinite(value) || value < 0.0 || value > (double)UINT32_MAX || value != (double)(uint32_t)value)
    {
        return false;
    }
    *out = (uint32_t)value;
    return true;
}

/**
 * @brief 校验应用实例，拒绝过期页面的控制请求。
 */
static bool _rf_web_instance(const cJSON *param)
{
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(param, "instance");
    const char  *instance = _linkg_web_network_basic_instance();

    return cJSON_IsString(item) && item->valuestring != NULL && instance != NULL &&
           strcmp(item->valuestring, instance) == 0;
}

/**
 * @brief 校验其他诊断及升级任务是否占用设备。
 */
static bool _rf_web_busy(void)
{
    linkg_upgrade_status_t         upgrade;
    linkg_web_speedtest_status_t   speedtest;
    linkg_uart_diagnostic_status_t uart;

    memset(&upgrade, 0, sizeof(upgrade));
    memset(&speedtest, 0, sizeof(speedtest));
    memset(&uart, 0, sizeof(uart));
    return (linkg_upgrade_get_status(&upgrade) == 0 && upgrade.state != LINKG_UPGRADE_STATE_IDLE && upgrade.state != LINKG_UPGRADE_STATE_FAILED) ||
           (_linkg_web_speedtest_get(0U, &speedtest) == 0 && speedtest.state >= LINKG_WEB_SPEEDTEST_STARTING && speedtest.state <= LINKG_WEB_SPEEDTEST_LISTENING) ||
           (linkg_uart_diagnostic_get_status(0U, &uart) == 0 && uart.state >= LINKG_UART_DIAGNOSTIC_STATE_STARTING && uart.state <= LINKG_UART_DIAGNOSTIC_STATE_RESTORING);
}

/****************************** 响应辅助 ******************************/

/**
 * @brief 返回任务状态文本。
 */
static const char *_rf_web_state(linkg_rf_state_t state)
{
    switch (state)
    {
        case LINKG_RF_IDLE:             return "idle";
        case LINKG_RF_STARTING:         return "starting";
        case LINKG_RF_OPENING:          return "opening";
        case LINKG_RF_CHECKING:         return "checking";
        case LINKG_RF_ENTERING:         return "entering";
        case LINKG_RF_STARTING_TX:      return "starting_tx";
        case LINKG_RF_RUNNING:          return "running";
        case LINKG_RF_STOPPING_TX:      return "stopping_tx";
        case LINKG_RF_EXITING:          return "exiting";
        case LINKG_RF_RELEASING:        return "releasing";
        case LINKG_RF_RESTORING:        return "restoring";
        case LINKG_RF_COMPLETED:        return "completed";
        case LINKG_RF_CANCELLED:        return "cancelled";
        case LINKG_RF_RECOVERY_FAILED:  return "recovery_failed";
        default:                       return "failed";
    }
}

/**
 * @brief 将服务端候选频段表转换为Web参数，不在前端重复维护频段。
 */
static int _rf_web_bands(cJSON *data)
{
    const linkg_rf_band_t *bands;
    cJSON                *array;
    cJSON                *item;
    cJSON                *widths;
    cJSON                *width;
    size_t                count;
    size_t                i;
    uint32_t              code;

    array = cJSON_AddArrayToObject(data, "bands");
    if (array == NULL)
    {
        return -ENOMEM;
    }
    bands = linkg_rf_test_bands(&count);
    for (i = 0U; i < count; i++)
    {
        item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(array, item))
        {
            cJSON_Delete(item);
            return -ENOMEM;
        }
        widths = cJSON_AddArrayToObject(item, "bandwidths");
        if (widths == NULL ||
            cJSON_AddNumberToObject(item, "rat", bands[i].rat) == NULL ||
            cJSON_AddNumberToObject(item, "band", bands[i].band) == NULL ||
            cJSON_AddNumberToObject(item, "low_khz", bands[i].low_khz) == NULL ||
            cJSON_AddNumberToObject(item, "high_khz", bands[i].high_khz) == NULL ||
            cJSON_AddNumberToObject(item, "first_channel", bands[i].first_channel) == NULL ||
            cJSON_AddNumberToObject(item, "last_channel", bands[i].last_channel) == NULL ||
            cJSON_AddNumberToObject(item, "channel_step", bands[i].channel_step) == NULL ||
            cJSON_AddNumberToObject(item, "step_khz", bands[i].step_khz) == NULL ||
            cJSON_AddNumberToObject(item, "default_khz", bands[i].default_khz) == NULL)
        {
            return -ENOMEM;
        }
        for (code = 0U; code <= 10U; code++)
        {
            if ((bands[i].bandwidth_mask & (1U << code)) == 0U)
            {
                continue;
            }
            width = cJSON_CreateObject();
            if (width == NULL || !cJSON_AddItemToArray(widths, width))
            {
                cJSON_Delete(width);
                return -ENOMEM;
            }
            if (cJSON_AddNumberToObject(width, "code", code) == NULL ||
                cJSON_AddNumberToObject(width, "khz", linkg_rf_test_bandwidth_khz(code)) == NULL)
            {
                return -ENOMEM;
            }
        }
    }
    return 0;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 提交强发任务，仅接受白名单整数参数及操作确认。
 */
int _linkg_web_handler_rf_test_start(const cJSON *param, char **response)
{
    linkg_rf_params_t params;
    uint32_t          job_id;
    uint32_t          channel;
    uint32_t          actual;
    cJSON            *data;
    int               ret;

    if (!cJSON_IsObject(param) || cJSON_GetArraySize(param) != 7 ||
        !_rf_web_instance(param) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(param, "confirm")) ||
        !_rf_web_uint(param, "rat", &params.rat) ||
        !_rf_web_uint(param, "band", &params.band) ||
        !_rf_web_uint(param, "bandwidth_code", &params.bandwidth_code) ||
        !_rf_web_uint(param, "frequency_khz", &params.frequency_khz) ||
        !_rf_web_uint(param, "duration_sec", &params.duration_sec) ||
        linkg_rf_test_resolve(&params, &channel, &actual) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_RF_TEST_START, "射频参数无效或未确认测试条件，请刷新后重试", response);
    }
    if (_rf_web_busy())
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_RF_TEST_START, "其他诊断或升级正在执行", response);
    }
    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    ret = linkg_rf_test_start(&params, &job_id);
    if (ret != 0)
    {
        cJSON_Delete(data);
        if (ret == -EINPROGRESS)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_RF_TEST_START, "正在恢复蜂窝资源，本次不会启动强发，请稍后查看状态", response);
        }
        return _linkg_web_response_error(LINKG_WEB_CMD_RF_TEST_START, "蜂窝资源正忙或未就绪，无法开始射频测试", response);
    }
    if (cJSON_AddNumberToObject(data, "job_id", job_id) == NULL)
    {
        cJSON_Delete(data);
        (void)linkg_rf_test_cancel(job_id);
        return -ENOMEM;
    }
    ret = _linkg_web_response_success(LINKG_WEB_CMD_RF_TEST_START, data, "射频测试已提交", response);
    if (ret != 0)
    {
        (void)linkg_rf_test_cancel(job_id);
    }
    return ret;
}

/**
 * @brief 返回状态、实际配置参数及候选频段表。
 */
int _linkg_web_handler_rf_test_get(const cJSON *param, char **response)
{
    linkg_rf_status_t status;
    cJSON            *data;

    (void)param;
    linkg_rf_test_get(&status);
    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    if (cJSON_AddStringToObject(data, "instance", _linkg_web_network_basic_instance()) == NULL ||
        cJSON_AddStringToObject(data, "state", _rf_web_state(status.state)) == NULL ||
        cJSON_AddBoolToObject(data, "active", status.active) == NULL ||
        cJSON_AddNumberToObject(data, "job_id", status.job_id) == NULL ||
        cJSON_AddNumberToObject(data, "rat", status.params.rat) == NULL ||
        cJSON_AddNumberToObject(data, "band", status.params.band) == NULL ||
        cJSON_AddNumberToObject(data, "bandwidth_khz", status.job_id ? linkg_rf_test_bandwidth_khz(status.params.bandwidth_code) : 0U) == NULL ||
        cJSON_AddNumberToObject(data, "bandwidth_code", status.params.bandwidth_code) == NULL ||
        cJSON_AddNumberToObject(data, "frequency_khz", status.params.frequency_khz) == NULL ||
        cJSON_AddNumberToObject(data, "actual_khz", status.actual_khz) == NULL ||
        cJSON_AddNumberToObject(data, "channel", status.channel) == NULL ||
        cJSON_AddNumberToObject(data, "duration_sec", status.params.duration_sec) == NULL ||
        cJSON_AddNumberToObject(data, "elapsed_ms", (double)status.elapsed_ms) == NULL ||
        cJSON_AddNumberToObject(data, "gain_dbm", 23) == NULL ||
        cJSON_AddNumberToObject(data, "test_error", status.test_error) == NULL ||
        cJSON_AddStringToObject(data, "test_failed_at", _rf_web_state(status.test_failed_at)) == NULL ||
        cJSON_AddStringToObject(data, "cleanup_failed_at", _rf_web_state(status.cleanup_failed_at)) == NULL ||
        cJSON_AddBoolToObject(data, "recovery_blocked", status.recovery_blocked) == NULL ||
        cJSON_AddBoolToObject(data, "recovery_active", status.recovery_active) == NULL ||
        cJSON_AddNumberToObject(data, "recovery_error", status.recovery_error) == NULL ||
        cJSON_AddNumberToObject(data, "cleanup_error", status.cleanup_error) == NULL ||
        cJSON_AddNumberToObject(data, "restore_error", status.restore_error) == NULL ||
        cJSON_AddBoolToObject(data, "tx_confirmed", status.tx_confirmed) == NULL ||
        cJSON_AddBoolToObject(data, "mode_verified", status.mode_verified) == NULL ||
        _rf_web_bands(data) != 0)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_RF_TEST_GET, data, NULL, response);
}

/**
 * @brief 取消指定任务，后端仍负责完整清理。
 */
int _linkg_web_handler_rf_test_stop(const cJSON *param, char **response)
{
    uint32_t job_id;

    if (!cJSON_IsObject(param) || cJSON_GetArraySize(param) != 2 ||
        !_rf_web_instance(param) || !_rf_web_uint(param, "job_id", &job_id) ||
        linkg_rf_test_cancel(job_id) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_RF_TEST_STOP, "任务不存在或已经结束，请刷新状态", response);
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_RF_TEST_STOP, NULL, "已请求停止射频测试", response);
}
