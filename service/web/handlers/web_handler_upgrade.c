/**
 * @file web_handler_upgrade.c
 * @brief 异步OTA请求和持久化升级状态查询
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-20
 */

#include "web_internal.h"

#include <errno.h>
#include <stdio.h>

#include "linkg_release_state.h"
#include "linkg_upgrade.h"

/****************************** Web接口 ******************************/

/**
 * @brief 只接受已确认的升级请求，安装、ENV写入和重启由工作线程处理。
 */
int _linkg_web_handler_upgrade_start(const cJSON *param, char **response)
{
    const char *message;
    cJSON      *data;
    int         ret;

    if (!cJSON_IsObject(param) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(param, "confirm")))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UPGRADE_START, "请先确认开始升级", response);
    }
    ret = linkg_upgrade_request_install();
    if (ret != 0)
    {
        message = ret == -EBUSY ? "升级、试运行或回滚进行中，请等待完成" :
                  ret == -EPROTONOSUPPORT ? "运行中的Supervisor不支持升级状态格式，请先部署兼容版并重启" :
                  ret == -ENOENT ? "上传包或版本状态不存在，请检查设备后重新上传" : "升级请求未被接受，请检查版本状态、ENV和系统日志";
        return _linkg_web_response_error(LINKG_WEB_CMD_UPGRADE_START, message, response);
    }
    data = cJSON_CreateObject();
    if (data == NULL || cJSON_AddBoolToObject(data, "accepted", true) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_UPGRADE_START, data, "升级任务已接受，尚未完成安装或确认", response);
}

/**
 * @brief 每次读取同一份JSON，文件损坏或不可读时明确报错，不回退到内存idle。
 */
int _linkg_web_handler_upgrade_status_get(const cJSON *param, char **response)
{
    linkg_release_state_t state;
    linkg_upgrade_status_t worker;
    const char           *path;
    char                  message[128];
    cJSON                *data;
    int                   ret;

    (void)param;
    path = linkg_release_path("LINKG_OTA_TEST_STATE", "/app/version_state.json");
    ret = linkg_release_load(path, &state);
    if (ret != 0)
    {
        snprintf(message, sizeof(message), "版本状态文件读取或校验失败，error=%d", ret);
        return _linkg_web_response_error(LINKG_WEB_CMD_UPGRADE_STATUS_GET, message, response);
    }
    data = cJSON_CreateObject();
    if (data == NULL ||
        cJSON_AddStringToObject(data, "upgrade_state", linkg_release_phase_name(state.upgrade)) == NULL ||
        cJSON_AddStringToObject(data, "release_partition", linkg_release_name(state.target)) == NULL ||
        cJSON_AddStringToObject(data, "stable_release_partition", linkg_release_name(state.stable)) == NULL ||
        cJSON_AddBoolToObject(data, "trial", state.target != state.stable) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    if (linkg_upgrade_get_status(&worker) == 0 && worker.task_id != 0U)
    {
        if (cJSON_AddNumberToObject(data, "task_id", worker.task_id) == NULL ||
            cJSON_AddNumberToObject(data, "progress", worker.progress) == NULL ||
            cJSON_AddNumberToObject(data, "error_code", worker.error_code) == NULL ||
            cJSON_AddStringToObject(data, "message", worker.message) == NULL)
        {
            cJSON_Delete(data);
            return -ENOMEM;
        }
    }
    return _linkg_web_response_success(LINKG_WEB_CMD_UPGRADE_STATUS_GET, data, NULL, response);
}
