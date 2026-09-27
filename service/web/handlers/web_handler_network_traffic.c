/**
 * @file web_handler_network_traffic.c
 * @brief LinkG Web业务流量分类配置处理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-27
 */

#include "web_internal.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_network_config.h"
#include "linkg_tun.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断两份业务流量分类配置是否完全相同。
 */
static bool _linkg_web_traffic_config_equal(const linkg_network_traffic_config_t *left, const linkg_network_traffic_config_t *right)
{
    const linkg_network_traffic_rule_t *left_rule;
    const linkg_network_traffic_rule_t *right_rule;
    uint32_t                            index;

    if (left == NULL || right == NULL || left->count != right->count)
    {
        return false;
    }

    for (index = 0U; index < left->count; index++)
    {
        left_rule  = &left->rules[index];
        right_rule = &right->rules[index];

        if (left_rule->traffic_class != right_rule->traffic_class ||
            left_rule->protocol != right_rule->protocol ||
            left_rule->start_port != right_rule->start_port ||
            left_rule->end_port != right_rule->end_port)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief 从完整Network配置生成traffic_rules JSON数组。
 *
 * 配置模块不对外暴露Traffic子配置序列化接口，
 * 因此通过公开的完整Network序列化接口获取traffic_rules。
 */
static int _linkg_web_traffic_rules_to_json(const linkg_network_config_t *config, cJSON **out)
{
    cJSON *root;
    cJSON *network;
    cJSON *rules;
    int    ret;

    if (config == NULL || out == NULL)
    {
        return -EINVAL;
    }

    *out = NULL;

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_network_config_to_json(root, "network", config);
    if (ret != 0)
    {
        cJSON_Delete(root);
        return ret;
    }

    network = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (network == NULL || !cJSON_IsObject(network))
    {
        cJSON_Delete(root);
        return -EINVAL;
    }

    rules = cJSON_DetachItemFromObjectCaseSensitive(network, "traffic_rules");
    if (rules == NULL || !cJSON_IsArray(rules))
    {
        cJSON_Delete(rules);
        cJSON_Delete(root);
        return -EINVAL;
    }

    cJSON_Delete(root);

    *out = rules;

    return 0;
}

/**
 * @brief 基于当前Network配置解析新的traffic_rules。
 *
 * 配置模块仅公开完整Network配置解析接口，因此先将当前Network配置
 * 序列化为JSON，再替换traffic_rules，最后重新执行完整Network解析和校验。
 */
static int _linkg_web_traffic_parse_network(const linkg_network_config_t *current, const cJSON *rules, linkg_network_config_t *out)
{
    cJSON *root;
    cJSON *network;
    cJSON *rules_copy;
    int    ret;

    if (current == NULL || rules == NULL || out == NULL || !cJSON_IsArray(rules))
    {
        return -EINVAL;
    }

    root = cJSON_CreateObject();
    if (root == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_network_config_to_json(root, "network", current);
    if (ret != 0)
    {
        cJSON_Delete(root);
        return ret;
    }

    network = cJSON_GetObjectItemCaseSensitive(root, "network");
    if (network == NULL || !cJSON_IsObject(network))
    {
        cJSON_Delete(root);
        return -EINVAL;
    }

    rules_copy = cJSON_Duplicate(rules, true);
    if (rules_copy == NULL)
    {
        cJSON_Delete(root);
        return -ENOMEM;
    }

    if (!cJSON_ReplaceItemInObjectCaseSensitive(network, "traffic_rules", rules_copy))
    {
        cJSON_Delete(rules_copy);
        cJSON_Delete(root);
        return -EINVAL;
    }

    ret = linkg_network_config_parse(network, out);

    cJSON_Delete(root);

    return ret;
}

/**
 * @brief 构造配置应用结果。
 */
static int _linkg_web_traffic_apply_response(const char *apply, char **response)
{
    cJSON *data;
    int    ret;

    if (apply == NULL || response == NULL)
    {
        return -EINVAL;
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = linkg_json_add_string(data, "apply", apply);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(data);
        return ret == LINKG_JSON_ERR_MEMORY ? -ENOMEM : -EINVAL;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, data, NULL, response);
}

/**
 * @brief 在业务流量配置提交失败后恢复旧配置。
 */
static int _linkg_web_traffic_restore_config(const linkg_config_t *config)
{
    int first_error;
    int ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    first_error = 0;

    ret = linkg_config_save(config, NULL);
    if (ret != 0)
    {
        first_error = ret;
    }

    ret = linkg_config_replace(config);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/****************************** 请求处理 ******************************/

/**
 * @brief 处理业务流量分类配置查询。
 */
int _linkg_web_handler_traffic_config_get(const cJSON *param, char **response)
{
    linkg_config_t config;
    cJSON         *data;
    cJSON         *rules;
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
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_GET, "获取当前配置失败", response);
    }

    rules = NULL;

    ret = _linkg_web_traffic_rules_to_json(&config.network, &rules);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_GET, "构造流量规则配置失败", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        cJSON_Delete(rules);
        return -ENOMEM;
    }

    ret = linkg_json_add_array(data, "traffic_rules", rules);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(rules);
        cJSON_Delete(data);
        return ret == LINKG_JSON_ERR_MEMORY ? -ENOMEM : -EINVAL;
    }

    ret = linkg_json_add_uint32(data, "max_rules", LINKG_NETWORK_TRAFFIC_RULE_MAX);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(data, "fallback_class", "data");
    }

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(data);
        return ret == LINKG_JSON_ERR_MEMORY ? -ENOMEM : -EINVAL;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_TRAFFIC_CONFIG_GET, data, NULL, response);
}

/**
 * @brief 处理业务流量分类配置更新。
 */
int _linkg_web_handler_traffic_config_set(const cJSON *param, char **response)
{
    linkg_config_t old_config;
    linkg_config_t new_config;
    const cJSON   *rules;
    const char    *error_message;
    char          *success_response;
    int            restore_ret;
    int            ret;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;

    if (param == NULL)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, "流量规则配置无效", response);
    }

    /**
     * SET必须显式提供traffic_rules。
     * 空数组表示清空规则，字段缺失不能被解释为清空。
     */
    ret = linkg_json_get_array(param, "traffic_rules", &rules);
    if (ret != LINKG_JSON_OK)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, "traffic_rules必须为JSON数组", response);
    }

    ret = linkg_config_create_snapshot(&old_config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, "获取当前配置失败", response);
    }

    new_config = old_config;

    /**
     * Traffic子配置没有独立公共解析接口。
     * 基于当前完整Network配置替换traffic_rules，
     * 再通过Network公共接口统一完成解析和校验。
     */
    ret = _linkg_web_traffic_parse_network(&old_config.network, rules, &new_config.network);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, "流量规则配置无效", response);
    }

    if (_linkg_web_traffic_config_equal(&old_config.network.traffic, &new_config.network.traffic))
    {
        return _linkg_web_traffic_apply_response("none", response);
    }

    /**
     * 在产生任何配置副作用前完整构造成功响应，
     * 避免配置已经生效后因响应分配或序列化失败向调用方报告失败。
     */
    success_response = NULL;

    ret = _linkg_web_traffic_apply_response("dynamic", &success_response);
    if (ret != 0)
    {
        return ret;
    }

    error_message = NULL;

    /**
     * 即使save返回失败，也不能假定正式配置文件没有变化。
     * 原子保存可能已经完成rename，仅在父目录fsync阶段失败。
     */
    ret = linkg_config_save(&new_config, NULL);
    if (ret != 0)
    {
        error_message = "保存流量规则失败";
        goto rollback;
    }

    ret = linkg_config_replace(&new_config);
    if (ret != 0)
    {
        error_message = "更新全局配置失败";
        goto rollback;
    }

    ret = linkg_tun_update_traffic_config(&new_config.network.traffic);
    if (ret != 0)
    {
        error_message = "动态应用流量规则失败";
        goto rollback;
    }

    *response = success_response;

    return 0;

rollback:
    linkg_json_string_free(success_response);

    /**
     * TUN更新只有ioctl完整成功后才替换运行态副本，
     * 因此更新失败时TUN仍保持旧规则，只恢复持久配置和全局快照。
     */
    restore_ret = _linkg_web_traffic_restore_config(&old_config);
    if (restore_ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, "流量规则更新失败，恢复原配置也失败", response);
    }

    return _linkg_web_response_error(LINKG_WEB_CMD_TRAFFIC_CONFIG_SET, error_message, response);
}
