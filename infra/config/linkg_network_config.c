/**
 * @file linkg_network_config.c
 * @brief LinkG 网络配置处理接口实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-07-23
 */

#include "linkg_network_config.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"

/****************************** 枚举转换 ******************************/

/**
 * @brief 将端口协议字符串转换为枚举。
 */
static int _network_port_protocol_from_string(const char *value, linkg_network_port_protocol_t *protocol)
{
    if (value == NULL || protocol == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(value, "tcp") == 0)
    {
        *protocol = LINKG_NETWORK_PORT_PROTOCOL_TCP;
        return CONFIG_OK;
    }

    if (strcmp(value, "udp") == 0)
    {
        *protocol = LINKG_NETWORK_PORT_PROTOCOL_UDP;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将端口协议枚举转换为字符串。
 */
static const char *_network_port_protocol_to_string(linkg_network_port_protocol_t protocol)
{
    switch (protocol)
    {
        case LINKG_NETWORK_PORT_PROTOCOL_TCP:
            return "tcp";

        case LINKG_NETWORK_PORT_PROTOCOL_UDP:
            return "udp";

        default:
            return NULL;
    }
}

/**
 * @brief 将业务类型字符串转换为枚举。
 */
static int _network_traffic_class_from_string(const char *value, linkg_network_traffic_class_t *traffic_class)
{
    if (value == NULL || traffic_class == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (strcmp(value, "realtime") == 0)
    {
        *traffic_class = LINKG_NETWORK_TRAFFIC_CLASS_REALTIME;
        return CONFIG_OK;
    }

    if (strcmp(value, "video") == 0)
    {
        *traffic_class = LINKG_NETWORK_TRAFFIC_CLASS_VIDEO;
        return CONFIG_OK;
    }

    return CONFIG_ERR_VALIDATE;
}

/**
 * @brief 将业务类型枚举转换为字符串。
 */
static const char *_network_traffic_class_to_string(linkg_network_traffic_class_t traffic_class)
{
    switch (traffic_class)
    {
        case LINKG_NETWORK_TRAFFIC_CLASS_REALTIME:
            return "realtime";

        case LINKG_NETWORK_TRAFFIC_CLASS_VIDEO:
            return "video";

        default:
            return NULL;
    }
}

/****************************** 配置校验 ******************************/

/**
 * @brief 校验节点编号。
 */
static int _network_node_id_validate(uint8_t node_id)
{
    if (node_id < LINKG_RESOURCE_NODE_ID_MIN || node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验IPv4网络配置。
 */
static int _network_ipv4_config_validate(const linkg_network_ipv4_config_t *config)
{
    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!linkg_network_ipv4_address_valid(&config->ip))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_network_ipv4_netmask_valid(&config->netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 获取系统固定IPv4网段配置。
 */
static int _network_fixed_ipv4_config_get(const char *network, uint8_t prefix, linkg_network_ipv4_config_t *out)
{
    if (network == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    memset(out, 0, sizeof(*out));

    if (!linkg_network_ipv4_from_string(network, &out->ip))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_network_ipv4_netmask_from_prefix(prefix, &out->netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 校验以太网网段是否与LinkG内部网段冲突。
 */
static int _network_ethernet_subnet_validate(const linkg_network_ipv4_config_t *ethernet)
{
    linkg_network_ipv4_config_t tun;
    linkg_network_ipv4_config_t wifi;
    int                         ret;

    if (ethernet == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_fixed_ipv4_config_get(
        LINKG_RESOURCE_TUN_IPV4_NETWORK,
        LINKG_RESOURCE_TUN_IPV4_PREFIX,
        &tun);

    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_fixed_ipv4_config_get(
        LINKG_RESOURCE_WIFI_IPV4_NETWORK,
        LINKG_RESOURCE_WIFI_IPV4_PREFIX,
        &wifi);

    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (linkg_network_ipv4_subnet_overlap(
            &ethernet->ip,
            &ethernet->netmask,
            &tun.ip,
            &tun.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (linkg_network_ipv4_subnet_overlap(
            &ethernet->ip,
            &ethernet->netmask,
            &wifi.ip,
            &wifi.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 判断两个端口范围是否存在重叠。
 */
static bool _network_port_range_overlap(uint16_t left_start, uint16_t left_end, uint16_t right_start, uint16_t right_end)
{
    if (left_end < right_start || right_end < left_start)
    {
        return false;
    }

    return true;
}

/**
 * @brief 校验用户业务流量分类配置。
 */
static int _network_traffic_config_validate(const linkg_network_traffic_config_t *config)
{
    const linkg_network_traffic_rule_t *left;
    const linkg_network_traffic_rule_t *right;
    uint32_t                            left_index;
    uint32_t                            right_index;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (config->count > LINKG_NETWORK_TRAFFIC_RULE_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    for (left_index = 0U; left_index < config->count; left_index++)
    {
        left = &config->rules[left_index];

        if (left->traffic_class != LINKG_NETWORK_TRAFFIC_CLASS_REALTIME &&
            left->traffic_class != LINKG_NETWORK_TRAFFIC_CLASS_VIDEO)
        {
            return CONFIG_ERR_VALIDATE;
        }

        if (left->protocol != LINKG_NETWORK_PORT_PROTOCOL_TCP &&
            left->protocol != LINKG_NETWORK_PORT_PROTOCOL_UDP)
        {
            return CONFIG_ERR_VALIDATE;
        }

        if (left->start_port == 0U || left->end_port == 0U)
        {
            return CONFIG_ERR_VALIDATE;
        }

        if (left->start_port > left->end_port)
        {
            return CONFIG_ERR_VALIDATE;
        }

        /**
         * 同一种协议的端口规则禁止重叠。
         *
         * 即使两个规则属于相同traffic class，也没有必要重复定义；
         * 如果属于不同traffic class，则会造成业务分类歧义。
         */
        for (right_index = left_index + 1U; right_index < config->count; right_index++)
        {
            right = &config->rules[right_index];

            if (left->protocol != right->protocol)
            {
                continue;
            }

            if (_network_port_range_overlap(
                    left->start_port,
                    left->end_port,
                    right->start_port,
                    right->end_port))
            {
                return CONFIG_ERR_VALIDATE;
            }
        }
    }

    return CONFIG_OK;
}

/****************************** 子配置解析 ******************************/

/**
 * @brief 解析IPv4网络配置。
 */
static int _network_ipv4_config_parse(const cJSON *node, linkg_network_ipv4_config_t *out)
{
    linkg_network_ipv4_config_t temp = {0};
    char                        value[INET_ADDRSTRLEN];
    int                         ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    ret = linkg_json_get_string(node, "ip", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (!linkg_network_ipv4_from_string(value, &temp.ip))
    {
        return CONFIG_ERR_VALIDATE;
    }

    ret = linkg_json_get_string(node, "netmask", value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (!linkg_network_ipv4_from_string(value, &temp.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    ret = _network_ipv4_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 解析用户业务流量分类配置。
 */
static int _network_traffic_config_parse(const cJSON *node, linkg_network_traffic_config_t *out)
{
    linkg_network_traffic_rule_t *rule;
    const cJSON                  *array;
    const cJSON                  *item;
    uint32_t                      count;
    uint32_t                      index;
    char                          traffic_class[16];
    char                          protocol[8];
    bool                          found;
    int                           ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    memset(out, 0, sizeof(*out));

    array = NULL;
    found = false;

    ret = linkg_json_try_get_array(node, "traffic_rules", &array, &found);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (!found)
    {
        return CONFIG_OK;
    }

    count = (uint32_t)cJSON_GetArraySize(array);
    if (count > LINKG_NETWORK_TRAFFIC_RULE_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    for (index = 0U; index < count; index++)
    {
        item = cJSON_GetArrayItem(array, (int)index);
        if (item == NULL || !cJSON_IsObject(item))
        {
            return CONFIG_ERR_PARSE;
        }

        rule = &out->rules[index];

        ret = linkg_json_get_string(
            item,
            "class",
            traffic_class,
            sizeof(traffic_class));

        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        ret = _network_traffic_class_from_string(
            traffic_class,
            &rule->traffic_class);

        if (ret != CONFIG_OK)
        {
            return ret;
        }

        ret = linkg_json_get_string(
            item,
            "protocol",
            protocol,
            sizeof(protocol));

        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        ret = _network_port_protocol_from_string(
            protocol,
            &rule->protocol);

        if (ret != CONFIG_OK)
        {
            return ret;
        }

        ret = linkg_json_get_uint16(
            item,
            "start",
            &rule->start_port);

        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        ret = linkg_json_get_uint16(
            item,
            "end",
            &rule->end_port);

        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        out->count++;
    }

    return _network_traffic_config_validate(out);
}

/****************************** JSON序列化 ******************************/

/**
 * @brief 将IPv4网络配置转换为JSON对象。
 */
static int _network_ipv4_config_to_json(cJSON *parent, const char *key, const linkg_network_ipv4_config_t *config)
{
    char   ip[INET_ADDRSTRLEN];
    char   netmask[INET_ADDRSTRLEN];
    cJSON *object;
    int    ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_ipv4_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (!linkg_network_ipv4_to_string(&config->ip, ip, sizeof(ip)) ||
        !linkg_network_ipv4_to_string(&config->netmask, netmask, sizeof(netmask)))
    {
        return CONFIG_ERR_VALIDATE;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_string(object, "ip", ip);
    if (ret == LINKG_JSON_OK)
    {
        ret = linkg_json_add_string(object, "netmask", netmask);
    }

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/**
 * @brief 将用户业务流量分类配置转换为JSON数组。
 */
static int _network_traffic_config_to_json(cJSON *parent, const linkg_network_traffic_config_t *config)
{
    const linkg_network_traffic_rule_t *rule;
    const char                         *traffic_class;
    const char                         *protocol;
    cJSON                              *array;
    cJSON                              *object;
    uint32_t                            index;
    int                                 ret;

    if (parent == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_traffic_config_validate(config);
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
        rule = &config->rules[index];

        traffic_class = _network_traffic_class_to_string(rule->traffic_class);
        if (traffic_class == NULL)
        {
            cJSON_Delete(array);
            return CONFIG_ERR_VALIDATE;
        }

        protocol = _network_port_protocol_to_string(rule->protocol);
        if (protocol == NULL)
        {
            cJSON_Delete(array);
            return CONFIG_ERR_VALIDATE;
        }

        object = cJSON_CreateObject();
        if (object == NULL)
        {
            cJSON_Delete(array);
            return CONFIG_ERR_MEMORY;
        }

        ret = linkg_json_add_string(
            object,
            "class",
            traffic_class);

        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_string(
                object,
                "protocol",
                protocol);
        }

        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_uint16(
                object,
                "start",
                rule->start_port);
        }

        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_uint16(
                object,
                "end",
                rule->end_port);
        }

        if (ret != LINKG_JSON_OK)
        {
            cJSON_Delete(object);
            cJSON_Delete(array);
            return config_json_write_error(ret);
        }

        if (!cJSON_AddItemToArray(array, object))
        {
            cJSON_Delete(object);
            cJSON_Delete(array);
            return CONFIG_ERR_MEMORY;
        }
    }

    ret = linkg_json_add_array(
        parent,
        "traffic_rules",
        array);

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(array);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/****************************** 配置处理 ******************************/

/**
 * @brief 设置网络配置默认值。
 */
void linkg_network_config_set_default(linkg_network_config_t *out)
{
    if (out == NULL)
    {
        return;
    }

    memset(out, 0, sizeof(*out));
}

/**
 * @brief 校验网络配置。
 */
int linkg_network_config_validate(const linkg_network_config_t *config)
{
    int ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_node_id_validate(config->node_id);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_ipv4_config_validate(&config->ethernet);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_ethernet_subnet_validate(&config->ethernet);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    return _network_traffic_config_validate(&config->traffic);
}

/**
 * @brief 解析网络配置。
 */
int linkg_network_config_parse(const cJSON *node, linkg_network_config_t *out)
{
    linkg_network_config_t temp;
    const cJSON           *child;
    uint32_t               node_id;
    int                    ret;

    if (node == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!cJSON_IsObject(node))
    {
        return CONFIG_ERR_PARSE;
    }

    linkg_network_config_set_default(&temp);

    ret = linkg_json_get_uint32(
        node,
        "node_id",
        &node_id);

    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    temp.node_id = (uint8_t)node_id;

    child = NULL;

    ret = linkg_json_get_object(
        node,
        "ethernet",
        &child);

    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    ret = _network_ipv4_config_parse(
        child,
        &temp.ethernet);

    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_traffic_config_parse(
        node,
        &temp.traffic);

    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_network_config_validate(&temp);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    *out = temp;

    return CONFIG_OK;
}

/**
 * @brief 将网络配置转换为JSON对象。
 */
int linkg_network_config_to_json(cJSON *parent, const char *key, const linkg_network_config_t *config)
{
    cJSON *object;
    int    ret;

    if (parent == NULL || key == NULL || config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_network_config_validate(config);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    object = cJSON_CreateObject();
    if (object == NULL)
    {
        return CONFIG_ERR_MEMORY;
    }

    ret = linkg_json_add_uint32(
        object,
        "node_id",
        config->node_id);

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = _network_ipv4_config_to_json(
        object,
        "ethernet",
        &config->ethernet);

    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = _network_traffic_config_to_json(
        object,
        &config->traffic);

    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_json_add_object(
        parent,
        key,
        object);

    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/**
 * @brief 获取TUN虚拟网络配置。
 */
int linkg_network_config_get_tun(const linkg_network_config_t *config, linkg_network_ipv4_config_t *tun)
{
    uint32_t address;
    int      ret;

    if (config == NULL || tun == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_node_id_validate(config->node_id);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_fixed_ipv4_config_get(
        LINKG_RESOURCE_TUN_IPV4_NETWORK,
        LINKG_RESOURCE_TUN_IPV4_PREFIX,
        tun);

    if (ret != CONFIG_OK)
    {
        return ret;
    }

    address = ntohl(tun->ip.s_addr);
    address = (address & ntohl(tun->netmask.s_addr)) |
              (uint32_t)config->node_id;

    tun->ip.s_addr = htonl(address);

    return CONFIG_OK;
}
