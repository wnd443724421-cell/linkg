/**
 * @file wifi_address.c
 * @brief LinkG Wi-Fi IPv4地址派生实现
 * @author Dawn
 * @version 1.1.0
 * @date 2026-08-26
 */

#include "wifi_address.h"

#include <arpa/inet.h>
#include <errno.h>
#include <string.h>

#include "linkg_network_ops.h"
#include "linkg_system_resources.h"

/****************************** 地址派生 ******************************/

/**
 * @brief 根据LinkG节点编号派生Wi-Fi接口IPv4配置。
 */
int wifi_address_from_node_id(uint8_t node_id, linkg_network_ipv4_config_t *config)
{
    linkg_network_ipv4_config_t derived;
    uint32_t                    network;
    uint32_t                    netmask;
    uint32_t                    host_mask;
    uint32_t                    host_id;

    if (config == NULL)
    {
        return -EINVAL;
    }

    memset(config, 0, sizeof(*config));

    if (node_id < LINKG_RESOURCE_NODE_ID_MIN || node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -ERANGE;
    }

    memset(&derived, 0, sizeof(derived));

    if (!linkg_network_ipv4_from_string(LINKG_RESOURCE_WIFI_IPV4_NETWORK, &derived.ip))
    {
        return -EINVAL;
    }

    if (!linkg_network_ipv4_netmask_from_prefix(LINKG_RESOURCE_WIFI_IPV4_PREFIX, &derived.netmask))
    {
        return -EINVAL;
    }

    network   = ntohl(derived.ip.s_addr);
    netmask   = ntohl(derived.netmask.s_addr);
    host_mask = ~netmask;
    host_id   = (uint32_t)node_id;

    if ((host_id & ~host_mask) != 0U || host_id == 0U || host_id == host_mask)
    {
        return -ERANGE;
    }

    derived.ip.s_addr = htonl((network & netmask) | host_id);

    *config = derived;

    return 0;
}
