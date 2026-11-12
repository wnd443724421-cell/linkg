/**
 * @file web_handler_speedtest.c
 * @brief Web网络吞吐量客户端与服务端诊断接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-09
 */

#include "web_internal.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "web_speedtest.h"

/****************************** 参数检查 ******************************/

/**
 * @brief 校验请求字段白名单和重复字段。
 */
static bool _linkg_web_speedtest_has_keys(const cJSON *object, const char *const *keys, size_t key_count)
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
 * @brief 安全提取JSON无符号整数。
 */
static bool _linkg_web_speedtest_get_uint32(const cJSON *item, uint32_t *value)
{
    double number;

    if (value == NULL || !cJSON_IsNumber(item))
    {
        return false;
    }
    number = item->valuedouble;
    if (!isfinite(number) || number < 0.0 || number > (double)UINT32_MAX ||
        number != (double)(uint32_t)number)
    {
        return false;
    }
    *value = (uint32_t)number;
    return true;
}

/**
 * @brief 返回当前测速任务状态文本。
 */
static const char *_linkg_web_speedtest_state_name(linkg_web_speedtest_state_t state)
{
    switch (state)
    {
        case LINKG_WEB_SPEEDTEST_IDLE:      return "idle";
        case LINKG_WEB_SPEEDTEST_STARTING:  return "starting";
        case LINKG_WEB_SPEEDTEST_RUNNING:   return "running";
        case LINKG_WEB_SPEEDTEST_LISTENING: return "listening";
        case LINKG_WEB_SPEEDTEST_COMPLETED: return "completed";
        case LINKG_WEB_SPEEDTEST_FAILED:    return "failed";
        case LINKG_WEB_SPEEDTEST_CANCELLED: return "cancelled";
        default:                            return "failed";
    }
}

/****************************** 请求处理 ******************************/

/**
 * @brief 启动客户端测速或在本机172.28地址上启动iperf3服务端。
 */
int _linkg_web_handler_speedtest_start(const cJSON *param, char **response)
{
    static const char *const client_keys[] =
    {
        "mode", "server", "port", "auto_port", "protocol", "direction", "duration_sec", "bandwidth_mbps", "packet_length"
    };
    static const char *const server_keys[] = {"mode", "port"};
    linkg_web_speedtest_params_t params;
    const cJSON                 *mode;
    const cJSON                 *server;
    const cJSON                 *protocol;
    const cJSON                 *direction;
    const cJSON                 *item;
    cJSON                       *data;
    uint32_t                     number;
    uint32_t                     job_id;
    int                          ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;
    if (!cJSON_IsObject(param))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "测速参数格式无效", response);
    }

    memset(&params, 0, sizeof(params));
    mode = cJSON_GetObjectItemCaseSensitive(param, "mode");
    if (mode == NULL || (cJSON_IsString(mode) && mode->valuestring != NULL &&
                         strcmp(mode->valuestring, "client") == 0))
    {
        params.mode = LINKG_WEB_SPEEDTEST_MODE_CLIENT;
    }
    else if (cJSON_IsString(mode) && mode->valuestring != NULL &&
             strcmp(mode->valuestring, "server") == 0)
    {
        params.mode = LINKG_WEB_SPEEDTEST_MODE_SERVER;
    }
    else
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "不支持的测速模式", response);
    }

    if (params.mode == LINKG_WEB_SPEEDTEST_MODE_SERVER)
    {
        if (!_linkg_web_speedtest_has_keys(param, server_keys, sizeof(server_keys) / sizeof(server_keys[0])))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START,
                                             "服务器模式只接受mode和port", response);
        }
        item = cJSON_GetObjectItemCaseSensitive(param, "port");
        if (!_linkg_web_speedtest_get_uint32(item, &number) || number == 0U || number > 65535U)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "监听端口必须为1至65535", response);
        }
        params.port = (uint16_t)number;
    }
    else
    {
        if (!_linkg_web_speedtest_has_keys(param, client_keys, sizeof(client_keys) / sizeof(client_keys[0])))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "客户端测速参数格式无效", response);
        }
        server    = cJSON_GetObjectItemCaseSensitive(param, "server");
        protocol  = cJSON_GetObjectItemCaseSensitive(param, "protocol");
        direction = cJSON_GetObjectItemCaseSensitive(param, "direction");
        if (!cJSON_IsString(server) || server->valuestring == NULL ||
            strlen(server->valuestring) >= sizeof(params.server))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "服务器地址无效", response);
        }
        snprintf(params.server, sizeof(params.server), "%s", server->valuestring);

        item = cJSON_GetObjectItemCaseSensitive(param, "port");
        if (!_linkg_web_speedtest_get_uint32(item, &number) || number == 0U || number > 65535U)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "连接端口必须为1至65535", response);
        }
        params.port = (uint16_t)number;

        item = cJSON_GetObjectItemCaseSensitive(param, "auto_port");
        if (item != NULL && !cJSON_IsBool(item))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "自动端口选项无效", response);
        }
        params.auto_port = cJSON_IsTrue(item);

        item = cJSON_GetObjectItemCaseSensitive(param, "duration_sec");
        if (!_linkg_web_speedtest_get_uint32(item, &number) || number < 5U || number > 60U)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "测速时长必须为5至60秒", response);
        }
        params.duration_sec = number;

        if (!cJSON_IsString(protocol) || protocol->valuestring == NULL)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "缺少测速协议", response);
        }
        if (strcmp(protocol->valuestring, "tcp") == 0)
        {
            params.protocol = LINKG_WEB_SPEEDTEST_TCP;
        }
        else if (strcmp(protocol->valuestring, "udp") == 0)
        {
            params.protocol = LINKG_WEB_SPEEDTEST_UDP;
        }
        else
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "不支持的测试协议", response);
        }

        if (!cJSON_IsString(direction) || direction->valuestring == NULL)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "缺少测速方向", response);
        }
        if (strcmp(direction->valuestring, "upload") == 0)
        {
            params.direction = LINKG_WEB_SPEEDTEST_UPLOAD;
        }
        else if (strcmp(direction->valuestring, "download") == 0)
        {
            params.direction = LINKG_WEB_SPEEDTEST_DOWNLOAD;
        }
        else
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "不支持的测试方向", response);
        }

        if (params.protocol == LINKG_WEB_SPEEDTEST_UDP)
        {
            item = cJSON_GetObjectItemCaseSensitive(param, "bandwidth_mbps");
            if (!cJSON_IsNumber(item) || !isfinite(item->valuedouble) ||
                item->valuedouble < 0.1 || item->valuedouble > 200.0)
            {
                return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START,
                                                 "UDP目标带宽必须为0.1至200Mbps", response);
            }
            params.bandwidth_mbps = item->valuedouble;
            item                  = cJSON_GetObjectItemCaseSensitive(param, "packet_length");
            if (!_linkg_web_speedtest_get_uint32(item, &params.packet_length) ||
                params.packet_length < 200U || params.packet_length > 1472U)
            {
                return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START,
                                                 "UDP数据长度必须为200至1472字节", response);
            }
        }
        else if (cJSON_GetObjectItemCaseSensitive(param, "bandwidth_mbps") != NULL ||
                 cJSON_GetObjectItemCaseSensitive(param, "packet_length") != NULL)
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START, "TCP测速不接受UDP参数", response);
        }
    }

    ret = _linkg_web_speedtest_submit(&params, &job_id);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_START,
                                         ret == -EBUSY ? "已有测速任务或服务器正在运行" :
                                         ret == -EINVAL ? "测速参数不合法" : "启动网络测速任务失败", response);
    }
    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    if (cJSON_AddNumberToObject(data, "job_id", job_id) == NULL ||
        cJSON_AddStringToObject(data, "state", "starting") == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_SPEEDTEST_START, data,
                                       params.mode == LINKG_WEB_SPEEDTEST_MODE_SERVER ?
                                       "正在启动组网测速服务器" : "测速任务已启动", response);
}

/**
 * @brief 读取当前或指定测速任务的状态。
 */
int _linkg_web_handler_speedtest_get(const cJSON *param, char **response)
{
    static const char *const keys[] = {"job_id"};
    linkg_web_speedtest_status_t status;
    const cJSON                 *item;
    cJSON                       *data;
    cJSON                       *samples;
    cJSON                       *sample;
    uint32_t                     job_id;
    uint32_t                     index;
    int                          ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;
    job_id    = 0U;

    if (param != NULL)
    {
        if (!_linkg_web_speedtest_has_keys(param, keys, sizeof(keys) / sizeof(keys[0])))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_GET, "查询参数无效", response);
        }
        item = cJSON_GetObjectItemCaseSensitive(param, "job_id");
        if (item != NULL && !_linkg_web_speedtest_get_uint32(item, &job_id))
        {
            return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_GET, "job_id无效", response);
        }
    }

    ret = _linkg_web_speedtest_get(job_id, &status);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_GET, "测速任务不存在", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    if (cJSON_AddNumberToObject(data, "job_id", status.job_id) == NULL ||
        cJSON_AddStringToObject(data, "state", _linkg_web_speedtest_state_name(status.state)) == NULL ||
        cJSON_AddStringToObject(data, "mode", status.params.mode == LINKG_WEB_SPEEDTEST_MODE_SERVER ? "server" : "client") == NULL ||
        cJSON_AddStringToObject(data, "server", status.params.server) == NULL ||
        cJSON_AddStringToObject(data, "bind_address", status.bind_address) == NULL ||
        cJSON_AddNumberToObject(data, "port", status.params.port) == NULL ||
        cJSON_AddBoolToObject(data, "auto_port", status.params.auto_port) == NULL ||
        cJSON_AddStringToObject(data, "protocol", status.params.protocol == LINKG_WEB_SPEEDTEST_UDP ? "udp" : "tcp") == NULL ||
        cJSON_AddStringToObject(data, "direction", status.params.direction == LINKG_WEB_SPEEDTEST_DOWNLOAD ? "download" : "upload") == NULL ||
        cJSON_AddBoolToObject(data, "rate_is_sender", status.rate_is_sender) == NULL ||
        cJSON_AddBoolToObject(data, "average_is_sender", status.average_is_sender) == NULL ||
        cJSON_AddNumberToObject(data, "duration_sec", status.params.duration_sec) == NULL ||
        cJSON_AddNumberToObject(data, "bandwidth_mbps", status.params.bandwidth_mbps) == NULL ||
        cJSON_AddNumberToObject(data, "packet_length", status.params.packet_length) == NULL ||
        cJSON_AddNumberToObject(data, "current_mbps", status.current_mbps) == NULL ||
        cJSON_AddNumberToObject(data, "average_mbps", status.average_mbps) == NULL ||
        cJSON_AddNumberToObject(data, "sender_mbps", status.sender_mbps) == NULL ||
        cJSON_AddBoolToObject(data, "sender_valid", status.sender_valid) == NULL ||
        cJSON_AddNumberToObject(data, "interval_count", status.interval_count) == NULL ||
        cJSON_AddNumberToObject(data, "session_id", status.session_id) == NULL ||
        cJSON_AddBoolToObject(data, "server_connected", status.server_connected) == NULL ||
        cJSON_AddNumberToObject(data, "elapsed_ms", (double)status.elapsed_ms) == NULL ||
        cJSON_AddBoolToObject(data, "average_valid", status.average_valid) == NULL ||
        cJSON_AddBoolToObject(data, "packet_loss_valid", status.packet_loss_valid) == NULL ||
        cJSON_AddNumberToObject(data, "packet_loss_percent", status.packet_loss_percent) == NULL ||
        cJSON_AddBoolToObject(data, "cancel_requested", status.cancel_requested) == NULL ||
        cJSON_AddStringToObject(data, "message", status.message) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    samples = cJSON_AddArrayToObject(data, "samples");
    if (samples == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    for (index = 0U; index < status.sample_count && index < LINKG_WEB_SPEEDTEST_SAMPLE_MAX; index++)
    {
        sample = cJSON_CreateObject();
        if (sample == NULL)
        {
            cJSON_Delete(data);
            return -ENOMEM;
        }
        if (cJSON_AddNumberToObject(sample, "sec", status.samples[index].second) == NULL ||
            cJSON_AddNumberToObject(sample, "mbps", status.samples[index].mbps) == NULL ||
            !cJSON_AddItemToArray(samples, sample))
        {
            cJSON_Delete(sample);
            cJSON_Delete(data);
            return -ENOMEM;
        }
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_SPEEDTEST_GET, data, NULL, response);
}

/**
 * @brief 请求取消正在运行的测速任务。
 */
int _linkg_web_handler_speedtest_stop(const cJSON *param, char **response)
{
    static const char *const keys[] = {"job_id"};
    const cJSON *item;
    cJSON       *data;
    uint32_t     job_id;
    int          ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;

    if (!_linkg_web_speedtest_has_keys(param, keys, sizeof(keys) / sizeof(keys[0])))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_STOP, "取消参数无效", response);
    }
    item = cJSON_GetObjectItemCaseSensitive(param, "job_id");
    if (!_linkg_web_speedtest_get_uint32(item, &job_id) || job_id == 0U)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_STOP, "job_id无效", response);
    }

    ret = _linkg_web_speedtest_cancel(job_id);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SPEEDTEST_STOP,
                                         ret == -EALREADY ? "测速任务已经结束" : "测速任务不存在", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    if (cJSON_AddNumberToObject(data, "job_id", job_id) == NULL ||
        cJSON_AddBoolToObject(data, "cancel_requested", true) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_SPEEDTEST_STOP, data, "已接收取消请求", response);
}
