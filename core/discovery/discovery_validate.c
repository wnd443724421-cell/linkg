/**
 * @file discovery_validate.c
 * @brief LinkG设备发现状态校验实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-25
 */

#include "discovery_internal.h"

#include <errno.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdint.h>

#include "linkg_network_ops.h"
#include "linkg_system_resources.h"

/****************************** 内部辅助 ******************************/

/**
 * @brief 校验Discovery节点编号是否合法。
 */
static bool _linkg_discovery_node_id_valid(uint8_t node_id)
{
    return node_id >= LINKG_RESOURCE_NODE_ID_MIN &&
           node_id <= LINKG_RESOURCE_NODE_ID_MAX;
}

/**
 * @brief 校验Discovery节点角色是否合法。
 */
static bool _linkg_discovery_role_valid(linkg_device_role_t role)
{
    return role == LINKG_DEVICE_ROLE_AP ||
           role == LINKG_DEVICE_ROLE_STA;
}

/**
 * @brief 校验Wi-Fi数据端点是否合法。
 */
static bool _linkg_discovery_wifi_endpoint_valid(const linkg_path_endpoint_t *endpoint)
{
    const struct sockaddr_in *address;

    if (endpoint == NULL)
    {
        return false;
    }

    if (endpoint->length != sizeof(struct sockaddr_in))
    {
        return false;
    }

    if (endpoint->address.ss_family != AF_INET)
    {
        return false;
    }

    address = (const struct sockaddr_in *)&endpoint->address;

    if (!linkg_network_ipv4_address_valid(&address->sin_addr))
    {
        return false;
    }

    if (address->sin_port == 0U)
    {
        return false;
    }

    return true;
}

/**
 * @brief 校验Cellular数据端点是否合法。
 */
static bool _linkg_discovery_cellular_endpoint_valid(const linkg_path_endpoint_t *endpoint)
{
    const struct sockaddr_in6 *address;

    if (endpoint == NULL)
    {
        return false;
    }

    if (endpoint->length != sizeof(struct sockaddr_in6))
    {
        return false;
    }

    if (endpoint->address.ss_family != AF_INET6)
    {
        return false;
    }

    address = (const struct sockaddr_in6 *)&endpoint->address;

    if (!linkg_network_ipv6_address_is_global(&address->sin6_addr))
    {
        return false;
    }

    if (address->sin6_port == 0U)
    {
        return false;
    }

    return true;
}

/**
 * @brief 校验Discovery Path状态与Endpoint是否一致。
 */
static bool _linkg_discovery_path_state_valid(const linkg_discovery_report_t *report)
{
    bool wifi_valid;
    bool cellular_valid;

    if (report == NULL)
    {
        return false;
    }

    if ((report->path_flags & (uint8_t)~LINKG_DISCOVERY_PATH_VALID_MASK) != 0U)
    {
        return false;
    }

    wifi_valid     = (report->path_flags & LINKG_DISCOVERY_PATH_WIFI_VALID) != 0U;
    cellular_valid = (report->path_flags & LINKG_DISCOVERY_PATH_CELLULAR_VALID) != 0U;

    if (wifi_valid)
    {
        if (!_linkg_discovery_wifi_endpoint_valid(&report->wifi_endpoint))
        {
            return false;
        }
    }
    else if (report->wifi_endpoint.length != 0U)
    {
        return false;
    }

    if (cellular_valid)
    {
        if (!_linkg_discovery_cellular_endpoint_valid(&report->cellular_endpoint))
        {
            return false;
        }
    }
    else if (report->cellular_endpoint.length != 0U)
    {
        return false;
    }

    return true;
}

/****************************** 状态校验 ******************************/

/**
 * @brief 校验Discovery完整设备状态是否合法。
 */
int _linkg_discovery_validate_report(const linkg_discovery_report_t *report)
{
    if (report == NULL)
    {
        return -EINVAL;
    }

    if (report->session_id == 0U)
    {
        return -EINVAL;
    }

    if (report->revision == 0U)
    {
        return -EINVAL;
    }

    if (!_linkg_discovery_node_id_valid(report->node.node_id))
    {
        return -EINVAL;
    }

    if (!_linkg_discovery_role_valid(report->node.role))
    {
        return -EINVAL;
    }

    if (!_linkg_discovery_path_state_valid(report))
    {
        return -EINVAL;
    }

    return 0;
}

/**
 * @brief 校验AP下发的完整拓扑同步状态。
 *
 * 调用方必须持有Discovery状态锁。
 */
int _linkg_discovery_validate_ap_sync_locked(const linkg_discovery_ap_sync_t *sync)
{
    uint8_t  node_id;
    uint32_t index;
    uint32_t other_index;
    int      ret;

    if (sync == NULL)
    {
        return -EINVAL;
    }

    if (g_discovery.local_report.node.role != LINKG_DEVICE_ROLE_STA)
    {
        return -EPERM;
    }

    ret = _linkg_discovery_validate_report(&sync->ap);
    if (ret != 0)
    {
        return ret;
    }

    if (sync->ap.node.role != LINKG_DEVICE_ROLE_AP)
    {
        return -EINVAL;
    }

    if (sync->ap.node.node_id == g_discovery.local_report.node.node_id)
    {
        return -EINVAL;
    }

    if (sync->topology_revision == 0U)
    {
        return -EINVAL;
    }

    if (sync->node_count > LINKG_RESOURCE_NETWORK_STA_MAX)
    {
        return -EINVAL;
    }

    for (index = 0U; index < sync->node_count; index++)
    {
        node_id = sync->node_ids[index];

        if (!_linkg_discovery_node_id_valid(node_id))
        {
            return -EINVAL;
        }

        if (node_id == sync->ap.node.node_id)
        {
            return -EINVAL;
        }

        for (other_index = 0U; other_index < index; other_index++)
        {
            if (sync->node_ids[other_index] == node_id)
            {
                return -EEXIST;
            }
        }
    }

    return 0;
}
