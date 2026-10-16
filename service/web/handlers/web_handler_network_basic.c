/**
 * @file web_handler_network_basic.c
 * @brief LinkG基础网络配置事务及重启确认实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "linkg_config.h"
#include "linkg_json.h"
#include "linkg_lifecycle.h"
#include "linkg_log.h"
#include "linkg_udhcp.h"

/****************************** 全局上下文 ******************************/

// Web最后启动；初始化及反初始化期间无请求，运行期间仅Web服务线程访问。
static linkg_network_config_t g_web_network_running;                // 服务启动时的运行快照
static char                   g_web_network_instance[80];           // 每轮完整服务初始化的标识
static bool                   g_web_network_restart_armed  = false; // 等待当前响应发送完成
static bool                   g_web_network_restart_failed = false; // 生命周期拒绝了重启请求
static bool                   g_web_network_dhcp_uncertain = false; // DHCP恢复失败，不能确认运行状态
static bool                   g_web_network_save_uncertain = false; // 持久配置恢复失败，不能确认已保存值

/****************************** 内部辅助 ******************************/

/**
 * @brief 判断基础地址是否一致。
 */
static bool _linkg_web_network_addresses_equal(const linkg_network_config_t *left, const linkg_network_config_t *right)
{
    return left->node_id == right->node_id &&
           left->virtual_network.s_addr == right->virtual_network.s_addr &&
           left->ethernet_network.s_addr == right->ethernet_network.s_addr;
}

/**
 * @brief 判断本页面管理的五项配置是否一致。
 */
static bool _linkg_web_network_equal(const linkg_network_config_t *left, const linkg_network_config_t *right)
{
    return _linkg_web_network_addresses_equal(left, right) &&
           left->dhcp.enabled == right->dhcp.enabled &&
           left->dhcp.default_gateway == right->dhcp.default_gateway;
}

/**
 * @brief 添加一份基础网络快照，所有JSON子对象由parent持有。
 */
static int _linkg_web_network_json(cJSON *parent, const char *key, const linkg_network_config_t *config)
{
    cJSON                       *object;
    cJSON                       *dhcp;
    linkg_network_ipv4_config_t ethernet;
    struct in_addr              virtual_ip;
    char                        virtual_text[INET_ADDRSTRLEN];
    char                        ethernet_text[INET_ADDRSTRLEN];
    char                        network_text[INET_ADDRSTRLEN];
    char                        virtual_network_text[INET_ADDRSTRLEN];

    if (linkg_network_config_get_ethernet(config, &ethernet) != 0 ||
        linkg_network_config_get_node_address(config, config->node_id, &virtual_ip) != 0)
    {
        return -EINVAL;
    }

    if (inet_ntop(AF_INET, &virtual_ip, virtual_text, sizeof(virtual_text)) == NULL ||
        inet_ntop(AF_INET, &ethernet.ip, ethernet_text, sizeof(ethernet_text)) == NULL ||
        inet_ntop(AF_INET, &config->virtual_network, virtual_network_text, sizeof(virtual_network_text)) == NULL ||
        inet_ntop(AF_INET, &config->ethernet_network, network_text, sizeof(network_text)) == NULL)
    {
        return -EINVAL;
    }

    object = cJSON_AddObjectToObject(parent, key);
    if (object == NULL)
    {
        return -ENOMEM;
    }

    dhcp = cJSON_AddObjectToObject(object, "dhcp");
    if (dhcp == NULL ||
        linkg_json_add_int(object, "node_id", config->node_id) != 0 ||
        linkg_json_add_string(object, "virtual_network", virtual_network_text) != 0 ||
        linkg_json_add_string(object, "ethernet_network", network_text) != 0 ||
        linkg_json_add_string(object, "virtual_ip", virtual_text) != 0 ||
        linkg_json_add_string(object, "ethernet_ip", ethernet_text) != 0 ||
        linkg_json_add_bool(dhcp, "enabled", config->dhcp.enabled) != 0 ||
        linkg_json_add_bool(dhcp, "default_gateway", config->dhcp.default_gateway) != 0)
    {
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief 在配置副作用之前完整构造响应。
 */
static int _linkg_web_network_apply_response(int cmd, const char *apply, char **response)
{
    cJSON *data = cJSON_CreateObject();

    if (data == NULL)
    {
        return -ENOMEM;
    }

    if (linkg_json_add_string(data, "apply", apply) != 0)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }

    return _linkg_web_response_success(cmd, data, NULL, response);
}

/**
 * @brief 严格读取五项配置，仅修改调用方提供的完整快照对应字段。
 */
static int _linkg_web_network_parse(const cJSON *param, linkg_network_config_t *network)
{
    const cJSON *dhcp;
    uint32_t     node_id;
    char         virtual_network[INET_ADDRSTRLEN];
    char         ethernet_network[INET_ADDRSTRLEN];

    if (!cJSON_IsObject(param) ||
        linkg_json_get_uint32(param, "node_id", &node_id) != 0 || node_id < 1U || node_id > 254U ||
        linkg_json_get_string(param, "virtual_network", virtual_network, sizeof(virtual_network)) != 0 ||
        linkg_json_get_string(param, "ethernet_network", ethernet_network, sizeof(ethernet_network)) != 0 ||
        linkg_json_get_object(param, "dhcp", &dhcp) != 0 ||
        linkg_json_get_bool(dhcp, "enabled", &network->dhcp.enabled) != 0 ||
        linkg_json_get_bool(dhcp, "default_gateway", &network->dhcp.default_gateway) != 0)
    {
        return -EINVAL;
    }

    network->node_id = (uint8_t)node_id;
    if (inet_pton(AF_INET, virtual_network, &network->virtual_network) != 1 ||
        inet_pton(AF_INET, ethernet_network, &network->ethernet_network) != 1)
    {
        return -EINVAL;
    }

    return linkg_network_config_validate(network) == 0 ? 0 : -EINVAL;
}

/****************************** 服务接口 ******************************/

/**
 * @brief 捕获本轮服务启动配置，不能从保存后的全局快照推断运行地址。
 */
int _linkg_web_network_basic_init(void)
{
    struct timespec now;
    int             ret;

    ret = linkg_config_get_network(&g_web_network_running);
    if (ret != 0)
    {
        return ret;
    }

    if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    {
        return -errno;
    }

    snprintf(g_web_network_instance, sizeof(g_web_network_instance), "%ld-%lld-%ld",
             (long)getpid(), (long long)now.tv_sec, now.tv_nsec);
    g_web_network_restart_armed  = false;
    g_web_network_restart_failed = false;
    g_web_network_dhcp_uncertain = false;
    g_web_network_save_uncertain = false;
    return 0;
}

/**
 * @brief 获取当前Web服务实例标识。
 */
const char *_linkg_web_network_basic_instance(void)
{
    return g_web_network_instance;
}

/**
 * @brief 只在确认响应已完整发送并关闭连接后通知主线程重启服务。
 *
 * 由Web服务线程调用；短暂宽限让独立CGI进程完成HTTP输出，无额外管理线程。
 */
void _linkg_web_network_response_complete(bool sent)
{
    int ret;

    if (!g_web_network_restart_armed)
    {
        return;
    }

    g_web_network_restart_armed = false;
    if (!sent)
    {
        g_web_network_restart_failed = true;
        return;
    }

    usleep(300000U);
    ret = linkg_lifecycle_request_exit(LINKG_EXIT_ACTION_RESTART_SERVICES);
    g_web_network_restart_failed = ret != 0;
    if (ret != 0)
    {
        LINKG_LOG_ERROR("WEB: service restart request rejected, error=%d", ret);
    }
}

/****************************** 请求处理 ******************************/

/**
 * @brief 查询运行配置与已保存配置。
 */
int _linkg_web_handler_network_basic_get(const cJSON *param, char **response)
{
    linkg_config_t saved;
    cJSON         *data;
    int            ret;

    (void)param;
    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    ret = linkg_config_create_snapshot(&saved);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_GET, "读取网络配置失败", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }

    ret = _linkg_web_network_json(data, "running", &g_web_network_running);
    if (ret == 0)
    {
        ret = _linkg_web_network_json(data, "saved", &saved.network);
    }

    if (ret != 0 ||
        linkg_json_add_bool(data, "pending_restart", !_linkg_web_network_equal(&saved.network, &g_web_network_running)) != 0 ||
        linkg_json_add_bool(data, "runtime_confirmed", !g_web_network_dhcp_uncertain) != 0 ||
        linkg_json_add_bool(data, "persistence_confirmed", !g_web_network_save_uncertain) != 0 ||
        linkg_json_add_bool(data, "restart_failed", g_web_network_restart_failed) != 0 ||
        linkg_json_add_string(data, "instance", g_web_network_instance) != 0)
    {
        cJSON_Delete(data);
        return ret != 0 ? ret : -ENOMEM;
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_NETWORK_BASIC_GET, data, NULL, response);
}

/**
 * @brief 保存基础网络；配置写事务由分发层持有。
 */
int _linkg_web_handler_network_basic_set(const cJSON *param, char **response)
{
    linkg_config_t         old_config;
    linkg_config_t         new_config;
    linkg_network_config_t dynamic_config;
    char                  *success_response = NULL;
    bool                   restart;
    bool                   reload_attempted = false;
    int                    save_restore;
    int                    memory_restore;
    int                    dhcp_restore = 0;
    int                    ret;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    ret = linkg_config_create_snapshot(&old_config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_SET, "读取配置失败", response);
    }

    new_config = old_config;
    ret = _linkg_web_network_parse(param, &new_config.network);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_SET, "网络配置无效：检查JSON类型、节点1–254、私有/16和/24网段及地址重叠", response);
    }

    if (_linkg_web_network_equal(&old_config.network, &new_config.network))
    {
        return _linkg_web_network_apply_response(LINKG_WEB_CMD_NETWORK_BASIC_SET, "none", response);
    }

    restart = !_linkg_web_network_addresses_equal(&old_config.network, &new_config.network);
    ret = _linkg_web_network_apply_response(LINKG_WEB_CMD_NETWORK_BASIC_SET,
                                           restart ? "restart_required" : "dynamic", &success_response);
    if (ret != 0)
    {
        return ret;
    }

    // save可能在rename后的目录fsync阶段失败，所有失败路径均尝试恢复持久配置。
    ret = linkg_config_save(&new_config);
    if (ret == 0)
    {
        ret = linkg_config_replace(&new_config);
    }

    if (ret == 0 && !restart)
    {
        // 已保存地址可能尚待重启，动态DHCP必须继续使用当前运行地址池。
        dynamic_config      = g_web_network_running;
        dynamic_config.dhcp = new_config.network.dhcp;
        reload_attempted    = true;
        ret = linkg_udhcp_reload(&dynamic_config);
    }

    if (ret != 0)
    {
        linkg_json_string_free(success_response);
        save_restore   = linkg_config_save(&old_config);
        memory_restore = linkg_config_replace(&old_config);
        g_web_network_save_uncertain = save_restore != 0 || memory_restore != 0;
        if (reload_attempted)
        {
            dhcp_restore = linkg_udhcp_reload(&g_web_network_running);
            g_web_network_dhcp_uncertain = dhcp_restore != 0;
        }

        LINKG_LOG_ERROR("WEB: network save failed=%d, restore file=%d memory=%d dhcp=%d",
                        ret, save_restore, memory_restore, dhcp_restore);
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_SET,
                save_restore != 0 || memory_restore != 0 || dhcp_restore != 0 ?
                "配置更新失败，恢复未完全成功；运行状态无法确认，请检查设备日志后重试或重启服务" :
                "配置更新失败，已恢复原配置", response);
    }

    if (!restart)
    {
        g_web_network_running.dhcp = new_config.network.dhcp;
        g_web_network_dhcp_uncertain = false;
    }

    *response = success_response;
    g_web_network_save_uncertain = false;
    return 0;
}

/**
 * @brief 用户确认后准备重启；实际通知在响应完成回调中执行。
 */
int _linkg_web_handler_network_basic_restart(const cJSON *param, char **response)
{
    linkg_config_t         saved;
    linkg_network_config_t confirmed;
    char                   instance[sizeof(g_web_network_instance)];
    int                    ret;

    if (response == NULL)
    {
        return -EINVAL;
    }

    *response = NULL;
    if (g_web_network_save_uncertain)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_RESTART, "持久配置状态无法确认，请先重新保存有效配置再重启", response);
    }

    if (linkg_json_get_string(param, "instance", instance, sizeof(instance)) != 0 ||
        strcmp(instance, g_web_network_instance) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_RESTART, "服务状态已变化，请重新加载后确认", response);
    }

    ret = linkg_config_create_snapshot(&saved);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_RESTART, "读取配置失败，未重启", response);
    }

    if (_linkg_web_network_equal(&saved.network, &g_web_network_running) && !g_web_network_dhcp_uncertain)
    {
        return _linkg_web_network_apply_response(LINKG_WEB_CMD_NETWORK_BASIC_RESTART, "none", response);
    }

    confirmed = saved.network;
    if (_linkg_web_network_parse(param, &confirmed) != 0 || !_linkg_web_network_equal(&confirmed, &saved.network))
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_NETWORK_BASIC_RESTART, "已保存配置已变化，请重新加载后确认重启", response);
    }

    ret = _linkg_web_network_apply_response(LINKG_WEB_CMD_NETWORK_BASIC_RESTART, "restarting", response);
    if (ret == 0)
    {
        g_web_network_restart_armed = true;
        g_web_network_restart_failed = false;
    }

    return ret;
}
