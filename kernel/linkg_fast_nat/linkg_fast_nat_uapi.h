/**
 * @file linkg_fast_nat_uapi.h
 * @brief LinkG Fast NAT内核模块用户态接口定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-01
 */

#ifndef LINKG_FAST_NAT_UAPI_H
#define LINKG_FAST_NAT_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

/****************************** UAPI版本 ******************************/

#define LINKG_FAST_NAT_UAPI_VERSION 1U // Fast NAT用户态接口版本

/****************************** 设备定义 ******************************/

#define LINKG_FAST_NAT_DEVICE_NAME "linkg_fast_nat"      // 字符设备名称
#define LINKG_FAST_NAT_DEVICE_PATH "/dev/linkg_fast_nat" // 字符设备路径

/****************************** 规则定义 ******************************/

/* LinkG虚拟网络基础映射。 */
#define LINKG_FAST_NAT_RULE_SOURCE_NETMAP                      (1U << 0) // Ethernet进入LinkG时替换源IP为本节点虚拟Endpoint地址
#define LINKG_FAST_NAT_RULE_LINKG_TO_ETHERNET_DNAT             (1U << 1) // linkg0进入Ethernet时替换目的IP为本地Ethernet真实地址

/* LinkG虚拟Endpoint访问本地Ethernet。 */
#define LINKG_FAST_NAT_RULE_VIRTUAL_ETHERNET_SNAT              (1U << 2) // 虚拟Endpoint访问本地Ethernet时替换源IP为本机Ethernet地址

/* LinkG本机和TUN节点访问本地Ethernet。 */
#define LINKG_FAST_NAT_RULE_LOCAL_OUTPUT_DESTINATION_NETMAP    (1U << 3) // 本机访问本节点虚拟Endpoint时替换目的IP为Ethernet真实地址
#define LINKG_FAST_NAT_RULE_TUN_ETHERNET_SNAT                  (1U << 4) // TUN节点访问本地Ethernet时替换源IP为本机Ethernet地址

/* 本地Ethernet虚拟地址访问。 */
#define LINKG_FAST_NAT_RULE_LOCAL_ETHERNET_DESTINATION_NETMAP  (1U << 5) // Ethernet访问本节点虚拟Endpoint时替换目的IP为Ethernet真实地址
#define LINKG_FAST_NAT_RULE_HAIRPIN_SNAT                       (1U << 6) // Ethernet回环访问时替换源IP为本机Ethernet地址

#define LINKG_FAST_NAT_RULE_ALL                                \
    (LINKG_FAST_NAT_RULE_SOURCE_NETMAP                     |   \
     LINKG_FAST_NAT_RULE_LINKG_TO_ETHERNET_DNAT            |   \
     LINKG_FAST_NAT_RULE_VIRTUAL_ETHERNET_SNAT             |   \
     LINKG_FAST_NAT_RULE_LOCAL_OUTPUT_DESTINATION_NETMAP   |   \
     LINKG_FAST_NAT_RULE_TUN_ETHERNET_SNAT                 |   \
     LINKG_FAST_NAT_RULE_LOCAL_ETHERNET_DESTINATION_NETMAP |   \
     LINKG_FAST_NAT_RULE_HAIRPIN_SNAT)

/****************************** 状态定义 ******************************/

#define LINKG_FAST_NAT_STATE_UNCONFIGURED 0U // 尚未配置
#define LINKG_FAST_NAT_STATE_CONFIGURED   1U // 已配置但未启动
#define LINKG_FAST_NAT_STATE_RUNNING      2U // Fast NAT正在运行

/****************************** IPv4配置 ******************************/

/**
 * @brief Fast NAT IPv4子网配置。
 *
 * network和netmask均使用IPv4网络字节序。
 * network必须已经规范化，即Host部分必须为0。
 */
typedef struct
{
    __be32 network; // IPv4网络地址
    __be32 netmask; // IPv4子网掩码
} linkg_fast_nat_ipv4_subnet_t;

/****************************** Fast NAT配置 ******************************/

/**
 * @brief Fast NAT运行配置。
 *
 * IPv4地址和掩码使用网络字节序。
 * ifindex、端口范围和控制字段使用主机字节序。
 * 配置只允许在非RUNNING状态下修改。
 */
typedef struct
{
    __u16 version;       // UAPI版本，固定为LINKG_FAST_NAT_UAPI_VERSION
    __u16 struct_size;   // 当前结构体大小
    __u32 enabled_rules; // 当前启用的固定NAT规则位图

    linkg_fast_nat_ipv4_subnet_t ethernet_network;     // 本节点Ethernet真实网络
    linkg_fast_nat_ipv4_subnet_t tun_network;          // LinkG TUN节点网络
    linkg_fast_nat_ipv4_subnet_t virtual_network;      // LinkG虚拟聚合网络
    linkg_fast_nat_ipv4_subnet_t local_virtual_subnet; // 本节点虚拟Endpoint子网

    __be32 ethernet_ip;     // 本节点Ethernet接口IPv4地址

    __s32 ethernet_ifindex; // Ethernet接口ifindex
    __s32 tun_ifindex;      // LinkG TUN接口ifindex

    __u16 snat_port_start;  // Fast SNAT动态端口池起始端口
    __u16 snat_port_end;    // Fast SNAT动态端口池结束端口

    __u32 reserved[8]; // 后续UAPI扩展预留
} linkg_fast_nat_config_t;

/****************************** Fast NAT状态 ******************************/

/**
 * @brief Fast NAT运行状态。
 */
typedef struct
{
    __u16 version;       // UAPI版本
    __u16 struct_size;   // 当前结构体大小
    __u32 state;         // LINKG_FAST_NAT_STATE_*
    __u32 enabled_rules; // 当前实际启用的规则位图

    __u32 reserved[5];   // 后续状态字段扩展预留
} linkg_fast_nat_status_t;

/****************************** IOCTL定义 ******************************/

#define LINKG_FAST_NAT_IOC_MAGIC 0xF5U // Fast NAT ioctl命令类型

#define LINKG_FAST_NAT_IOC_SET_CONFIG \
    _IOW(LINKG_FAST_NAT_IOC_MAGIC, 0x01, linkg_fast_nat_config_t)

#define LINKG_FAST_NAT_IOC_START \
    _IO(LINKG_FAST_NAT_IOC_MAGIC, 0x02)

#define LINKG_FAST_NAT_IOC_STOP \
    _IO(LINKG_FAST_NAT_IOC_MAGIC, 0x03)

#define LINKG_FAST_NAT_IOC_GET_STATUS \
    _IOR(LINKG_FAST_NAT_IOC_MAGIC, 0x04, linkg_fast_nat_status_t)

#endif // LINKG_FAST_NAT_UAPI_H
