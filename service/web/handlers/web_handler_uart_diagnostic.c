/**
 * @file web_handler_uart_diagnostic.c
 * @brief UART硬件诊断任务Web接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-01
 */

#include "web_internal.h"

#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "linkg_uart_diagnostic.h"
#include "linkg_json.h"

/****************************** JSON辅助 ******************************/

/**
 * @brief 校验JSON对象只包含指定字段且没有重复字段。
 */
static bool _uart_diag_web_object_has_keys(const cJSON *object, const char *const *keys, size_t key_count)
{
    const cJSON *item;
    const cJSON *previous;
    size_t       index;
    bool         found;

    if (!cJSON_IsObject(object))
    {
        return false;
    }

    for (item = object->child; item != NULL; item = item->next)
    {
        if (item->string == NULL)
        {
            return false;
        }

        found = false;
        for (index = 0U; index < key_count; index++)
        {
            if (strcmp(item->string, keys[index]) == 0)
            {
                found = true;
                break;
            }
        }
        if (!found)
        {
            return false;
        }

        for (previous = object->child; previous != item; previous = previous->next)
        {
            if (previous->string != NULL && strcmp(previous->string, item->string) == 0)
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief 将JSON整数安全转换为uint32_t。
 */
static int _uart_diag_web_get_uint32(const cJSON *item, uint32_t *value)
{
    double number;

    if (!cJSON_IsNumber(item) || value == NULL)
    {
        return -EINVAL;
    }

    number = item->valuedouble;
    if (!isfinite(number) || number < 0.0 || number > (double)UINT32_MAX ||
        number != (double)(uint32_t)number)
    {
        return -EINVAL;
    }

    *value = (uint32_t)number;
    return 0;
}

/**
 * @brief 返回UART诊断任务状态名称。
 */
static const char *_uart_diag_web_state_name(linkg_uart_diagnostic_state_t state)
{
    switch (state)
    {
        case LINKG_UART_DIAGNOSTIC_STATE_IDLE:
            return "idle";
        case LINKG_UART_DIAGNOSTIC_STATE_STARTING:
            return "starting";
        case LINKG_UART_DIAGNOSTIC_STATE_RUNNING:
            return "running";
        case LINKG_UART_DIAGNOSTIC_STATE_RESTORING:
            return "restoring";
        case LINKG_UART_DIAGNOSTIC_STATE_COMPLETED:
            return "completed";
        case LINKG_UART_DIAGNOSTIC_STATE_CANCELLED:
            return "cancelled";
        case LINKG_UART_DIAGNOSTIC_STATE_FAILED:
            return "failed";
        default:
            return "unknown";
    }
}

/**
 * @brief 返回UART单项测试状态名称。
 */
static const char *_uart_diag_web_test_state_name(linkg_uart_diagnostic_test_state_t state)
{
    switch (state)
    {
        case LINKG_UART_DIAGNOSTIC_TEST_NOT_RUN:
            return "not_run";
        case LINKG_UART_DIAGNOSTIC_TEST_RUNNING:
            return "running";
        case LINKG_UART_DIAGNOSTIC_TEST_PASSED:
            return "passed";
        case LINKG_UART_DIAGNOSTIC_TEST_FAILED:
            return "failed";
        case LINKG_UART_DIAGNOSTIC_TEST_CANCELLED:
            return "cancelled";
        case LINKG_UART_DIAGNOSTIC_TEST_SKIPPED:
            return "skipped";
        default:
            return "unknown";
    }
}

/**
 * @brief 返回UART诊断模式名称。
 */
static const char *_uart_diag_web_mode_name(linkg_uart_diagnostic_mode_t mode)
{
    if (mode == LINKG_UART_DIAGNOSTIC_MODE_SINGLE)
    {
        return "single";
    }
    if (mode == LINKG_UART_DIAGNOSTIC_MODE_ALL)
    {
        return "all";
    }

    return (int)mode == 0 ? "none" : "unknown";
}

/**
 * @brief 判断诊断任务是否仍处于可取消状态。
 */
static bool _uart_diag_web_state_active(linkg_uart_diagnostic_state_t state)
{
    return state == LINKG_UART_DIAGNOSTIC_STATE_STARTING ||
           state == LINKG_UART_DIAGNOSTIC_STATE_RUNNING ||
           state == LINKG_UART_DIAGNOSTIC_STATE_RESTORING;
}

/**
 * @brief 将单项UART诊断结果追加到JSON数组。
 */
static int _uart_diag_web_add_test(cJSON *array, const linkg_uart_diagnostic_test_result_t *result)
{
    cJSON      *item;
    const char *path;

    item = cJSON_CreateObject();
    if (item == NULL)
    {
        return -ENOMEM;
    }
    if (!cJSON_AddItemToArray(array, item))
    {
        cJSON_Delete(item);
        return -ENOMEM;
    }

    path = result->path == LINKG_UART_DIAGNOSTIC_PATH_NORMAL ? "normal" :
           result->path == LINKG_UART_DIAGNOSTIC_PATH_SBUS ? "sbus" : "unknown";

    if (cJSON_AddNumberToObject(item, "uart_num", result->uart_num) == NULL ||
        cJSON_AddStringToObject(item, "path", path) == NULL ||
        cJSON_AddStringToObject(item, "state", _uart_diag_web_test_state_name(result->state)) == NULL ||
        cJSON_AddNumberToObject(item, "tx_bytes", result->tx_bytes) == NULL ||
        cJSON_AddNumberToObject(item, "rx_bytes", result->rx_bytes) == NULL ||
        cJSON_AddNumberToObject(item, "error_count", result->error_count) == NULL ||
        cJSON_AddNumberToObject(item, "first_error_offset", result->first_error_offset) == NULL ||
        cJSON_AddNumberToObject(item, "frames_total", result->frames_total) == NULL ||
        cJSON_AddNumberToObject(item, "frames_ok", result->frames_ok) == NULL ||
        cJSON_AddNumberToObject(item, "error", result->error) == NULL ||
        cJSON_AddNumberToObject(item, "elapsed_ms", (double)result->elapsed_ms) == NULL)
    {
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief 构造启动响应并预留任务编号位置，避免任务启动后响应分配失败。
 */
static int _uart_diag_web_prepare_start_response(const linkg_uart_diagnostic_params_t *params, char **response, char **job_position)
{
    cJSON      *data;
    const char *mode;
    int         ret;

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    mode = _uart_diag_web_mode_name(params->mode);
    if (cJSON_AddNumberToObject(data, "job_id", (double)UINT32_MAX) == NULL ||
        cJSON_AddStringToObject(data, "mode", mode) == NULL ||
        cJSON_AddStringToObject(data, "state", "starting") == NULL ||
        (params->mode == LINKG_UART_DIAGNOSTIC_MODE_SINGLE &&
         cJSON_AddNumberToObject(data, "selected_uart", params->uart_num) == NULL))
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    ret = _linkg_web_response_success(LINKG_WEB_CMD_UART_DIAG_START, data,
                                      "已接受串口硬件诊断任务", response);
    if (ret != 0)
    {
        return ret;
    }

    *job_position = strstr(*response, "\"job_id\":4294967295");
    if (*job_position == NULL)
    {
        linkg_json_string_free(*response);
        *response = NULL;
        return -EPROTO;
    }

    *job_position += strlen("\"job_id\":");
    return 0;
}

/****************************** 任务请求 ******************************/

/**
 * @brief 构造诊断取消响应。
 */
static int _uart_diag_web_stop_response(uint32_t job_id, bool cancel_requested, const char *state, char **response)
{
    cJSON *data;

    data = cJSON_CreateObject();
    if (data == NULL ||
        cJSON_AddNumberToObject(data, "job_id", job_id) == NULL ||
        cJSON_AddBoolToObject(data, "cancel_requested", cancel_requested) == NULL ||
        cJSON_AddStringToObject(data, "state", state) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_UART_DIAG_STOP, data, NULL, response);
}

/**
 * @brief 接受单串口或全部串口硬件诊断任务。
 */
int _linkg_web_handler_uart_diagnostic_start(const cJSON *param, char **response)
{
    static const char *const keys[] = {"mode", "uart_num"};
    const cJSON                    *mode;
    const cJSON                    *uart_num;
    linkg_uart_diagnostic_params_t  params;
    char                           *job_position;
    char                            job_text[11];
    size_t                          job_length;
    uint32_t                        uart_index;
    uint32_t                        job_id;
    int                             ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;

    if (!_uart_diag_web_object_has_keys(param, keys, sizeof(keys) / sizeof(keys[0])))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_START, "诊断参数无效：仅允许mode和uart_num字段", response);
    }

    mode = cJSON_GetObjectItemCaseSensitive(param, "mode");
    if (!cJSON_IsString(mode) || mode->valuestring == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_START, "缺少有效的诊断模式", response);
    }

    memset(&params, 0, sizeof(params));
    if (strcmp(mode->valuestring, "single") == 0)
    {
        uart_num = cJSON_GetObjectItemCaseSensitive(param, "uart_num");
        ret = _uart_diag_web_get_uint32(uart_num, &uart_index);
        if (ret != 0 || uart_index >= LINKG_UART_DIAGNOSTIC_UART_COUNT)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_START, "单串口模式需要uart_num=0、1或2", response);
        }

        params.mode     = LINKG_UART_DIAGNOSTIC_MODE_SINGLE;
        params.uart_num = (uint8_t)uart_index;
    }
    else if (strcmp(mode->valuestring, "all") == 0)
    {
        if (cJSON_GetObjectItemCaseSensitive(param, "uart_num") != NULL)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_START, "全部测试模式不允许设置uart_num", response);
        }

        params.mode = LINKG_UART_DIAGNOSTIC_MODE_ALL;
    }
    else
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_START, "不支持的诊断模式", response);
    }

    ret = _uart_diag_web_prepare_start_response(&params, response, &job_position);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_uart_diagnostic_submit(&params, &job_id);
    if (ret != 0)
    {
        linkg_json_string_free(*response);
        *response = NULL;
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_START,
                                        ret == -EBUSY ? "串口正在应用配置或执行其他维护任务" :
                                        ret == -ENODEV ? "UART诊断模块尚未初始化" :
                                        ret == -ESHUTDOWN ? "UART诊断线程尚未启动或正在停止" :
                                        ret == -EINVAL ? "诊断参数无效" : "启动串口硬件诊断失败", response);
    }

    (void)snprintf(job_text, sizeof(job_text), "%" PRIu32, job_id);
    job_length = strlen(job_text);
    memmove(job_position + job_length, job_position + 10U, strlen(job_position + 10U) + 1U);
    memcpy(job_position, job_text, job_length);

    return 0;
}

/**
 * @brief 查询当前或指定UART硬件诊断任务及各项测试结果。
 */
int _linkg_web_handler_uart_diagnostic_get(const cJSON *param, char **response)
{
    static const char *const keys[] = {"job_id"};
    linkg_uart_diagnostic_status_t status;
    const cJSON                   *job;
    cJSON                         *data;
    cJSON                         *tests;
    uint32_t                       job_id;
    uint32_t                       index;
    int                            ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;

    job_id = 0U;
    if (param != NULL)
    {
        if (!_uart_diag_web_object_has_keys(param, keys, sizeof(keys) / sizeof(keys[0])))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_GET, "查询参数无效：仅允许job_id字段", response);
        }

        job = cJSON_GetObjectItemCaseSensitive(param, "job_id");
        if (job != NULL && _uart_diag_web_get_uint32(job, &job_id) != 0)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_GET, "job_id无效", response);
        }
    }

    ret = linkg_uart_diagnostic_get_status(job_id, &status);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_GET, ret == -ENOENT ? "诊断任务不存在或已被新任务覆盖" : "读取诊断状态失败", response);
    }
    if (status.test_count > LINKG_UART_DIAGNOSTIC_MAX_TESTS)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_GET, "诊断状态数据无效", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    tests = cJSON_AddArrayToObject(data, "tests");
    if (tests == NULL ||
        cJSON_AddNumberToObject(data, "job_id", status.job_id) == NULL ||
        cJSON_AddStringToObject(data, "mode", _uart_diag_web_mode_name(status.mode)) == NULL ||
        cJSON_AddStringToObject(data, "state", _uart_diag_web_state_name(status.state)) == NULL ||
        cJSON_AddNumberToObject(data, "selected_uart", status.selected_uart) == NULL ||
        cJSON_AddNumberToObject(data, "current_test", status.current_test) == NULL ||
        cJSON_AddNumberToObject(data, "test_count", status.test_count) == NULL ||
        cJSON_AddBoolToObject(data, "cancel_requested", status.cancel_requested) == NULL ||
        cJSON_AddBoolToObject(data, "uart_was_initialized", status.uart_was_initialized) == NULL ||
        cJSON_AddBoolToObject(data, "uart_was_running", status.uart_was_running) == NULL ||
        cJSON_AddBoolToObject(data, "uart_restored", status.uart_restored) == NULL ||
        cJSON_AddBoolToObject(data, "tests_passed", status.tests_passed) == NULL ||
        cJSON_AddNumberToObject(data, "error", status.error) == NULL ||
        cJSON_AddNumberToObject(data, "restore_error", status.restore_error) == NULL ||
        cJSON_AddNumberToObject(data, "elapsed_ms", (double)status.elapsed_ms) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    for (index = 0U; index < status.test_count; index++)
    {
        ret = _uart_diag_web_add_test(tests, &status.tests[index]);
        if (ret != 0)
        {
            cJSON_Delete(data);
            return ret;
        }
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_UART_DIAG_GET, data, NULL, response);
}

/**
 * @brief 请求取消当前或指定UART硬件诊断任务。
 */
int _linkg_web_handler_uart_diagnostic_stop(const cJSON *param, char **response)
{
    static const char *const keys[] = {"job_id"};
    linkg_uart_diagnostic_status_t status;
    const cJSON                  *job;
    char                         *accepted_response;
    char                         *finished_response;
    uint32_t                      job_id;
    uint32_t                      target_job_id;
    int                           ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;

    if (!_uart_diag_web_object_has_keys(param, keys, sizeof(keys) / sizeof(keys[0])))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_STOP, "取消参数无效：必须提供job_id，0表示当前任务", response);
    }

    job = cJSON_GetObjectItemCaseSensitive(param, "job_id");
    if (_uart_diag_web_get_uint32(job, &job_id) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_STOP, "job_id无效", response);
    }

    ret = linkg_uart_diagnostic_get_status(job_id, &status);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_STOP, "诊断任务不存在或已被新任务覆盖", response);
    }

    target_job_id = job_id == 0U ? status.job_id : job_id;
    if (!_uart_diag_web_state_active(status.state))
    {
        return _uart_diag_web_stop_response(target_job_id, false, _uart_diag_web_state_name(status.state), response);
    }

    // 取消会改变后台任务状态，先准备两种响应，避免取消成功后因内存分配失败丢失确认。
    accepted_response = NULL;
    ret = _uart_diag_web_stop_response(target_job_id, true, "cancel_requested", &accepted_response);
    if (ret != 0)
    {
        return ret;
    }

    finished_response = NULL;
    ret = _uart_diag_web_stop_response(target_job_id, false, "already_finished", &finished_response);
    if (ret != 0)
    {
        linkg_json_string_free(accepted_response);
        return ret;
    }

    ret = linkg_uart_diagnostic_cancel(target_job_id);
    if (ret == 0)
    {
        linkg_json_string_free(finished_response);
        *response = accepted_response;
        return 0;
    }

    linkg_json_string_free(accepted_response);
    if (ret == -EALREADY)
    {
        *response = finished_response;
        return 0;
    }

    linkg_json_string_free(finished_response);
    return _linkg_web_response_error(LINKG_WEB_CMD_UART_DIAG_STOP, ret == -ENOENT ? "诊断任务已被新任务替代" : "请求取消诊断任务失败", response);
}
