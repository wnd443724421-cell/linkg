/**
 * @file web_handler_uart.c
 * @brief 串口配置能力、异步应用及运行状态Web接口
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-15
 */

#include "web_internal.h"

#include <arpa/inet.h>
#include <errno.h>
#include <ifaddrs.h>
#include <inttypes.h>
#include <net/if.h>
#include <stdio.h>
#include <string.h>

#include "linkg_config.h"
#include "linkg_network_config.h"
#include "linkg_network.h"
#include "linkg_uart_manager.h"
#include "linkg_json.h"

/****************************** JSON辅助 ******************************/

/**
 * @brief 判断JSON字符串数组是否已经包含指定地址。
 */
static bool _uart_web_string_exists(const cJSON *array, const char *text)
{
    const cJSON *item;

    cJSON_ArrayForEach(item, array)
    {
        if (cJSON_IsString(item) && strcmp(item->valuestring, text) == 0)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 返回当前接口的定向广播地址，供页面即时校验。
 */
static int _uart_web_broadcast_addresses(cJSON *data)
{
    struct ifaddrs *interfaces;
    struct ifaddrs *item;
    cJSON          *array;
    cJSON          *address_item;
    char            address[INET_ADDRSTRLEN];
    int             ret;

    array = cJSON_AddArrayToObject(data, "broadcast_addresses");
    if (array == NULL)
    {
        return -ENOMEM;
    }
    if (getifaddrs(&interfaces) != 0)
    {
        return -errno;
    }

    ret = 0;
    for (item = interfaces; item != NULL; item = item->ifa_next)
    {
        if ((item->ifa_flags & IFF_BROADCAST) == 0 || item->ifa_broadaddr == NULL ||
            item->ifa_broadaddr->sa_family != AF_INET ||
            inet_ntop(AF_INET, &((struct sockaddr_in *)item->ifa_broadaddr)->sin_addr, address, sizeof(address)) == NULL ||
            _uart_web_string_exists(array, address))
        {
            continue;
        }
        address_item = cJSON_CreateString(address);
        if (address_item == NULL || !cJSON_AddItemToArray(array, address_item))
        {
            cJSON_Delete(address_item);
            ret = -ENOMEM;
            break;
        }
    }
    freeifaddrs(interfaces);

    return ret;
}

/**
 * @brief 输出应用状态名称，接受请求和提交成功具有不同语义。
 */
static const char *_uart_web_apply_name(linkg_uart_apply_state_t state)
{
    switch (state)
    {
        case LINKG_UART_APPLY_RUNNING:
            return "applying";
        case LINKG_UART_APPLY_APPLIED:
            return "applied";
        case LINKG_UART_APPLY_ROLLED_BACK:
            return "rolled_back";
        case LINKG_UART_APPLY_FAILED:
            return "failed";
        default:
            return "none";
    }
}

/**
 * @brief 将64位累计计数写为十进制字符串，避免JavaScript精度损失。
 */
static bool _uart_web_counter(cJSON *object, const char *name, uint64_t count)
{
    char text[32];

    snprintf(text, sizeof(text), "%" PRIu64, count);

    return cJSON_AddStringToObject(object, name, text) != NULL;
}

/**
 * @brief 构造由后端权威约束生成的端口、波特率和保留端口能力。
 */
static int _uart_web_capabilities(cJSON *data)
{
    const uint32_t *baudrates;
    cJSON          *array;
    cJSON          *ports;
    cJSON          *reserved;
    cJSON          *item;
    cJSON          *sbus;
    size_t          count;
    size_t          i;
    uint32_t        port;
    int             proto;

    array    = cJSON_AddArrayToObject(data, "baudrates");
    ports    = cJSON_AddArrayToObject(data, "ports");
    reserved = cJSON_AddArrayToObject(data, "reserved_local_ports");
    sbus     = cJSON_AddObjectToObject(data, "sbus");
    if (array == NULL || ports == NULL || reserved == NULL || sbus == NULL ||
        cJSON_AddNumberToObject(data, "max_ports", LINKG_UART_CONFIG_PORT_COUNT_MAX) == NULL ||
        cJSON_AddNumberToObject(data, "max_routes_per_port", LINKG_UART_CONFIG_ROUTE_COUNT_MAX) == NULL ||
        cJSON_AddNumberToObject(sbus, "uart_num", 0) == NULL ||
        cJSON_AddNumberToObject(sbus, "baudrate", LINKG_UART_CONFIG_SBUS_BAUDRATE) == NULL ||
        cJSON_AddNumberToObject(sbus, "data_bits", LINKG_UART_CONFIG_SBUS_DATA_BITS) == NULL ||
        cJSON_AddNumberToObject(sbus, "stop_bits", LINKG_UART_CONFIG_SBUS_STOP_BITS) == NULL ||
        cJSON_AddStringToObject(sbus, "parity", "e") == NULL)
    {
        return -ENOMEM;
    }

    baudrates = linkg_uart_config_baudrates(&count);
    for (i = 0U; i < count; i++)
    {
        item = cJSON_CreateNumber(baudrates[i]);
        if (item == NULL || !cJSON_AddItemToArray(array, item))
        {
            cJSON_Delete(item);
            return -ENOMEM;
        }
    }

    for (i = 0U; i < LINKG_UART_CONFIG_PORT_COUNT_MAX; i++)
    {
        item = cJSON_CreateObject();
        if (item == NULL || !cJSON_AddItemToArray(ports, item))
        {
            cJSON_Delete(item);
            return -ENOMEM;
        }
        if (cJSON_AddNumberToObject(item, "uart_num", (double)i) == NULL ||
            cJSON_AddStringToObject(item, "device_path", linkg_uart_manager_device_path((uint8_t)i)) == NULL ||
            cJSON_AddBoolToObject(item, "sbus_supported", i == 0U) == NULL)
        {
            return -ENOMEM;
        }
    }

    for (port = LINKG_RESOURCE_PORT_RESERVED_START; port <= LINKG_RESOURCE_PORT_RESERVED_END; port++)
    {
        for (proto = LINKG_UART_ROUTE_PROTO_TCP; proto <= LINKG_UART_ROUTE_PROTO_UDP; proto++)
        {
            if (!linkg_uart_config_reserved_port((linkg_uart_route_proto_t)proto, (uint16_t)port))
            {
                continue;
            }
            item = cJSON_CreateObject();
            if (item == NULL || !cJSON_AddItemToArray(reserved, item))
            {
                cJSON_Delete(item);
                return -ENOMEM;
            }
            if (cJSON_AddStringToObject(item, "proto", proto == LINKG_UART_ROUTE_PROTO_TCP ? "tcp" : "udp") == NULL ||
                cJSON_AddNumberToObject(item, "port", port) == NULL)
            {
                return -ENOMEM;
            }
        }
    }

    return _uart_web_broadcast_addresses(data);
}

/**
 * @brief 返回本机实际组网IPv4与其他节点的示例地址。
 *
 * 本机组网地址按照当前虚拟网络基址与Node ID生成，不使用固定IP。
 * 示例对端只用于输入框占位提示，不代表该节点在线。
 */
static int _uart_web_network_addresses(cJSON *data)
{
    linkg_network_config_t network;
    struct in_addr         local_address;
    struct in_addr         example_address;
    char                   local_ip[INET_ADDRSTRLEN];
    char                   example_ip[INET_ADDRSTRLEN];
    uint8_t                example_node_id;
    int                    ret;

    ret = linkg_network_get_running_config(&network);
    if (ret != 0)
    {
        return ret;
    }

    ret = linkg_network_config_get_node_address(&network, network.node_id, &local_address);
    if (ret != 0)
    {
        return -EINVAL;
    }

    example_node_id = network.node_id == LINKG_RESOURCE_NODE_ID_MIN ?
                      LINKG_RESOURCE_NODE_ID_MIN + 1U : LINKG_RESOURCE_NODE_ID_MIN;

    ret = linkg_network_config_get_node_address(&network, example_node_id, &example_address);
    if (ret != 0)
    {
        return -EINVAL;
    }

    if (inet_ntop(AF_INET, &local_address, local_ip, sizeof(local_ip)) == NULL ||
        inet_ntop(AF_INET, &example_address, example_ip, sizeof(example_ip)) == NULL)
    {
        return -EINVAL;
    }

    if (cJSON_AddStringToObject(data, "local_virtual_ip", local_ip) == NULL ||
        cJSON_AddStringToObject(data, "remote_virtual_ip_example", example_ip) == NULL)
    {
        return -ENOMEM;
    }

    return 0;
}

/****************************** 配置请求 ******************************/

/**
 * @brief 返回已经完成提交的UART配置和统一能力，不暴露应用中的候选配置。
 */
int _linkg_web_handler_uart_config_get(const cJSON *param, char **response)
{
    linkg_uart_config_t config;
    cJSON              *data;
    int                 ret;

    (void)param;
    ret = linkg_uart_manager_get_config(&config);
    if (ret != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_CONFIG_GET,
                                        ret == -EBUSY ? "UART正在应用配置，请稍后读取" : "UART配置不可用，请检查应用状态", response);
    }

    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    ret = linkg_uart_config_to_json(data, "uart", &config);
    if (ret == 0)
    {
        ret = _uart_web_capabilities(data);
    }
    if (ret == 0)
    {
        ret = _uart_web_network_addresses(data);
    }
    if (ret != 0)
    {
        cJSON_Delete(data);
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_CONFIG_GET, "构造串口配置失败", response);
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_UART_CONFIG_GET, data, NULL, response);
}

/**
 * @brief 完整校验UART快照后交给UART模块异步应用，Handler不操作Worker和fd。
 */
int _linkg_web_handler_uart_config_set(const cJSON *param, char **response)
{
    linkg_uart_config_t config;
    const cJSON        *uart;
    cJSON              *data;
    char               *pending_response;
    char               *none_response;
    uint32_t            job_id;
    char                job_text[11];
    char               *job_position;
    int                 ret;

    uart = cJSON_GetObjectItemCaseSensitive(param, "uart");
    if (!cJSON_IsObject(param) || param->child == NULL || param->child != uart || uart->next != NULL ||
        linkg_uart_config_parse(uart, &config) != 0)
    {
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_CONFIG_SET,
                                        "串口配置无效：检查完整字段、SBUS参数、IP、端口及重复监听规则", response);
    }

    // 在产生副作用前分配两种固定响应；具体任务编号由状态查询返回。
    data = cJSON_CreateObject();
    if (data == NULL || cJSON_AddStringToObject(data, "apply", "applying") == NULL ||
        cJSON_AddStringToObject(data, "job_id", "0000000000") == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    ret = _linkg_web_response_success(LINKG_WEB_CMD_UART_CONFIG_SET, data, "仅UART服务正在重新加载", &pending_response);
    if (ret != 0)
    {
        return ret;
    }
    data = cJSON_CreateObject();
    if (data == NULL || cJSON_AddStringToObject(data, "apply", "none") == NULL)
    {
        cJSON_Delete(data);
        linkg_json_string_free(pending_response);
        return -ENOMEM;
    }
    ret = _linkg_web_response_success(LINKG_WEB_CMD_UART_CONFIG_SET, data, NULL, &none_response);
    if (ret != 0)
    {
        linkg_json_string_free(pending_response);
        return ret;
    }

    job_position = strstr(pending_response, "0000000000");
    if (job_position == NULL)
    {
        linkg_json_string_free(pending_response);
        linkg_json_string_free(none_response);
        return -EPROTO;
    }

    ret = linkg_uart_manager_apply(&config, &job_id);
    if (ret != 0)
    {
        linkg_json_string_free(pending_response);
        linkg_json_string_free(none_response);
        return _linkg_web_response_error(LINKG_WEB_CMD_UART_CONFIG_SET,
                                        ret == -EBUSY ? "UART正在应用配置，请勿重复提交" : "UART暂不可应用，请检查服务状态和地址配置", response);
    }

    // 固定宽度任务标识在已分配响应中原位填入，接受任务后不再分配内存。
    snprintf(job_text, sizeof(job_text), "%010" PRIu32, job_id);
    memcpy(job_position, job_text, 10U);

    *response = job_id == 0U ? none_response : pending_response;
    linkg_json_string_free(job_id == 0U ? pending_response : none_response);

    return 0;
}

/****************************** 运行状态 ******************************/

/**
 * @brief 追加一个活动串口状态，设备打开和网络连接分别显示。
 */
static int _uart_web_port_status(cJSON *array, const linkg_uart_port_status_t *port)
{
    cJSON    *item;
    cJSON    *routes;
    cJSON    *route;
    cJSON    *baud;
    uint32_t  i;

    item = cJSON_CreateObject();
    if (item == NULL || !cJSON_AddItemToArray(array, item))
    {
        cJSON_Delete(item);
        return -ENOMEM;
    }
    routes = cJSON_AddArrayToObject(item, "routes");
    if (routes == NULL || cJSON_AddNumberToObject(item, "uart_num", port->uart_num) == NULL ||
        cJSON_AddStringToObject(item, "device_path", port->device_path) == NULL ||
        cJSON_AddBoolToObject(item, "device_open", port->device_open) == NULL ||
        cJSON_AddBoolToObject(item, "sbus_running", port->sbus_running) == NULL ||
        cJSON_AddNumberToObject(item, "route_count", port->route_count) == NULL ||
        !_uart_web_counter(item, "uart_rx_bytes", port->uart_rx_bytes) ||
        !_uart_web_counter(item, "uart_tx_bytes", port->uart_tx_bytes) ||
        !_uart_web_counter(item, "net_rx_bytes", port->net_rx_bytes) ||
        !_uart_web_counter(item, "net_tx_bytes", port->net_tx_bytes) ||
        !_uart_web_counter(item, "errors", port->errors) || !_uart_web_counter(item, "drops", port->drops))
    {
        return -ENOMEM;
    }
    for (i = 0U; i < port->route_count; i++)
    {
        route = cJSON_CreateString(port->routes[i]);
        if (route == NULL || !cJSON_AddItemToArray(routes, route))
        {
            cJSON_Delete(route);
            return -ENOMEM;
        }
    }
    baud = cJSON_AddObjectToObject(item, "baud");
    if (baud == NULL || cJSON_AddBoolToObject(baud, "verified", port->baud_verified) == NULL)
    {
        return -ENOMEM;
    }
    if (port->baud_verified &&
        (cJSON_AddNumberToObject(baud, "baud_base", port->baud.baud_base) == NULL ||
         cJSON_AddNumberToObject(baud, "divisor", port->baud.divisor) == NULL ||
         cJSON_AddNumberToObject(baud, "actual", port->baud.actual) == NULL ||
         cJSON_AddNumberToObject(baud, "error_pct", port->baud.error_pct) == NULL))
    {
        return -ENOMEM;
    }

    return 0;
}

/**
 * @brief 返回生命周期、最终应用结果和活动端口采样，不把start成功当成链路连通。
 */
int _linkg_web_handler_uart_status_get(const cJSON *param, char **response)
{
    linkg_uart_status_t       status;
    linkg_uart_apply_status_t apply;
    cJSON                    *data;
    cJSON                    *ports;
    cJSON                    *job;
    uint32_t                  i;

    (void)param;
    linkg_uart_manager_get_status(&status);
    linkg_uart_manager_get_apply_status(&apply);
    data = cJSON_CreateObject();
    if (data == NULL)
    {
        return -ENOMEM;
    }
    ports = cJSON_AddArrayToObject(data, "ports");
    job   = cJSON_AddObjectToObject(data, "apply");
    if (ports == NULL || job == NULL ||
        cJSON_AddStringToObject(data, "state", apply.state == LINKG_UART_APPLY_RUNNING ? "reloading" : status.state) == NULL ||
        cJSON_AddBoolToObject(data, "worker_running", status.worker_running) == NULL ||
        cJSON_AddNumberToObject(job, "job_id", apply.job_id) == NULL ||
        cJSON_AddStringToObject(job, "state", _uart_web_apply_name(apply.state)) == NULL ||
        cJSON_AddNumberToObject(job, "error", apply.error) == NULL ||
        cJSON_AddNumberToObject(job, "rollback_error", apply.rollback_error) == NULL ||
        cJSON_AddNumberToObject(job, "elapsed_ms", (double)apply.elapsed_ms) == NULL)
    {
        cJSON_Delete(data);
        return -ENOMEM;
    }
    for (i = 0U; i < status.port_count; i++)
    {
        if (_uart_web_port_status(ports, &status.ports[i]) != 0)
        {
            cJSON_Delete(data);
            return -ENOMEM;
        }
    }

    return _linkg_web_response_success(LINKG_WEB_CMD_UART_STATUS_GET, data, NULL, response);
}
