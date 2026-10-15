/**
 * @file linkg_uart_config.c
 * @brief LinkG串口及网络路由配置解析、校验和序列化
 * @author Dawn
 * @version 1.0.0
 * @date 2026-10-14
 */

#include "linkg_uart_config.h"

#include <arpa/inet.h>
#include <stddef.h>
#include <string.h>

#include "linkg_json.h"
#include "linkg_system_resources.h"

#include "config_internal.h"

/****************************** 配置常量 ******************************/

#define LINKG_UART_CONFIG_ENUM_STRING_MAX  16U     // 枚举字符串缓冲区长度

/****************************** 枚举转换 ******************************/

/**
 * @brief 将串口类型字符串转换为枚举。
 */
static int _uart_type_from_string(const char *value, linkg_uart_port_type_t *out)
{
    if (value == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(value, "normal") == 0)
    {
        *out = LINKG_UART_PORT_TYPE_NORMAL;
        return CONFIG_OK;
    }

    if (strcmp(value, "sbus") == 0)
    {
        *out = LINKG_UART_PORT_TYPE_SBUS;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将串口类型枚举转换为字符串。
 */
static const char *_uart_type_to_string(linkg_uart_port_type_t type)
{
    switch (type)
    {
        case LINKG_UART_PORT_TYPE_NORMAL:
            return "normal";

        case LINKG_UART_PORT_TYPE_SBUS:
            return "sbus";

        default:
            return NULL;
    }
}

/**
 * @brief 将路由协议字符串转换为枚举。
 */
static int _uart_proto_from_string(const char *value, linkg_uart_route_proto_t *out)
{
    if (value == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(value, "tcp") == 0)
    {
        *out = LINKG_UART_ROUTE_PROTO_TCP;
        return CONFIG_OK;
    }

    if (strcmp(value, "udp") == 0)
    {
        *out = LINKG_UART_ROUTE_PROTO_UDP;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将路由协议枚举转换为字符串。
 */
static const char *_uart_proto_to_string(linkg_uart_route_proto_t proto)
{
    switch (proto)
    {
        case LINKG_UART_ROUTE_PROTO_TCP:
            return "tcp";

        case LINKG_UART_ROUTE_PROTO_UDP:
            return "udp";

        default:
            return NULL;
    }
}

/**
 * @brief 将路由端点角色字符串转换为枚举。
 */
static int _uart_endpoint_from_string(const char *value, linkg_uart_route_endpoint_t *out)
{
    if (value == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(value, "local") == 0)
    {
        *out = LINKG_UART_ROUTE_ENDPOINT_LOCAL;
        return CONFIG_OK;
    }

    if (strcmp(value, "remote") == 0)
    {
        *out = LINKG_UART_ROUTE_ENDPOINT_REMOTE;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将路由端点角色枚举转换为字符串。
 */
static const char *_uart_endpoint_to_string(linkg_uart_route_endpoint_t endpoint)
{
    switch (endpoint)
    {
        case LINKG_UART_ROUTE_ENDPOINT_LOCAL:
            return "local";

        case LINKG_UART_ROUTE_ENDPOINT_REMOTE:
            return "remote";

        default:
            return NULL;
    }
}

/**
 * @brief 将路由方向字符串转换为枚举。
 */
static int _uart_dir_from_string(const char *value, linkg_uart_route_dir_t *out)
{
    if (value == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(value, "tx") == 0)
    {
        *out = LINKG_UART_ROUTE_DIR_TX;
        return CONFIG_OK;
    }

    if (strcmp(value, "rx") == 0)
    {
        *out = LINKG_UART_ROUTE_DIR_RX;
        return CONFIG_OK;
    }

    if (strcmp(value, "rxtx") == 0)
    {
        *out = LINKG_UART_ROUTE_DIR_BOTH;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将路由方向枚举转换为字符串。
 */
static const char *_uart_dir_to_string(linkg_uart_route_dir_t dir)
{
    switch (dir)
    {
        case LINKG_UART_ROUTE_DIR_TX:
            return "tx";

        case LINKG_UART_ROUTE_DIR_RX:
            return "rx";

        case LINKG_UART_ROUTE_DIR_BOTH:
            return "rxtx";

        default:
            return NULL;
    }
}

/****************************** 配置校验 ******************************/

/**
 * @brief 返回配置校验和Web能力列表共用的波特率集合。
 */
const uint32_t *linkg_uart_config_baudrates(size_t *count)
{
    static const uint32_t baudrates[] =
    {
        1200U, 2400U, 4800U, 9600U, 19200U, 38400U, 57600U,
        100000U, 115200U, 150000U, 187500U, 250000U, 300000U,
        375000U, 500000U, 750000U, 1500000U
    };

    if (count != NULL)
    {
        *count = sizeof(baudrates) / sizeof(baudrates[0]);
    }

    return baudrates;
}

/**
 * @brief 检查配置波特率是否属于软件支持集合，硬件分频在打开设备时校验。
 */
static bool _uart_baudrate_supported(uint32_t baudrate)
{
    const uint32_t *baudrates;
    size_t          count;
    size_t          index;

    baudrates = linkg_uart_config_baudrates(&count);
    for (index = 0U; index < count; index++)
    {
        if (baudrates[index] == baudrate)
        {
            return true;
        }
    }

    return false;
}

/**
 * @brief 按协议检查LinkG固定监听和预留端口，不禁止TCP与UDP合法共用端口。
 */
bool linkg_uart_config_reserved_port(linkg_uart_route_proto_t proto, uint16_t port)
{
    if (proto == LINKG_UART_ROUTE_PROTO_UDP)
    {
        return port >= LINKG_RESOURCE_UDP_PORT_WIFI_DATA && port <= LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY;
    }

    return port == LINKG_RESOURCE_TCP_PORT_WEB_CONTROL || port == LINKG_RESOURCE_TCP_PORT_WEBSOCKET;
}

/**
 * @brief 拒绝未知或重复JSON字段，避免旧协议字段被静默忽略。
 */
static bool _uart_fields_valid(const cJSON *node, const char *const *names, size_t count)
{
    const cJSON *field;
    const cJSON *other;
    size_t       index;

    cJSON_ArrayForEach(field, node)
    {
        if (field->string == NULL)
        {
            return false;
        }

        for (index = 0U; index < count; index++)
        {
            if (strcmp(field->string, names[index]) == 0)
            {
                break;
            }
        }

        if (index == count)
        {
            return false;
        }

        for (other = field->next; other != NULL; other = other->next)
        {
            if (other->string != NULL && strcmp(field->string, other->string) == 0)
            {
                return false;
            }
        }
    }

    return true;
}

/**
 * @brief 校验串口物理配置与逻辑端口约束。
 */
static int _uart_port_config_validate(uint8_t uart_num, const linkg_uart_port_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (uart_num >= LINKG_UART_CONFIG_PORT_COUNT_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->type == LINKG_UART_PORT_TYPE_SBUS)
    {
        if (uart_num != 0U || config->baudrate != LINKG_UART_CONFIG_SBUS_BAUDRATE ||
            config->data_bits != LINKG_UART_CONFIG_SBUS_DATA_BITS ||
            config->stop_bits != LINKG_UART_CONFIG_SBUS_STOP_BITS ||
            config->parity != 'e')
        {
            return CONFIG_ERR_VALIDATE;
        }

        return CONFIG_OK;
    }

    if (config->type != LINKG_UART_PORT_TYPE_NORMAL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!_uart_baudrate_supported(config->baudrate))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->data_bits != 7U && config->data_bits != 8U)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->stop_bits != 1U && config->stop_bits != 2U)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->parity != 'n' && config->parity != 'e' && config->parity != 'o')
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验路由地址是否符合IPv4单播或本地绑定语义。
 */
static int _uart_route_address_validate(const linkg_uart_route_config_t *config)
{
    uint32_t address;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    address = ntohl(config->ip.s_addr);

    if (address == INADDR_BROADCAST || (address & 0xF0000000U) == 0xE0000000U ||
        (address & 0xF0000000U) == 0xF0000000U)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if ((address >> 24) == 0U &&
        (address != INADDR_ANY || config->endpoint == LINKG_UART_ROUTE_ENDPOINT_REMOTE))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验单条串口网络路由。
 */
static int _uart_route_config_validate(const linkg_uart_route_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->proto != LINKG_UART_ROUTE_PROTO_TCP && config->proto != LINKG_UART_ROUTE_PROTO_UDP)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->endpoint != LINKG_UART_ROUTE_ENDPOINT_LOCAL && config->endpoint != LINKG_UART_ROUTE_ENDPOINT_REMOTE)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->dir != LINKG_UART_ROUTE_DIR_TX && config->dir != LINKG_UART_ROUTE_DIR_RX &&
        config->dir != LINKG_UART_ROUTE_DIR_BOTH)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->port == 0U)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (config->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL &&
        linkg_uart_config_reserved_port(config->proto, config->port))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return _uart_route_address_validate(config);
}

/****************************** 配置解析 ******************************/

/**
 * @brief 从JSON读取串口物理配置。
 */
static int _uart_port_config_parse(const cJSON *node, linkg_uart_port_config_t *out)
{
    linkg_uart_port_config_t temp = {0};
    char                     value[LINKG_UART_CONFIG_ENUM_STRING_MAX];
    uint16_t                 number;
    int                      ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    const char *const fields[] = {"type", "baudrate", "data_bits", "stop_bits", "parity"};

    if (!cJSON_IsObject(node) || !_uart_fields_valid(node, fields, sizeof(fields) / sizeof(fields[0])))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_string(node, "type", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _uart_type_from_string(value, &temp.type);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_get_uint32(node, "baudrate", &temp.baudrate);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = linkg_json_get_uint16(node, "data_bits", &number);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (number > UINT8_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    temp.data_bits = (uint8_t)number;

    ret = linkg_json_get_uint16(node, "stop_bits", &number);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (number > UINT8_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    temp.stop_bits = (uint8_t)number;

    ret = linkg_json_get_string(node, "parity", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (strcmp(value, "n") != 0 && strcmp(value, "e") != 0 && strcmp(value, "o") != 0)
    {
        return CONFIG_ERR_VALIDATE;
    }

    temp.parity = value[0];

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 从JSON读取单条串口网络路由。
 */
static int _uart_route_config_parse(const cJSON *node, linkg_uart_route_config_t *out)
{
    linkg_uart_route_config_t temp = {0};
    char                      value[LINKG_UART_CONFIG_ENUM_STRING_MAX];
    char                      ip[INET_ADDRSTRLEN];
    bool                      found;
    int                       ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    const char *const fields[] = {"proto", "endpoint", "dir", "ip", "port"};

    if (!cJSON_IsObject(node) || !_uart_fields_valid(node, fields, sizeof(fields) / sizeof(fields[0])))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_string(node, "proto", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _uart_proto_from_string(value, &temp.proto);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_get_string(node, "endpoint", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _uart_endpoint_from_string(value, &temp.endpoint);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_get_string(node, "dir", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _uart_dir_from_string(value, &temp.dir);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_json_get_uint16(node, "port", &temp.port);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    found = false;

    ret = linkg_json_try_get_string(node, "ip", ip, sizeof(ip), &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (found)
    {
        if (inet_pton(AF_INET, ip, &temp.ip) != 1)
        {
            return CONFIG_ERR_VALIDATE;
        }
    }
    else
    {
        return CONFIG_ERR_PARSE;
    }

    ret = _uart_route_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 从JSON读取一个逻辑串口及其路由列表。
 */
static int _uart_item_config_parse(const cJSON *node, linkg_uart_item_config_t *out)
{
    linkg_uart_item_config_t temp = {0};
    const cJSON             *port;
    const cJSON             *routes;
    const cJSON             *route;
    uint32_t                 uart_num;
    uint32_t                 index;
    int                      count;
    bool                     found;
    int                      ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    const char *const fields[] = {"uart_num", "port_config", "routes"};

    if (!cJSON_IsObject(node) || !_uart_fields_valid(node, fields, sizeof(fields) / sizeof(fields[0])))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_uint32(node, "uart_num", &uart_num);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (uart_num >= LINKG_UART_CONFIG_PORT_COUNT_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    temp.uart_num = (uint8_t)uart_num;

    port = NULL;

    ret = linkg_json_get_object(node, "port_config", &port);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _uart_port_config_parse(port, &temp.port_config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _uart_port_config_validate(temp.uart_num, &temp.port_config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    routes = NULL;
    found  = false;

    ret = linkg_json_try_get_array(node, "routes", &routes, &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (!found)
    {
        return CONFIG_ERR_PARSE;
    }

    count = cJSON_GetArraySize(routes);
    if (count < 0 || (uint32_t)count > LINKG_UART_CONFIG_ROUTE_COUNT_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    for (index = 0U; index < (uint32_t)count; index++)
    {
        route = cJSON_GetArrayItem(routes, (int)index);
        ret   = _uart_route_config_parse(route, &temp.routes[index]);
        if (ret != CONFIG_OK)
        {
            return ret;
        }

        temp.route_count++;
    }

    *out = temp;

    return CONFIG_OK;
}

/****************************** JSON序列化 ******************************/

/**
 * @brief 将串口物理配置写入JSON对象。
 */
static int _uart_port_config_to_json(cJSON *parent, const linkg_uart_port_config_t *config)
{
    const char *type;
    cJSON      *object;
    char        parity[2];
    int         ret;

    if (parent == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    type = _uart_type_to_string(config->type);
    if (type == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    parity[0]   = config->parity;
    parity[1]   = '\0';

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "type", type);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint32(object, "baudrate", config->baudrate);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(object, "data_bits", config->data_bits);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(object, "stop_bits", config->stop_bits);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "parity", parity);
    }

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = linkg_json_add_object(parent, "port_config", object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/**
 * @brief 将一条串口路由写入JSON数组。
 */
static int _uart_route_config_to_json(cJSON *array, const linkg_uart_route_config_t *config)
{
    const char *proto;
    const char *endpoint;
    const char *dir;
    cJSON      *object;
    char        ip[INET_ADDRSTRLEN];
    int         ret;

    if (array == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    proto    = _uart_proto_to_string(config->proto);
    endpoint = _uart_endpoint_to_string(config->endpoint);
    dir      = _uart_dir_to_string(config->dir);

    if (proto == NULL || endpoint == NULL || dir == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (inet_ntop(AF_INET, &config->ip, ip, sizeof(ip)) == NULL)
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "proto", proto);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "endpoint", endpoint);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "dir", dir);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "ip", ip);
    }

    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_uint16(object, "port", config->port);
    }

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    if (!cJSON_AddItemToArray(array, object))
    {
        cJSON_Delete(object);
        return CONFIG_ERR_MEMORY;
    }

    return CONFIG_OK;
}

/**
 * @brief 将一个逻辑串口及其路由列表写入JSON数组。
 */
static int _uart_item_config_to_json(cJSON *array, const linkg_uart_item_config_t *config)
{
    cJSON    *object;
    cJSON    *routes;
    uint32_t  index;
    int       ret;

    if (array == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_uint32(object, "uart_num", config->uart_num);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = _uart_port_config_to_json(object, &config->port_config);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    routes = cJSON_CreateArray();
    if (routes == NULL)
    {
        cJSON_Delete(object);
        return CONFIG_ERR_MEMORY;
    }

    for (index = 0U; index < config->route_count; index++)
    {
        ret = _uart_route_config_to_json(routes, &config->routes[index]);
        if (ret != CONFIG_OK)
        {
            cJSON_Delete(routes);
            cJSON_Delete(object);
            return ret;
        }
    }

    ret = linkg_json_add_array(object, "routes", routes);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(routes);
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    if (!cJSON_AddItemToArray(array, object))
    {
        cJSON_Delete(object);
        return CONFIG_ERR_MEMORY;
    }

    return CONFIG_OK;
}

/****************************** 配置处理 ******************************/

/**
 * @brief 设置空串口配置作为默认值。
 */
void linkg_uart_config_set_default(linkg_uart_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
}

/**
 * @brief 解析串口配置JSON数组，失败时不修改原配置。
 */
int linkg_uart_config_parse(const cJSON *node, linkg_uart_config_t *out)
{
    linkg_uart_config_t  temp;
    const cJSON         *item;
    uint32_t             index;
    int                  count;
    int                  ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsArray(node))
    {
        return CONFIG_ERR_PARSE;
    }

    linkg_uart_config_set_default(&temp);

    count = cJSON_GetArraySize(node);
    if (count < 0 || (uint32_t)count > LINKG_UART_CONFIG_PORT_COUNT_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    for (index = 0U; index < (uint32_t)count; index++)
    {
        item = cJSON_GetArrayItem(node, (int)index);

        ret = _uart_item_config_parse(item, &temp.items[index]);
        if (ret != CONFIG_OK)
        {
            return ret;
        }

        temp.count++;
    }

    ret = linkg_uart_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 校验全部逻辑串口配置与路由。
 */
int linkg_uart_config_validate(const linkg_uart_config_t *config)
{
    const linkg_uart_item_config_t *item;
    uint32_t                        index;
    uint32_t                        other;
    uint32_t                        route;
    int                             ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->count > LINKG_UART_CONFIG_PORT_COUNT_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    for (index = 0U; index < config->count; index++)
    {
        item = &config->items[index];

        ret = _uart_port_config_validate(item->uart_num, &item->port_config);
        if (ret != CONFIG_OK)
        {
            return ret;
        }

        if (item->route_count > LINKG_UART_CONFIG_ROUTE_COUNT_MAX)
        {
            return CONFIG_ERR_VALIDATE;
        }

        for (other = index + 1U; other < config->count; other++)
        {
            if (item->uart_num == config->items[other].uart_num)
            {
                return CONFIG_ERR_VALIDATE;
            }
        }

        for (route = 0U; route < item->route_count; route++)
        {
            ret = _uart_route_config_validate(&item->routes[route]);
            if (ret != CONFIG_OK)
            {
                return ret;
            }
        }
    }

    for (index = 0U; index < config->count; index++)
    {
        for (route = 0U; route < config->items[index].route_count; route++)
        {
            const linkg_uart_route_config_t *left = &config->items[index].routes[route];
            uint32_t                         next;

            if (left->endpoint != LINKG_UART_ROUTE_ENDPOINT_LOCAL)
            {
                continue;
            }

            for (other = index; other < config->count; other++)
            {
                for (next = other == index ? route + 1U : 0U; next < config->items[other].route_count; next++)
                {
                    const linkg_uart_route_config_t *right = &config->items[other].routes[next];

                    if (right->endpoint == LINKG_UART_ROUTE_ENDPOINT_LOCAL &&
                        left->proto == right->proto && left->port == right->port &&
                        (left->ip.s_addr == INADDR_ANY || right->ip.s_addr == INADDR_ANY || left->ip.s_addr == right->ip.s_addr))
                    {
                        return CONFIG_ERR_VALIDATE;
                    }
                }
            }
        }
    }

    return CONFIG_OK;
}

/**
 * @brief 将串口配置写入指定JSON父对象。
 */
int linkg_uart_config_to_json(cJSON *parent, const char *key, const linkg_uart_config_t *config)
{
    cJSON    *array;
    uint32_t  index;
    int       ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_uart_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    array = cJSON_CreateArray();
    if (array == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    for (index = 0U; index < config->count; index++)
    {
        ret = _uart_item_config_to_json(array, &config->items[index]);
        if (ret != CONFIG_OK)
        {
            cJSON_Delete(array);
            return ret;
        }
    }

    ret = linkg_json_add_array(parent, key, array);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(array);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/**
 * @brief 按字段比较配置，忽略结构体填充和UART数组顺序，保留规则顺序语义。
 */
bool linkg_uart_config_equal(const linkg_uart_config_t *left, const linkg_uart_config_t *right)
{
    uint32_t i;
    uint32_t j;
    uint32_t route;

    if (left == NULL || right == NULL || left->count != right->count)
    {
        return false;
    }

    for (i = 0U; i < left->count; i++)
    {
        const linkg_uart_item_config_t *a = &left->items[i];
        const linkg_uart_item_config_t *b = NULL;

        for (j = 0U; j < right->count; j++)
        {
            if (right->items[j].uart_num == a->uart_num)
            {
                b = &right->items[j];
                break;
            }
        }

        if (b == NULL || a->route_count != b->route_count ||
            a->port_config.type != b->port_config.type || a->port_config.baudrate != b->port_config.baudrate ||
            a->port_config.data_bits != b->port_config.data_bits || a->port_config.stop_bits != b->port_config.stop_bits ||
            a->port_config.parity != b->port_config.parity)
        {
            return false;
        }

        for (route = 0U; route < a->route_count; route++)
        {
            const linkg_uart_route_config_t *x = &a->routes[route];
            const linkg_uart_route_config_t *y = &b->routes[route];

            if (x->proto != y->proto || x->endpoint != y->endpoint || x->dir != y->dir ||
                x->ip.s_addr != y->ip.s_addr || x->port != y->port)
            {
                return false;
            }
        }
    }

    return true;
}
