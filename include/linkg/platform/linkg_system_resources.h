/**
 * @file linkg_system_resources.h
 * @brief LinkG系统固定资源分配
 */

#ifndef LINKG_SYSTEM_RESOURCES_H
#define LINKG_SYSTEM_RESOURCES_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 组网地址 ******************************/

#define LINKG_RESOURCE_NODE_ID_MIN        1U             // 最小节点编号
#define LINKG_RESOURCE_NODE_ID_MAX        16U            // 最大节点编号

#define LINKG_RESOURCE_TUN_IPV4_NETWORK   "172.31.8.0"   // TUN固定IPv4网段
#define LINKG_RESOURCE_TUN_IPV4_PREFIX    24U            // TUN固定前缀长度
#define LINKG_RESOURCE_TUN_MTU            1500U          // TUN固定MTU

#define LINKG_RESOURCE_WIFI_IPV4_NETWORK  "11.21.191.0"  // Wi-Fi固定IPv4网段
#define LINKG_RESOURCE_WIFI_IPV4_PREFIX   24U            // Wi-Fi固定前缀长度

/****************************** 网络端口 ******************************/

#define LINKG_RESOURCE_UDP_PORT_WIFI_DATA           5000U  // Wi-Fi普通数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_WIFI_REALTIME       5001U  // Wi-Fi实时数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_WIFI_VIDEO          5002U  // Wi-Fi视频数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_DATA       5003U  // 蜂窝普通数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_REALTIME   5004U  // 蜂窝实时数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_VIDEO      5005U  // 蜂窝视频数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY      5006U  // Wi-Fi设备发现UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY  5007U  // 蜂窝路径存活心跳UDP端口

/****************************** 网络接口 ******************************/

#define LINKG_RESOURCE_INTERFACE_WIFI      "wlan0"   // Wi-Fi网络接口
#define LINKG_RESOURCE_INTERFACE_CELLULAR  "usb0"    // 蜂窝网络接口
#define LINKG_RESOURCE_INTERFACE_TUN       "linkg0"  // LinkG虚拟网络接口
#define LINKG_RESOURCE_INTERFACE_ETHERNET  "eth0"    // 以太网接口

#ifdef __cplusplus
}
#endif

#endif
