/**
 * @file web_handler_config.c
 * @brief LinkG完整用户配置导入、导出及应答后服务重载
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-13
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_lifecycle.h"
#include "linkg_log.h"
#include "linkg_time.h"

/****************************** 模块常量 ******************************/

#define LINKG_WEB_CONFIG_FILE_MAX (64U * 1024U) // 导入文件最大字节数

/****************************** 全局上下文 ******************************/

// Handler与完成回调均由同一个Web线程调用。
static uint64_t g_web_config_retry_at; // 下次重载请求重试的单调时间
static bool g_web_config_restart_armed = false; // 配置落盘后等待应答完成

/****************************** 服务接口 ******************************/

/**
 * @brief 每轮服务初始化时清除导入操作状态。
 */
void _linkg_web_config_init(void)
{
    g_web_config_restart_armed = false;
    g_web_config_retry_at = 0U;
}

/**
 * @brief 已提交的配置必须重载，HTTP应答失败时同样执行。
 */
void _linkg_web_config_response_complete(bool sent)
{
    if (!g_web_config_restart_armed)
    {
        return;
    }
    if (sent)
    {
        usleep(300000U);
    }
    _linkg_web_config_retry_reload();
}

/**
 * @brief 导入已经落盘时保留重载义务，失败后由Web周期轮询再次提交。
 */
void _linkg_web_config_retry_reload(void)
{
    uint64_t now;
    int      ret;

    if (!g_web_config_restart_armed)
    {
        return;
    }
    now = linkg_time_elapsed_ms();
    if (now < g_web_config_retry_at)
    {
        return;
    }
    ret = linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_RESTART_SERVICES);
    if (ret == 0)
    {
        g_web_config_restart_armed = false;
        g_web_config_retry_at = 0U;
        return;
    }
    g_web_config_retry_at = now + 1000U;
    LINKG_LOG_ERROR("WEB: imported configuration restart failed, retry pending, error=%d", ret);
}

/**
 * @brief 仅Web线程读取，待重载时启用有界轮询，不依赖新的客户端请求。
 */
bool _linkg_web_config_reload_pending(void)
{
    return g_web_config_restart_armed;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 导出配置快照，不创建Web公共目录中的配置副本。
 */
int _linkg_web_handler_config_export(const cJSON *param, char **response)
{
    linkg_config_t config;
    cJSON         *data;
    cJSON         *backup;
    int            ret;

    (void)param;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    ret = linkg_config_create_snapshot(&config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_EXPORT, "读取当前配置失败", response);
    }

    ret = linkg_config_to_json(&config, &backup);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_EXPORT, "生成配置备份失败", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(backup);
        return -ENOMEM;
    }

    if (!cJSON_AddItemToObject(data, "config", backup))
    {
        cJSON_Delete(backup);
        cJSON_Delete(data);
        return -ENOMEM;
    }

    if (cJSON_AddStringToObject(data, "filename", "LinkG_Backup.json") == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_CONFIG_EXPORT, data, "配置备份已生成", response);
}

/**
 * @brief 完整校验、原子保存配置，在应答完成后重启全部服务。
 *
 * 配置事务由Dispatcher持有，导入文件不得影响本轮运行快照。
 */
int _linkg_web_handler_config_import(const cJSON *param, char **response)
{
    linkg_config_t             candidate;
    linkg_config_t             previous;
    linkg_network_ipv4_config_t ethernet;
    const cJSON               *content;
    const cJSON               *instance;
    const char                *current_instance;
    cJSON                     *root;
    cJSON                     *data;
    cJSON                     *expected;
    char                      *accepted;
    char                       address[INET_ADDRSTRLEN];
    int                        restored;
    int                        ret;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    if (!cJSON_IsObject(param) || !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(param, "confirm")))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, "请确认替换配置并重启LinkG服务", response);
    }

    instance         = cJSON_GetObjectItemCaseSensitive(param, "instance");
    content          = cJSON_GetObjectItemCaseSensitive(param, "content");
    current_instance = _linkg_web_network_basic_instance();
    if (!cJSON_IsString(instance) || current_instance == NULL || strcmp(instance->valuestring, current_instance) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, "服务实例已变化，请刷新后重新导入", response);
    }

    if (!cJSON_IsString(content) || content->valuestring[0] == '\0' || strlen(content->valuestring) > LINKG_WEB_CONFIG_FILE_MAX)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, "请选择不超过64 KiB的JSON配置文件", response);
    }

    root = NULL;
    ret = linkg_json_parse_buffer(content->valuestring, strlen(content->valuestring), &root);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, "配置JSON格式无效或存在重复字段", response);
    }

    ret = linkg_config_parse_json(root, &candidate);
    if (ret != 0 || cJSON_GetArraySize(root) != 5 || !cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(root, "uart")))
    {
        cJSON_Delete(root);
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, "配置不完整或业务参数非法，未修改当前配置", response);
    }

    if (linkg_config_create_snapshot(&previous) != 0 ||
        linkg_network_config_get_ethernet(&candidate.network, &ethernet) != 0 ||
        inet_ntop(AF_INET, &ethernet.ip, address, sizeof(address)) == NULL)
    {
        cJSON_Delete(root);
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, "无法读取或计算配置，请稍后重试", response);
    }

    // 先构造完整成功应答，避免落盘成功后因内存不足无法返回目标配置。
    data = cJSON_CreateObject();
    expected = NULL;
    ret = linkg_config_to_json(&candidate, &expected);
    if (data == NULL || ret != 0)
    {
        cJSON_Delete(expected);
        cJSON_Delete(data);
        cJSON_Delete(root);
        return -ENOMEM;
    }

    if (!cJSON_AddItemToObject(data, "expected_config", expected))
    {
        cJSON_Delete(expected);
        cJSON_Delete(data);
        cJSON_Delete(root);
        return -ENOMEM;
    }

    if (cJSON_AddStringToObject(data, "apply", "restarting") == NULL ||
        cJSON_AddStringToObject(data, "management_ip", address) == NULL)
    {
        cJSON_Delete(data);
        cJSON_Delete(root);
        return -ENOMEM;
    }

    accepted = NULL;
    ret = _linkg_web_response_success(LINKG_WEB_CMD_CONFIG_IMPORT, data, "配置已保存，正在重启LinkG服务", &accepted);
    if (ret != 0)
    {
        cJSON_Delete(root);
        return ret;
    }

    ret = linkg_config_import(root);
    cJSON_Delete(root);
    if (ret != 0)
    {
        linkg_json_string_free(accepted);
        restored = linkg_config_save(&previous);
        LINKG_LOG_ERROR("WEB: configuration import save failed=%d, restore=%d", ret, restored);
        return _linkg_web_response_error(LINKG_WEB_CMD_CONFIG_IMPORT, restored == 0 ?
                                         "配置保存失败，已恢复原配置" : "配置保存失败，持久化状态无法确认，请检查日志", response);
    }

    g_web_config_retry_at = 0U;
    g_web_config_restart_armed = true;
    *response                  = accepted;

    return 0;
}
