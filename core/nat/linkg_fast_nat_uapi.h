/**
 * @file linkg_fast_nat_uapi.h
 * @brief LinkG Fast NAT内核模块用户态接口定义
 * @author Dawn
 * @version 2.0.0
 * @date 2026-09-06
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
 * ifindex和SNAT起始端口使用主机字节序。
 * 配置只允许在Fast NAT未运行时修改。
 */
typedef struct
{
    __u16 version;     // UAPI版本，固定为LINKG_FAST_NAT_UAPI_VERSION
    __u16 struct_size; // 当前结构体大小

    linkg_fast_nat_ipv4_subnet_t ethernet_network;     // 本节点Ethernet真实网络
    linkg_fast_nat_ipv4_subnet_t tun_network;          // LinkG TUN节点网络
    linkg_fast_nat_ipv4_subnet_t virtual_network;      // LinkG虚拟聚合网络
    linkg_fast_nat_ipv4_subnet_t local_virtual_subnet; // 本节点虚拟Endpoint子网

    __be32 ethernet_ip; // 本节点Ethernet接口IPv4地址

    __s32 ethernet_ifindex; // Ethernet接口ifindex
    __s32 tun_ifindex;      // LinkG TUN接口ifindex

    __u16 snat_port_start; // Fast SNAT动态端口池起始端口
    __u16 reserved0;       // 对齐及后续扩展预留

    __u32 reserved[8]; // 后续UAPI扩展预留
} linkg_fast_nat_config_t;

/****************************** IOCTL定义 ******************************/

#define LINKG_FAST_NAT_IOC_MAGIC 0xF5U // Fast NAT ioctl命令类型

#define LINKG_FAST_NAT_IOC_SET_CONFIG \
    _IOW(LINKG_FAST_NAT_IOC_MAGIC, 0x01, linkg_fast_nat_config_t)

#define LINKG_FAST_NAT_IOC_START \
    _IO(LINKG_FAST_NAT_IOC_MAGIC, 0x02)

#define LINKG_FAST_NAT_IOC_STOP \
    _IO(LINKG_FAST_NAT_IOC_MAGIC, 0x03)

#endif // LINKG_FAST_NAT_UAPI_H
