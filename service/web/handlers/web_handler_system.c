/**
 * @file web_handler_system.c
 * @brief LinkG服务重启、整机重启与恢复出厂配置
 *
 * 确认命令由Web线程接受；在应答发送并关闭连接后才请求Lifecycle退出。
 */

#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_lifecycle.h"
#include "linkg_log.h"

/****************************** 操作状态 ******************************/

// Web Handler与响应完成回调均在同一Web Server线程调用。
static linkg_exit_action_t g_web_system_action = LINKG_EXIT_ACTION_NONE;
static bool                g_web_system_factory_restored = false;

/****************************** 内部辅助 ******************************/

/**
 * @brief 确认字段必须是严格的JSON true。
 */
static bool _linkg_web_system_confirmed(const cJSON *param)
{
    return cJSON_IsObject(param) &&
           cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(param, "confirm"));
}

/**
 * @brief 构造已接收操作响应；构造失败时不改变系统状态。
 */
static int _linkg_web_system_accept(int cmd, const char *action, char **response)
{
    cJSON *data;

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    if (cJSON_AddBoolToObject(data, "accepted", true) == NULL ||
        cJSON_AddStringToObject(data, "action", action) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    return _linkg_web_response_success(cmd, data, "已接受操作请求", response);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 每轮Web重新初始化时清除旧的应答后操作。
 */
void _linkg_web_system_init(void)
{
    g_web_system_action           = LINKG_EXIT_ACTION_NONE;
    g_web_system_factory_restored = false;
}

/**
 * @brief 连接关闭后执行所接受的操作，避免重启中断HTTP应答。
 *
 * 恢复出厂配置已经持久化时，即使HTTP应答发送失败也必须重启，
 * 防止当前内存配置与磁盘配置长期不一致。
 */
void _linkg_web_system_response_complete(bool sent)
{
    linkg_exit_action_t action;
    bool                restored;
    int                 ret;

    action   = g_web_system_action;
    restored = g_web_system_factory_restored;

    g_web_system_action           = LINKG_EXIT_ACTION_NONE;
    g_web_system_factory_restored = false;

    if (action == LINKG_EXIT_ACTION_NONE || (!sent && !restored))
    {
        return;
    }

    if (sent)
    {
        // 允许独立CGI进程将已发送的后端响应完整写回浏览器。
        usleep(300000U);
    }

    ret = linkg_lifecycle_request_exit(action);
    if (ret != 0)
    {
        LINKG_LOG_ERROR("WEB: system action request failed, action=%d, error=%d",
                        (int)action, ret);
    }
}

/****************************** 请求处理 ******************************/

/**
 * @brief 1023：重启全部LinkG服务，或重启Linux整机。
 */
int _linkg_web_handler_system_reboot(const cJSON *param, char **response)
{
    const cJSON       *scope;
    linkg_exit_action_t action;
    const char        *label;
    int                ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;

    if (!_linkg_web_system_confirmed(param))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_REBOOT, "请确认重启操作", response);
    }

    scope = cJSON_GetObjectItemCaseSensitive(param, "scope");
    if (!cJSON_IsString(scope) || scope->valuestring == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_REBOOT, "无效的重启类型", response);
    }

    if (strcmp(scope->valuestring, "services") == 0)
    {
        action = LINKG_EXIT_ACTION_RESTART_SERVICES;
        label  = "restart_services";
    }
    else if (strcmp(scope->valuestring, "system") == 0)
    {
        action = LINKG_EXIT_ACTION_REBOOT_SYSTEM;
        label  = "reboot_system";
    }
    else
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_REBOOT, "无效的重启类型", response);
    }

    if (linkg_lifecycle_is_exit_requested() || g_web_system_action != LINKG_EXIT_ACTION_NONE)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_REBOOT, "设备正在停止或重启", response);
    }

    ret = _linkg_web_system_accept(LINKG_WEB_CMD_SYSTEM_REBOOT, label, response);
    if (ret == 0)
    {
        g_web_system_action = action;
    }
    return ret;
}

/**
 * @brief 1024：原子恢复出厂配置，成功后请求整机重启。
 *
 * 由Dispatcher持有配置写事务，避免与Web配置保存交叉执行。
 * GPIO长按恢复使用相同的Config与Lifecycle接口。
 */
int _linkg_web_handler_system_factory_reset(const cJSON *param, char **response)
{
    char *accepted;
    int   ret;

    if (response == NULL)
    {
        return -EINVAL;
    }
    *response = NULL;

    if (!_linkg_web_system_confirmed(param))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_FACTORY_RESET, "请确认恢复出厂配置", response);
    }

    if (linkg_lifecycle_is_exit_requested() || g_web_system_action != LINKG_EXIT_ACTION_NONE)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_FACTORY_RESET, "设备正在停止或重启", response);
    }

    accepted = NULL;
    ret = _linkg_web_system_accept(LINKG_WEB_CMD_SYSTEM_FACTORY_RESET, "factory_reset", &accepted);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_config_restore_factory();
    if (ret != 0)
    {
        linkg_json_string_free(accepted);
        LINKG_LOG_ERROR("WEB: factory configuration restore failed, error=%d", ret);
        return _linkg_web_response_error(LINKG_WEB_CMD_SYSTEM_FACTORY_RESET,
                                         "恢复出厂配置失败，设备未重启", response);
    }

    g_web_system_factory_restored = true;
    g_web_system_action           = LINKG_EXIT_ACTION_REBOOT_SYSTEM;
    *response                     = accepted;
    return 0;
}
