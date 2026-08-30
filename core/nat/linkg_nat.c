/**
 * @file linkg_nat.c
 * @brief LinkG虚拟网络NAT管理实现
 * @author Dawn
 * @version 1.0.0
 * @date 2026-08-30
 */

#include "linkg_nat.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "linkg_network_ops.h"
#include "linkg_os.h"
#include "linkg_system_resources.h"

/****************************** 模块常量 ******************************/

#define LINKG_NAT_IPTABLES_PROGRAM     "iptables"          // iptables执行程序
#define LINKG_NAT_CHAIN_PREROUTING     "LINKG_PREROUTING"  // LinkG NAT入口Chain
#define LINKG_NAT_CHAIN_OUTPUT         "LINKG_OUTPUT"      // LinkG NAT本机出口Chain
#define LINKG_NAT_CHAIN_POSTROUTING    "LINKG_POSTROUTING" // LinkG NAT出口Chain
#define LINKG_NAT_IPV4_ADDRESS_SIZE    16U                 // IPv4地址字符串缓冲区大小
#define LINKG_NAT_IPV4_CIDR_SIZE       20U                 // IPv4 CIDR字符串缓冲区大小

/****************************** 内部类型 ******************************/

typedef struct
{
    linkg_network_ipv4_config_t virtual_network;      // LinkG虚拟聚合网络
    linkg_network_ipv4_config_t local_virtual_subnet; // 本节点虚拟Endpoint子网
    linkg_network_ipv4_config_t ethernet_network;     // 本节点Ethernet网络
    linkg_network_ipv4_config_t ethernet;             // 本节点Ethernet接口IPv4配置
    uint8_t                     node_id;              // 本节点编号
    bool                        initialized;          // 模块是否已经初始化
    bool                        started;              // NAT规则是否已经启用
} linkg_nat_context_t;

/****************************** 全局上下文 ******************************/

static linkg_nat_context_t g_nat =
{
    .node_id     = LINKG_RESOURCE_NODE_ID_INVALID, // 当前无有效节点
    .initialized = false,                          // 模块尚未初始化
    .started     = false                           // NAT规则尚未启用
};

/****************************** 内部辅助 ******************************/

/**
 * @brief 根据网络配置构造NAT地址映射上下文。
 */
static int _linkg_nat_build_context(const linkg_network_config_t *network_config, linkg_nat_context_t *context)
{
    int ret;

    if (network_config == NULL)
    {
        return -EINVAL;
    }

    if (context == NULL)
    {
        return -EINVAL;
    }

    if (network_config->node_id < LINKG_RESOURCE_NODE_ID_MIN ||
        network_config->node_id > LINKG_RESOURCE_NODE_ID_MAX)
    {
        return -EINVAL;
    }

    memset(context, 0, sizeof(*context));

    ret = linkg_network_config_get_virtual_network(network_config, &context->virtual_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_node_virtual_subnet(network_config,
                                                       network_config->node_id,
                                                       &context->local_virtual_subnet);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_ethernet_network(network_config, &context->ethernet_network);
    if (ret != 0)
    {
        return -EINVAL;
    }

    ret = linkg_network_config_get_ethernet(network_config, &context->ethernet);
    if (ret != 0)
    {
        return -EINVAL;
    }

    context->node_id = network_config->node_id;

    return 0;
}

/**
 * @brief 获取IPv4子网掩码对应的前缀长度。
 */
static int _linkg_nat_ipv4_prefix_length(const struct in_addr *netmask, uint8_t *prefix_length)
{
    uint32_t mask;
    uint8_t  prefix;

    if (netmask == NULL)
    {
        return -EINVAL;
    }

    if (prefix_length == NULL)
    {
        return -EINVAL;
    }

    if (!linkg_network_ipv4_netmask_valid(netmask))
    {
        return -EINVAL;
    }

    mask   = ntohl(netmask->s_addr);
    prefix = 0U;

    while ((mask & 0x80000000U) != 0U)
    {
        prefix++;
        mask <<= 1U;
    }

    *prefix_length = prefix;

    return 0;
}

/**
 * @brief 将IPv4网络配置转换为CIDR字符串。
 */
static int _linkg_nat_ipv4_config_to_cidr(const linkg_network_ipv4_config_t *config, char *buffer, size_t buffer_size)
{
    char    address[LINKG_NAT_IPV4_ADDRESS_SIZE];
    uint8_t prefix_length;
    int     length;
    int     ret;

    if (config == NULL)
    {
        return -EINVAL;
    }

    if (buffer == NULL)
    {
        return -EINVAL;
    }

    if (buffer_size == 0U)
    {
        return -EINVAL;
    }

    ret = _linkg_nat_ipv4_prefix_length(&config->netmask, &prefix_length);
    if (ret != 0)
    {
        return ret;
    }

    if (!linkg_network_ipv4_to_string(&config->ip, address, sizeof(address)))
    {
        return -EINVAL;
    }

    length = snprintf(buffer, buffer_size, "%s/%u", address, (unsigned int)prefix_length);
    if (length < 0)
    {
        return -EIO;
    }

    if ((size_t)length >= buffer_size)
    {
        return -ENOBUFS;
    }

    return 0;
}

/****************************** iptables辅助 ******************************/

/**
 * @brief 创建LinkG专用iptables NAT Chain。
 */
static int _linkg_nat_chain_create(const char *chain)
{
    if (chain == NULL)
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-N", chain,
                        NULL);
}

/**
 * @brief 清空LinkG专用iptables NAT Chain。
 */
static int _linkg_nat_chain_flush(const char *chain)
{
    if (chain == NULL)
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-F", chain,
                        NULL);
}

/**
 * @brief 删除LinkG专用iptables NAT Chain。
 */
static int _linkg_nat_chain_delete(const char *chain)
{
    if (chain == NULL)
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-X", chain,
                        NULL);
}

/**
 * @brief 将LinkG专用Chain挂接到iptables内建Chain首部。
 */
static int _linkg_nat_jump_add(const char *parent_chain, const char *target_chain)
{
    if (parent_chain == NULL)
    {
        return -EINVAL;
    }

    if (target_chain == NULL)
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-I", parent_chain, "1",
                        "-j", target_chain,
                        NULL);
}

/**
 * @brief 从iptables内建Chain删除LinkG专用Chain跳转。
 */
static int _linkg_nat_jump_remove(const char *parent_chain, const char *target_chain)
{
    if (parent_chain == NULL)
    {
        return -EINVAL;
    }

    if (target_chain == NULL)
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-D", parent_chain,
                        "-j", target_chain,
                        NULL);
}

/**
 * @brief 清理可能由上次异常退出遗留的LinkG NAT规则。
 *
 * 清理仅操作LinkG专用Chain及对应跳转，不修改系统其他NAT规则。
 * 该接口用于启动前恢复确定状态，因此忽略规则不存在等错误。
 */
static void _linkg_nat_rules_cleanup_stale(void)
{
    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-D", "PREROUTING",
                        "-j", LINKG_NAT_CHAIN_PREROUTING,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-D", "OUTPUT",
                        "-j", LINKG_NAT_CHAIN_OUTPUT,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-D", "POSTROUTING",
                        "-j", LINKG_NAT_CHAIN_POSTROUTING,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-F", LINKG_NAT_CHAIN_PREROUTING,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-F", LINKG_NAT_CHAIN_OUTPUT,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-F", LINKG_NAT_CHAIN_POSTROUTING,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-X", LINKG_NAT_CHAIN_PREROUTING,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-X", LINKG_NAT_CHAIN_OUTPUT,
                        NULL);

    linkg_os_run_ignore(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-X", LINKG_NAT_CHAIN_POSTROUTING,
                        NULL);
}

/****************************** NAT规则 ******************************/

/**
 * @brief 安装Ethernet发送到LinkG虚拟网络的Source NETMAP规则。
 *
 * 将本地Ethernet主机地址映射为本节点虚拟Endpoint地址，
 * 保持IPv4 Host部分不变后通过linkg0发送到远端节点。
 */
static int _linkg_nat_source_netmap_add(void)
{
    char ethernet_network[LINKG_NAT_IPV4_CIDR_SIZE];
    char local_virtual_subnet[LINKG_NAT_IPV4_CIDR_SIZE];
    char virtual_network[LINKG_NAT_IPV4_CIDR_SIZE];
    int  ret;

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.ethernet_network,
                                         ethernet_network,
                                         sizeof(ethernet_network));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.local_virtual_subnet,
                                         local_virtual_subnet,
                                         sizeof(local_virtual_subnet));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.virtual_network,
                                         virtual_network,
                                         sizeof(virtual_network));
    if (ret != 0)
    {
        return ret;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-A", LINKG_NAT_CHAIN_POSTROUTING,
                        "-s", ethernet_network,
                        "-d", virtual_network,
                        "-o", LINKG_RESOURCE_INTERFACE_TUN,
                        "-j", "NETMAP",
                        "--to", local_virtual_subnet,
                        NULL);
}

/**
 * @brief 安装远端LinkG流量访问本节点虚拟Endpoint子网的Destination NETMAP规则。
 *
 * 将所有从linkg0进入且发往本节点虚拟Endpoint子网的流量映射为
 * 本地Ethernet真实主机地址。
 */
static int _linkg_nat_destination_netmap_add(void)
{
    char ethernet_network[LINKG_NAT_IPV4_CIDR_SIZE];
    char local_virtual_subnet[LINKG_NAT_IPV4_CIDR_SIZE];
    int  ret;

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.ethernet_network,
                                         ethernet_network,
                                         sizeof(ethernet_network));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.local_virtual_subnet,
                                         local_virtual_subnet,
                                         sizeof(local_virtual_subnet));
    if (ret != 0)
    {
        return ret;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-A", LINKG_NAT_CHAIN_PREROUTING,
                        "-i", LINKG_RESOURCE_INTERFACE_TUN,
                        "-d", local_virtual_subnet,
                        "-j", "NETMAP",
                        "--to", ethernet_network,
                        NULL);
}

/**
 * @brief 安装本地Ethernet访问本节点虚拟Endpoint子网的Destination NETMAP规则。
 *
 * 本地Ethernet设备访问本节点172.28.<node_id>.X地址时，在进入路由
 * 决策前直接映射为本地Ethernet真实地址，避免流量错误进入linkg0。
 */
static int _linkg_nat_local_ethernet_destination_netmap_add(void)
{
    char ethernet_network[LINKG_NAT_IPV4_CIDR_SIZE];
    char local_virtual_subnet[LINKG_NAT_IPV4_CIDR_SIZE];
    int  ret;

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.ethernet_network,
                                         ethernet_network,
                                         sizeof(ethernet_network));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.local_virtual_subnet,
                                         local_virtual_subnet,
                                         sizeof(local_virtual_subnet));
    if (ret != 0)
    {
        return ret;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-A", LINKG_NAT_CHAIN_PREROUTING,
                        "-i", LINKG_RESOURCE_INTERFACE_ETHERNET,
                        "-d", local_virtual_subnet,
                        "-j", "NETMAP",
                        "--to", ethernet_network,
                        NULL);
}

/**
 * @brief 安装本机访问自身虚拟Endpoint子网的Destination NETMAP规则。
 *
 * 本机进程访问本节点虚拟Endpoint地址时直接映射到本机Ethernet
 * 真实地址，不将本节点流量发送到LinkG Transport。
 */
static int _linkg_nat_local_destination_netmap_add(void)
{
    char ethernet_network[LINKG_NAT_IPV4_CIDR_SIZE];
    char local_virtual_subnet[LINKG_NAT_IPV4_CIDR_SIZE];
    int  ret;

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.ethernet_network,
                                         ethernet_network,
                                         sizeof(ethernet_network));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.local_virtual_subnet,
                                         local_virtual_subnet,
                                         sizeof(local_virtual_subnet));
    if (ret != 0)
    {
        return ret;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-A", LINKG_NAT_CHAIN_OUTPUT,
                        "-d", local_virtual_subnet,
                        "-j", "NETMAP",
                        "--to", ethernet_network,
                        NULL);
}

/**
 * @brief 安装LinkG虚拟网络访问本地Ethernet时的SNAT规则。
 *
 * 将远端虚拟Endpoint源地址转换为本机Ethernet网关地址，
 * 使Ethernet终端可以直接通过默认网关完成回包。
 */
static int _linkg_nat_ethernet_snat_add(void)
{
    char ethernet_address[LINKG_NAT_IPV4_ADDRESS_SIZE];
    char ethernet_network[LINKG_NAT_IPV4_CIDR_SIZE];
    char virtual_network[LINKG_NAT_IPV4_CIDR_SIZE];
    int  ret;

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.ethernet_network,
                                         ethernet_network,
                                         sizeof(ethernet_network));
    if (ret != 0)
    {
        return ret;
    }

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.virtual_network,
                                         virtual_network,
                                         sizeof(virtual_network));
    if (ret != 0)
    {
        return ret;
    }

    if (!linkg_network_ipv4_to_string(&g_nat.ethernet.ip,
                                      ethernet_address,
                                      sizeof(ethernet_address)))
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-A", LINKG_NAT_CHAIN_POSTROUTING,
                        "-s", virtual_network,
                        "-d", ethernet_network,
                        "-o", LINKG_RESOURCE_INTERFACE_ETHERNET,
                        "-j", "SNAT",
                        "--to-source", ethernet_address,
                        NULL);
}

/**
 * @brief 安装本地Ethernet虚拟地址回环访问的Hairpin SNAT规则。
 *
 * 本地Ethernet设备通过本节点虚拟地址访问同一Ethernet子网内设备时，
 * 将源地址转换为本机Ethernet网关地址，强制目标设备回包重新经过
 * LinkG网关，使conntrack可以完成Destination NETMAP反向恢复。
 */
static int _linkg_nat_hairpin_snat_add(void)
{
    char ethernet_address[LINKG_NAT_IPV4_ADDRESS_SIZE];
    char ethernet_network[LINKG_NAT_IPV4_CIDR_SIZE];
    int  ret;

    ret = _linkg_nat_ipv4_config_to_cidr(&g_nat.ethernet_network,
                                         ethernet_network,
                                         sizeof(ethernet_network));
    if (ret != 0)
    {
        return ret;
    }

    if (!linkg_network_ipv4_to_string(&g_nat.ethernet.ip,
                                      ethernet_address,
                                      sizeof(ethernet_address)))
    {
        return -EINVAL;
    }

    return linkg_os_run(LINKG_NAT_IPTABLES_PROGRAM,
                        "-t", "nat",
                        "-A", LINKG_NAT_CHAIN_POSTROUTING,
                        "-s", ethernet_network,
                        "-d", ethernet_network,
                        "-o", LINKG_RESOURCE_INTERFACE_ETHERNET,
                        "-j", "SNAT",
                        "--to-source", ethernet_address,
                        NULL);
}

/**
 * @brief 安装LinkG NAT规则。
 *
 * 启动前首先删除可能存在的LinkG遗留规则，然后重新创建专用Chain。
 * 业务规则全部建立完成后才挂接到iptables内建Chain，避免安装过程中
 * 暴露不完整的NAT配置。任一步骤失败均清理当前安装结果。
 */
static int _linkg_nat_rules_install(void)
{
    int ret;

    _linkg_nat_rules_cleanup_stale();

    ret = _linkg_nat_chain_create(LINKG_NAT_CHAIN_PREROUTING);
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_chain_create(LINKG_NAT_CHAIN_OUTPUT);
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_chain_create(LINKG_NAT_CHAIN_POSTROUTING);
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_destination_netmap_add();
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_local_ethernet_destination_netmap_add();
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_local_destination_netmap_add();
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_source_netmap_add();
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_ethernet_snat_add();
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_hairpin_snat_add();
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_jump_add("PREROUTING", LINKG_NAT_CHAIN_PREROUTING);
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_jump_add("OUTPUT", LINKG_NAT_CHAIN_OUTPUT);
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    ret = _linkg_nat_jump_add("POSTROUTING", LINKG_NAT_CHAIN_POSTROUTING);
    if (ret != 0)
    {
        _linkg_nat_rules_cleanup_stale();
        return ret;
    }

    return 0;
}

/**
 * @brief 删除LinkG NAT规则。
 *
 * 先解除内建Chain跳转，再清空并删除LinkG专用Chain。
 * 删除过程中继续执行剩余清理步骤并返回首次发生的错误。
 */
static int _linkg_nat_rules_remove(void)
{
    int first_error;
    int ret;

    first_error = 0;

    ret = _linkg_nat_jump_remove("PREROUTING", LINKG_NAT_CHAIN_PREROUTING);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_jump_remove("OUTPUT", LINKG_NAT_CHAIN_OUTPUT);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_jump_remove("POSTROUTING", LINKG_NAT_CHAIN_POSTROUTING);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_chain_flush(LINKG_NAT_CHAIN_PREROUTING);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_chain_flush(LINKG_NAT_CHAIN_OUTPUT);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_chain_flush(LINKG_NAT_CHAIN_POSTROUTING);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_chain_delete(LINKG_NAT_CHAIN_PREROUTING);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_chain_delete(LINKG_NAT_CHAIN_OUTPUT);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    ret = _linkg_nat_chain_delete(LINKG_NAT_CHAIN_POSTROUTING);
    if (ret != 0 && first_error == 0)
    {
        first_error = ret;
    }

    return first_error;
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化NAT管理模块。
 *
 * 根据网络配置提前计算并缓存NAT规则所需的地址信息。
 * 本阶段仅初始化模块软件状态，不修改系统iptables规则。
 */
int linkg_nat_init(const linkg_network_config_t *network_config)
{
    linkg_nat_context_t context;
    int                 ret;

    if (network_config == NULL)
    {
        return -EINVAL;
    }

    if (g_nat.initialized)
    {
        return 0;
    }

    memset(&context, 0, sizeof(context));

    ret = _linkg_nat_build_context(network_config, &context);
    if (ret != 0)
    {
        return ret;
    }

    context.initialized = true;

    g_nat = context;

    return 0;
}

/**
 * @brief 启动NAT管理模块。
 *
 * 安装本节点虚拟网络与Ethernet网络之间的NAT规则。
 * 只有全部规则安装成功后模块才进入启动状态。
 */
int linkg_nat_start(void)
{
    int ret;

    if (!g_nat.initialized)
    {
        return -ENODEV;
    }

    if (g_nat.started)
    {
        return 0;
    }

    ret = _linkg_nat_rules_install();
    if (ret != 0)
    {
        return ret;
    }

    g_nat.started = true;

    return 0;
}

/**
 * @brief 停止NAT管理模块。
 *
 * 删除LinkG安装的NAT规则。
 */
int linkg_nat_stop(void)
{
    int ret;

    if (!g_nat.initialized)
    {
        return -ENODEV;
    }

    if (!g_nat.started)
    {
        return 0;
    }

    ret = _linkg_nat_rules_remove();
    if (ret != 0)
    {
        return ret;
    }

    g_nat.started = false;

    return 0;
}

/**
 * @brief 反初始化NAT管理模块。
 *
 * 调用前NAT模块必须已经停止。
 */
int linkg_nat_deinit(void)
{
    if (!g_nat.initialized)
    {
        return 0;
    }

    if (g_nat.started)
    {
        return -EBUSY;
    }

    memset(&g_nat, 0, sizeof(g_nat));

    g_nat.node_id = LINKG_RESOURCE_NODE_ID_INVALID;

    return 0;
}
