/**
 * @file linkg_network_config.c
 * @brief LinkG网络配置处理接口实现
 * @author Dawn
 * @version 1.3.0
 * @date 2026-08-28
 */

#include "linkg_network_config.h"

#include <arpa/inet.h>
#include <stdbool.h>
#include <string.h>

#include "config_internal.h"
#include "linkg_json.h"
#include "linkg_network_ops.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_NETWORK_VIRTUAL_IPV4_PREFIX        16U // LinkG虚拟聚合网络固定前缀
#define LINKG_NETWORK_VIRTUAL_SUBNET_IPV4_PREFIX 24U // 单节点虚拟Endpoint子网固定前缀
#define LINKG_NETWORK_ETHERNET_IPV4_PREFIX       24U // Ethernet网络固定前缀
#define LINKG_NETWORK_VIRTUAL_NODE_SHIFT         8U  // 虚拟地址中Node ID位移
#define LINKG_NETWORK_ETHERNET_GATEWAY_HOST_ID   1U  // LinkG Ethernet固定主机编号

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

/****************************** 地址辅助 ******************************/

/**
 * @brief 判断IPv4地址是否属于RFC1918私有地址空间。
 */
static bool _network_ipv4_private(const struct in_addr *address)
{
    uint32_t value;

    if (address == NULL)
    {
        return false;
    }

    value = ntohl(address->s_addr);

    if ((value & 0xFF000000U) == 0x0A000000U)
    {
        return true;
    }

    if ((value & 0xFFF00000U) == 0xAC100000U)
    {
        return true;
    }

    if ((value & 0xFFFF0000U) == 0xC0A80000U)
    {
        return true;
    }

    return false;
}

/**
 * @brief 校验IPv4网络基地址及固定前缀。
 */
static int _network_ipv4_network_validate(const struct in_addr *network, uint8_t prefix)
{
    struct in_addr network_address;
    struct in_addr netmask;

    if (network == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    if (!linkg_network_ipv4_address_valid(network))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!_network_ipv4_private(network))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_network_ipv4_netmask_from_prefix(prefix, &netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (!linkg_network_ipv4_network_address(network, &netmask, &network_address))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (network_address.s_addr != network->s_addr)
    {
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}

/**
 * @brief 根据网络基地址和固定前缀生成IPv4网络配置。
 */
static int _network_ipv4_config_from_network(const struct in_addr *network, uint8_t prefix, linkg_network_ipv4_config_t *out)
{
    int ret;

    if (network == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_ipv4_network_validate(network, prefix);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    memset(out, 0, sizeof(*out));

    out->ip = *network;

    if (!linkg_network_ipv4_netmask_from_prefix(prefix, &out->netmask))
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
 * @brief 校验虚拟网络基地址。
 */
static int _network_virtual_network_validate(const struct in_addr *network)
{
    return _network_ipv4_network_validate(network, LINKG_NETWORK_VIRTUAL_IPV4_PREFIX);
}

/**
 * @brief 校验Ethernet网络基地址。
 */
static int _network_ethernet_network_validate(const struct in_addr *network)
{
    return _network_ipv4_network_validate(network, LINKG_NETWORK_ETHERNET_IPV4_PREFIX);
}

/**
 * @brief 校验网络配置与系统固定网络之间的地址冲突。
 */
static int _network_subnet_conflict_validate(const linkg_network_config_t *config)
{
    linkg_network_ipv4_config_t ethernet;
    linkg_network_ipv4_config_t tun;
    linkg_network_ipv4_config_t virtual_network;
    linkg_network_ipv4_config_t wifi;
    int                         ret;

    if (config == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_ipv4_config_from_network(&config->virtual_network, LINKG_NETWORK_VIRTUAL_IPV4_PREFIX, &virtual_network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_ipv4_config_from_network(&config->ethernet_network, LINKG_NETWORK_ETHERNET_IPV4_PREFIX, &ethernet);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_fixed_ipv4_config_get(LINKG_RESOURCE_TUN_IPV4_NETWORK, LINKG_RESOURCE_TUN_IPV4_PREFIX, &tun);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_fixed_ipv4_config_get(LINKG_RESOURCE_WIFI_IPV4_NETWORK, LINKG_RESOURCE_WIFI_IPV4_PREFIX, &wifi);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (linkg_network_ipv4_subnet_overlap(&virtual_network.ip, &virtual_network.netmask, &ethernet.ip, &ethernet.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (linkg_network_ipv4_subnet_overlap(&virtual_network.ip, &virtual_network.netmask, &tun.ip, &tun.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (linkg_network_ipv4_subnet_overlap(&virtual_network.ip, &virtual_network.netmask, &wifi.ip, &wifi.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (linkg_network_ipv4_subnet_overlap(&ethernet.ip, &ethernet.netmask, &tun.ip, &tun.netmask))
    {
        return CONFIG_ERR_VALIDATE;
    }

    if (linkg_network_ipv4_subnet_overlap(&ethernet.ip, &ethernet.netmask, &wifi.ip, &wifi.netmask))
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
         * 相同类型的重复规则没有意义，不同类型的重叠规则会造成业务分类歧义。
         */
        for (right_index = left_index + 1U; right_index < config->count; right_index++)
        {
            right = &config->rules[right_index];

            if (left->protocol != right->protocol)
            {
                continue;
            }

            if (_network_port_range_overlap(left->start_port, left->end_port, right->start_port, right->end_port))
            {
                return CONFIG_ERR_VALIDATE;
            }
        }
    }

    return CONFIG_OK;
}

/****************************** 配置解析 ******************************/

/**
 * @brief 解析IPv4网络基地址配置。
 */
static int _network_ipv4_network_parse(const cJSON *node, const char *key, uint8_t prefix, struct in_addr *out)
{
    char value[INET_ADDRSTRLEN];
    int  ret;

    if (node == NULL || key == NULL || out == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = linkg_json_get_string(node, key, value, sizeof(value));
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (!linkg_network_ipv4_from_string(value, out))
    {
        return CONFIG_ERR_VALIDATE;
    }

    return _network_ipv4_network_validate(out, prefix);
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

        ret = linkg_json_get_string(item, "class", traffic_class, sizeof(traffic_class));
        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        ret = _network_traffic_class_from_string(traffic_class, &rule->traffic_class);
        if (ret != CONFIG_OK)
        {
            return ret;
        }

        ret = linkg_json_get_string(item, "protocol", protocol, sizeof(protocol));
        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        ret = _network_port_protocol_from_string(protocol, &rule->protocol);
        if (ret != CONFIG_OK)
        {
            return ret;
        }

        ret = linkg_json_get_uint16(item, "start", &rule->start_port);
        if (ret != LINKG_JSON_OK)
        {
            return config_json_parse_error(ret);
        }

        ret = linkg_json_get_uint16(item, "end", &rule->end_port);
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
 * @brief 将IPv4网络基地址转换为JSON字符串字段。
 */
static int _network_ipv4_network_to_json(cJSON *parent, const char *key, const struct in_addr *network, uint8_t prefix)
{
    char value[INET_ADDRSTRLEN];
    int  ret;

    if (parent == NULL || key == NULL || network == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_ipv4_network_validate(network, prefix);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    if (!linkg_network_ipv4_to_string(network, value, sizeof(value)))
    {
        return CONFIG_ERR_VALIDATE;
    }

    ret = linkg_json_add_string(parent, key, value);
    if (ret != LINKG_JSON_OK)
    {
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

        ret = linkg_json_add_string(object, "class", traffic_class);
        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_string(object, "protocol", protocol);
        }

        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_uint16(object, "start", rule->start_port);
        }

        if (ret == LINKG_JSON_OK)
        {
            ret = linkg_json_add_uint16(object, "end", rule->end_port);
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

    ret = linkg_json_add_array(parent, "traffic_rules", array);
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

    ret = _network_virtual_network_validate(&config->virtual_network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_ethernet_network_validate(&config->ethernet_network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_subnet_conflict_validate(config);
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

    ret = linkg_json_get_uint32(node, "node_id", &node_id);
    if (ret != LINKG_JSON_OK)
    {
        return config_json_parse_error(ret);
    }

    if (node_id < LINKG_RESOURCE_NODE_ID_MIN || node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return CONFIG_ERR_VALIDATE;
    }

    temp.node_id = (uint8_t)node_id;

    ret = _network_ipv4_network_parse(node, "virtual_network", LINKG_NETWORK_VIRTUAL_IPV4_PREFIX, &temp.virtual_network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_ipv4_network_parse(node, "ethernet_network", LINKG_NETWORK_ETHERNET_IPV4_PREFIX, &temp.ethernet_network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = _network_traffic_config_parse(node, &temp.traffic);
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

    ret = linkg_json_add_uint32(object, "node_id", config->node_id);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    ret = _network_ipv4_network_to_json(object, "virtual_network", &config->virtual_network, LINKG_NETWORK_VIRTUAL_IPV4_PREFIX);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = _network_ipv4_network_to_json(object, "ethernet_network", &config->ethernet_network, LINKG_NETWORK_ETHERNET_IPV4_PREFIX);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = _network_traffic_config_to_json(object, &config->traffic);
    if (ret != CONFIG_OK)
    {
        cJSON_Delete(object);
        return ret;
    }

    ret = linkg_json_add_object(parent, key, object);
    if (ret != LINKG_JSON_OK)
    {
        cJSON_Delete(object);
        return config_json_write_error(ret);
    }

    return CONFIG_OK;
}

/****************************** 地址查询 ******************************/

/**
 * @brief 获取Ethernet接口IPv4配置。
 */
int linkg_network_config_get_ethernet(const linkg_network_config_t *config, linkg_network_ipv4_config_t *ethernet)
{
    uint32_t address;
    int      ret;

    if (config == NULL || ethernet == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_ipv4_config_from_network(&config->ethernet_network, LINKG_NETWORK_ETHERNET_IPV4_PREFIX, ethernet);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    address = ntohl(ethernet->ip.s_addr);
    address |= LINKG_NETWORK_ETHERNET_GATEWAY_HOST_ID;

    ethernet->ip.s_addr = htonl(address);

    return CONFIG_OK;
}

/**
 * @brief 获取Ethernet网络配置。
 */
int linkg_network_config_get_ethernet_network(const linkg_network_config_t *config, linkg_network_ipv4_config_t *network)
{
    if (config == NULL || network == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    return _network_ipv4_config_from_network(&config->ethernet_network, LINKG_NETWORK_ETHERNET_IPV4_PREFIX, network);
}

/**
 * @brief 获取本节点TUN IPv4配置。
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

    ret = _network_fixed_ipv4_config_get(LINKG_RESOURCE_TUN_IPV4_NETWORK, LINKG_RESOURCE_TUN_IPV4_PREFIX, tun);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    address = ntohl(tun->ip.s_addr);
    address = (address & ntohl(tun->netmask.s_addr)) | (uint32_t)config->node_id;

    tun->ip.s_addr = htonl(address);

    return CONFIG_OK;
}

/**
 * @brief 获取LinkG虚拟聚合网络配置。
 */
int linkg_network_config_get_virtual_network(const linkg_network_config_t *config, linkg_network_ipv4_config_t *network)
{
    if (config == NULL || network == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    return _network_ipv4_config_from_network(&config->virtual_network, LINKG_NETWORK_VIRTUAL_IPV4_PREFIX, network);
}

/**
 * @brief 获取指定节点的虚拟Endpoint子网配置。
 */
int linkg_network_config_get_node_virtual_subnet(const linkg_network_config_t *config, uint8_t node_id, linkg_network_ipv4_config_t *subnet)
{
    linkg_network_ipv4_config_t virtual_network;
    uint32_t                    address;
    int                         ret;

    if (config == NULL || subnet == NULL)
    {
        return CONFIG_ERR_PARAM;
    }

    ret = _network_node_id_validate(node_id);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    ret = linkg_network_config_get_virtual_network(config, &virtual_network);
    if (ret != CONFIG_OK)
    {
        return ret;
    }

    memset(subnet, 0, sizeof(*subnet));

    address = ntohl(virtual_network.ip.s_addr);
    address |= (uint32_t)node_id << LINKG_NETWORK_VIRTUAL_NODE_SHIFT;

    subnet->ip.s_addr = htonl(address);

    if (!linkg_network_ipv4_netmask_from_prefix(LINKG_NETWORK_VIRTUAL_SUBNET_IPV4_PREFIX, &subnet->netmask))
    {
        memset(subnet, 0, sizeof(*subnet));
        return CONFIG_ERR_VALIDATE;
    }

    return CONFIG_OK;
}
