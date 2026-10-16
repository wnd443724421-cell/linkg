/**
 * @file web_handler_device.c
 * @brief LinkG设备信息查询、角色配置及全服务重启确认
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-16
 */

#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_device_info.h"
#include "linkg_json.h"
#include "linkg_lifecycle.h"
#include "linkg_log.h"

/****************************** 全局上下文 ******************************/

// Web请求处理与应答完成回调均在Web Server线程中执行，无并发访问。
static linkg_device_role_t g_web_device_running_role   = LINKG_DEVICE_ROLE_UNKNOWN; // 本轮服务实际运行的角色
static bool                g_web_device_initialized    = false;                     // 运行角色快照已初始化
static bool                g_web_device_restart_armed  = false;                     // 等待当前Web应答完成后重启
static bool                g_web_device_restart_failed = false;                     // 上次服务重启请求被拒绝
static bool                g_web_device_save_uncertain = false;                     // 保存失败且回滚状态无法确认

/****************************** 内部辅助 ******************************/

/**
 * @brief 将设备角色转换为Web协议字符串。
 */
static const char *_linkg_web_device_role_string(linkg_device_role_t role)
{
    switch (role)
    {
        case LINKG_DEVICE_ROLE_AP:
            return "ap";

        case LINKG_DEVICE_ROLE_STA:
            return "sta";

        default:
            return "unknown";
    }
}

/**
 * @brief 严格解析请求中的AP/STA角色。
 */
static int _linkg_web_device_role_parse(const cJSON *param, const char *key, linkg_device_role_t *role)
{
    char value[16];

    if (!cJSON_IsObject(param) || key == NULL || role == NULL)
    {
        return -EINVAL;
    }

    if (linkg_json_get_string(param, key, value, sizeof(value)) != 0)
    {
        return -EINVAL;
    }

    if (strcmp(value, "ap") == 0)
    {
        *role = LINKG_DEVICE_ROLE_AP;
        return 0;
    }

    if (strcmp(value, "sta") == 0)
    {
        *role = LINKG_DEVICE_ROLE_STA;
        return 0;
    }

    return -EINVAL;
}

/**
 * @brief 验证当前请求所携带的服务实例标识。
 */
static int _linkg_web_device_check_instance(const cJSON *param)
{
    const char *instance;
    char        received[80];

    if (!g_web_device_initialized || !cJSON_IsObject(param))
    {
        return -EINVAL;
    }

    instance = _linkg_web_network_basic_instance();
    if (instance == NULL || linkg_json_get_string(param, "instance", received, sizeof(received)) != 0)
    {
        return -EINVAL;
    }

    return strcmp(received, instance) == 0 ? 0 : -EINVAL;
}

/**
 * @brief 将出厂字段状态转换为Web协议字符串。
 */
static const char *_linkg_web_device_field_state_string(linkg_device_field_state_t state)
{
    switch (state)
    {
        case LINKG_DEVICE_FIELD_VALID:
            return "valid";

        case LINKG_DEVICE_FIELD_MISSING:
            return "missing";

        default:
            return "error";
    }
}

/**
 * @brief 序列化可用的出厂字段，缺失或读取错误时写入JSON null。
 */
static int _linkg_web_device_add_optional(cJSON *object, const char *key, const char *value, linkg_device_field_state_t state)
{
    if (state == LINKG_DEVICE_FIELD_VALID)
    {
        return linkg_json_add_string(object, key, value) == 0 ? 0 : -ENOMEM;
    }

    return cJSON_AddNullToObject(object, key) != NULL ? 0 : -ENOMEM;
}

/**
 * @brief 在保存或发起重启前完整构造成功响应。
 */
static int _linkg_web_device_apply_response(int cmd, const char *apply, char **response)
{
    cJSON *data;

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    if (linkg_json_add_string(data, "apply", apply) != 0)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    // 成功或失败均由响应构造函数接管并释放data。
    return _linkg_web_response_success(cmd, data, NULL, response);
}

/****************************** 服务接口 ******************************/

/**
 * @brief 每轮全服务初始化时记录实际运行角色并重置Web设备页面状态。
 */
int _linkg_web_device_init(void)
{
    linkg_device_config_t device;
    int                   ret;
    const char           *instance;

    g_web_device_initialized = false;

    instance = _linkg_web_network_basic_instance();
    ret = linkg_config_get_device(&device);
    if (ret != 0)
    {
        return ret;
    }

    if (linkg_device_config_validate(&device) != 0 || instance == NULL || instance[0] == '\0')
    {
        return -EINVAL;
    }

    g_web_device_running_role   = device.role;
    g_web_device_restart_armed  = false;
    g_web_device_restart_failed = false;
    g_web_device_save_uncertain = false;
    g_web_device_initialized    = true;

    return 0;
}

/**
 * @brief Web响应发送并关闭连接后，向主线程请求完整服务重启。
 */
void _linkg_web_device_response_complete(bool sent)
{
    int ret;

    if (!g_web_device_restart_armed)
    {
        return;
    }

    g_web_device_restart_armed = false;

    if (!sent)
    {
        g_web_device_restart_failed = true;
        return;
    }

    // 复用基础网络的300ms宽限，等待独立CGI进程写完HTTP响应。
    usleep(300000U);

    ret = linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_RESTART_SERVICES);
    g_web_device_restart_failed = ret != 0;

    if (ret != 0)
    {
        LINKG_LOG_ERROR("WEB: device restart request rejected, error=%d", ret);
    }
}

/****************************** 请求处理 ******************************/

/**
 * @brief 查询设备出厂信息、实际运行角色及已保存角色。
 */
int _linkg_web_handler_device_info_get(const cJSON *param, char **response)
{
    linkg_device_info_t info;
    linkg_config_t      saved;
    const char         *instance;
    cJSON              *data;
    cJSON              *device;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    if (!g_web_device_initialized || linkg_device_info_get(&info) != 0 || linkg_config_create_snapshot(&saved) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_INFO_GET, "设备信息尚未就绪", response);
    }

    instance = _linkg_web_network_basic_instance();
    if (instance == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_INFO_GET, "服务实例尚未就绪", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    device = cJSON_AddObjectToObject(data, "device");
    if (device == NULL ||
        linkg_json_add_string(device, "model", info.model) != 0 ||
        linkg_json_add_string(device, "software_version", info.software_version) != 0 ||
        linkg_json_add_string(device, "build_date", info.build_date) != 0 ||
        linkg_json_add_string(device, "wifi_hardware", linkg_wifi_hardware_type_string(info.wifi_type)) != 0 ||
        linkg_json_add_string(device, "role", _linkg_web_device_role_string(g_web_device_running_role)) != 0 ||
        linkg_json_add_string(device, "serial_state", _linkg_web_device_field_state_string(info.serial_state)) != 0 ||
        linkg_json_add_string(device, "hardware_state", _linkg_web_device_field_state_string(info.hardware_state)) != 0 ||
        _linkg_web_device_add_optional(device, "serial_number", info.serial_number, info.serial_state) != 0 ||
        _linkg_web_device_add_optional(device, "hardware_version", info.hardware_version, info.hardware_state) != 0)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    if (linkg_json_add_string(data, "running_role", _linkg_web_device_role_string(g_web_device_running_role)) != 0 ||
        linkg_json_add_string(data, "saved_role", _linkg_web_device_role_string(saved.device.role)) != 0 ||
        linkg_json_add_bool(data, "pending_restart", saved.device.role != g_web_device_running_role) != 0 ||
        linkg_json_add_bool(data, "persistence_confirmed", !g_web_device_save_uncertain) != 0 ||
        linkg_json_add_bool(data, "restart_failed", g_web_device_restart_failed) != 0 ||
        linkg_json_add_string(data, "instance", instance) != 0)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    // Web响应函数接管data的所有权。
    return _linkg_web_response_success(LINKG_WEB_CMD_DEVICE_INFO_GET, data, NULL, response);
}

/**
 * @brief 保存AP/STA角色，不立即重启当前服务。
 *
 * 配置写事务由Web分发器持有；保存失败时同时尝试恢复文件和内存配置。
 */
int _linkg_web_handler_device_role_set(const cJSON *param, char **response)
{
    linkg_config_t      old_config;
    linkg_config_t      new_config;
    linkg_device_role_t role;
    linkg_device_role_t expected_role;
    char               *success_response;
    int                 restore_file;
    int                 restore_memory;
    int                 ret;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    if (_linkg_web_device_check_instance(param) != 0 ||
        _linkg_web_device_role_parse(param, "role", &role) != 0 ||
        _linkg_web_device_role_parse(param, "expected_role", &expected_role) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_ROLE_SET, "角色参数或服务实例无效，请刷新页面", response);
    }

    ret = linkg_config_create_snapshot(&old_config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_ROLE_SET, "读取设备配置失败", response);
    }

    if (old_config.device.role != expected_role)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_ROLE_SET, "已保存角色已变化，请刷新页面", response);
    }

    if (old_config.device.role == role && !g_web_device_save_uncertain)
    {
        return _linkg_web_device_apply_response(LINKG_WEB_CMD_DEVICE_ROLE_SET, "none", response);
    }

    new_config             = old_config;
    new_config.device.role = role;

    if (linkg_device_config_validate(&new_config.device) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_ROLE_SET, "设备角色无效", response);
    }

    success_response = NULL;
    ret = _linkg_web_device_apply_response(LINKG_WEB_CMD_DEVICE_ROLE_SET, role == g_web_device_running_role ? "none" : "restart_required", &success_response);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_config_save(&new_config);
    if (ret == 0)
    {
        ret = linkg_config_replace(&new_config);
    }

    if (ret != 0)
    {
        linkg_json_string_free(success_response);

        restore_file   = linkg_config_save(&old_config);
        restore_memory = linkg_config_replace(&old_config);
        g_web_device_save_uncertain = restore_file != 0 || restore_memory != 0;

        LINKG_LOG_ERROR("WEB: device role save failed=%d, restore file=%d memory=%d", ret, restore_file, restore_memory);

        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_ROLE_SET, g_web_device_save_uncertain ?
                                         "保存失败且回滚状态无法确认，请检查日志" : "保存失败，已恢复原配置", response);
    }

    g_web_device_save_uncertain = false;
    *response = success_response;

    return 0;
}

/**
 * @brief 验证用户确认的目标角色，准备在Web应答完成后重启全部服务。
 */
int _linkg_web_handler_device_restart(const cJSON *param, char **response)
{
    linkg_config_t      saved;
    linkg_device_role_t confirmed_role;
    int                 ret;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    if (g_web_device_save_uncertain)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_RESTART, "持久配置状态无法确认，请重新保存", response);
    }

    if (_linkg_web_device_check_instance(param) != 0 || _linkg_web_device_role_parse(param, "role", &confirmed_role) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_RESTART, "确认角色或服务实例无效，请刷新页面", response);
    }

    ret = linkg_config_create_snapshot(&saved);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_RESTART, "读取已保存配置失败，未重启", response);
    }

    if (saved.device.role != confirmed_role)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_DEVICE_RESTART, "已保存角色发生变化，请重新确认", response);
    }

    if (saved.device.role == g_web_device_running_role)
    {
        return _linkg_web_device_apply_response(LINKG_WEB_CMD_DEVICE_RESTART, "none", response);
    }

    ret = _linkg_web_device_apply_response(LINKG_WEB_CMD_DEVICE_RESTART, "restarting", response);
    if (ret == 0)
    {
        g_web_device_restart_armed  = true;
        g_web_device_restart_failed = false;
    }

    return ret;
}
