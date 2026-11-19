/**
 * @file linkg_fast_nat_config.c
 * @brief LinkG Fast NAT配置校验与运行模式识别
 * @author Dawn
 * @version 1.0.0
 * @date 2026-11-18
 */

#include "linkg_fast_nat_internal.h"

#include <linux/errno.h>
#include <linux/in.h>
#include <linux/types.h>

#include "linkg_fast_nat_utils.h"

/****************************** 配置辅助 ******************************/

/**
 * @brief 校验Fast NAT IPv4子网配置。
 */
static bool _linkg_fast_nat_config_subnet_valid(const linkg_fast_nat_ipv4_subnet_t *subnet)
{
    if (subnet == NULL)
    {
        return false;
    }

    if (!linkg_fast_nat_ipv4_netmask_valid(subnet->netmask))
    {
        return false;
    }

    return linkg_fast_nat_ipv4_subnet_normalized(subnet);
}

/**
 * @brief 判断IPv4地址是否为本节点可映射的Virtual Endpoint地址。
 */
bool linkg_fast_nat_local_virtual_endpoint(const linkg_fast_nat_config_t *config, __be32 address)
{
    __u32 host_mask;
    __u32 host;

    if (config == NULL ||
        !linkg_fast_nat_ipv4_host_usable(address, &config->local_virtual_subnet))
    {
        return false;
    }

    host_mask = ~ntohl(config->local_virtual_subnet.netmask);
    host      = ntohl(address) & host_mask;

    return host != 1U;
}

/****************************** 配置校验 ******************************/

/**
 * @brief 校验用户态下发的Fast NAT配置，并确定NAT或Direct运行模式。
 */
int linkg_fast_nat_config_validate(const linkg_fast_nat_config_t *config, linkg_fast_nat_mode_t *mode)
{
    bool direct_mode;

    if (config == NULL || mode == NULL)
    {
        return -EINVAL;
    }

    if (config->version != LINKG_FAST_NAT_UAPI_VERSION || config->struct_size != sizeof(*config))
    {
        return -EPROTO;
    }

    if (config->direct_snat_disabled > 1U)
    {
        return -EINVAL;
    }

    if (!_linkg_fast_nat_config_subnet_valid(&config->ethernet_network) ||
        !_linkg_fast_nat_config_subnet_valid(&config->tun_network) ||
        !_linkg_fast_nat_config_subnet_valid(&config->virtual_network) ||
        !_linkg_fast_nat_config_subnet_valid(&config->local_virtual_subnet))
    {
        return -EINVAL;
    }

    // Ethernet与本节点Virtual Endpoint保留相同Host部分，要求子网掩码一致。
    if (config->ethernet_network.netmask != config->local_virtual_subnet.netmask)
    {
        return -EINVAL;
    }

    if (!linkg_fast_nat_ipv4_subnet_contains(&config->virtual_network, &config->local_virtual_subnet))
    {
        return -EINVAL;
    }

    direct_mode = linkg_fast_nat_ipv4_subnet_equal(&config->ethernet_network, &config->local_virtual_subnet);

    // 仅允许Ethernet与本节点Virtual Endpoint子网完全一致，其余重叠继续拒绝。
    if ((linkg_fast_nat_ipv4_subnet_overlap(&config->ethernet_network, &config->virtual_network) && !direct_mode) ||
        linkg_fast_nat_ipv4_subnet_overlap(&config->ethernet_network, &config->tun_network) ||
        linkg_fast_nat_ipv4_subnet_overlap(&config->virtual_network, &config->tun_network))
    {
        return -EINVAL;
    }

    if (!linkg_fast_nat_ipv4_host_usable(config->ethernet_ip, &config->ethernet_network))
    {
        return -EINVAL;
    }

    if (config->ethernet_ifindex <= 0 || config->tun_ifindex <= 0 || config->uplink_ifindex < 0)
    {
        return -EINVAL;
    }

    if (config->ethernet_ifindex == config->tun_ifindex)
    {
        return -EINVAL;
    }

    if (config->uplink_ifindex > 0 &&
        (config->uplink_ifindex == config->ethernet_ifindex ||
         config->uplink_ifindex == config->tun_ifindex))
    {
        return -EINVAL;
    }

    if (config->snat_port_start == 0U)
    {
        return -EINVAL;
    }

    if ((__u32)config->snat_port_start + LINKG_FAST_NAT_SLOT_COUNT - 1U > 65535U)
    {
        return -EINVAL;
    }

    *mode = direct_mode ? LINKG_FAST_NAT_MODE_DIRECT : LINKG_FAST_NAT_MODE_NAT;

    return 0;
}
