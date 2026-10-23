/**
 * @file linkg_system_resources.h
 * @brief LinkG系统固定资源分配
 */

#ifndef LINKG_SYSTEM_RESOURCES_H
#define LINKG_SYSTEM_RESOURCES_H

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 节点资源 ******************************/

#define LINKG_RESOURCE_NODE_ID_INVALID       0U   // 无效节点编号
#define LINKG_RESOURCE_NODE_ID_MIN           1U   // 最小有效节点编号
#define LINKG_RESOURCE_NODE_ID_MAX           254U // 最大有效节点编号
#define LINKG_RESOURCE_NODE_ID_BROADCAST     255U // 广播节点编号，普通节点禁止使用

#define LINKG_RESOURCE_NETWORK_NODE_MAX      17U  // 单个组网最大节点数量，1个AP加16个STA
#define LINKG_RESOURCE_NETWORK_STA_MAX       16U  // 单个组网最大STA数量

/****************************** 组网地址 ******************************/

#define LINKG_RESOURCE_TUN_IPV4_NETWORK      "172.31.8.0"  // TUN固定IPv4网段
#define LINKG_RESOURCE_TUN_IPV4_PREFIX       24U           // TUN固定前缀长度
#define LINKG_RESOURCE_TUN_MTU               1500U         // TUN固定MTU

#define LINKG_RESOURCE_WIFI_IPV4_NETWORK     "11.21.191.0" // Wi-Fi固定IPv4网段
#define LINKG_RESOURCE_WIFI_IPV4_PREFIX      24U           // Wi-Fi固定前缀长度

/****************************** 网络端口 ******************************/

#define LINKG_RESOURCE_PORT_RESERVED_START             29600U // LinkG系统保留端口起始值
#define LINKG_RESOURCE_PORT_RESERVED_END               29615U // LinkG系统保留端口结束值

#define LINKG_RESOURCE_UDP_PORT_WIFI_DATA              29600U // Wi-Fi普通数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_WIFI_REALTIME          29601U // Wi-Fi实时数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_WIFI_VIDEO             29602U // Wi-Fi视频数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_DATA          29603U // 蜂窝普通数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_REALTIME      29604U // 蜂窝实时数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_VIDEO         29605U // 蜂窝视频数据UDP端口
#define LINKG_RESOURCE_UDP_PORT_WIFI_DISCOVERY         29606U // Wi-Fi设备发现UDP端口
#define LINKG_RESOURCE_UDP_PORT_CELLULAR_DISCOVERY     29607U // 蜂窝设备发现UDP端口
#define LINKG_RESOURCE_TCP_PORT_WEB_CONTROL            29608U // Web CGI本机控制TCP端口
#define LINKG_RESOURCE_TCP_PORT_WEBSOCKET              29609U // WebSocket服务预留端口

/****************************** NAT资源 ******************************/

#define LINKG_RESOURCE_NAT_SNAT_PORT_START             61000U // Fast NAT SNAT端口池起始值
#define LINKG_RESOURCE_NAT_SNAT_PORT_COUNT             4096U  // Fast NAT SNAT端口池大小
#define LINKG_RESOURCE_NAT_SNAT_PORT_END               65095U // Fast NAT SNAT端口池结束值

/****************************** 网络接口 ******************************/

#define LINKG_RESOURCE_INTERFACE_WIFI      "wlan0"  // Wi-Fi网络接口
#define LINKG_RESOURCE_INTERFACE_CELLULAR  "usb0"   // 蜂窝网络接口
#define LINKG_RESOURCE_INTERFACE_TUN       "linkg0" // LinkG虚拟网络接口
#define LINKG_RESOURCE_INTERFACE_ETHERNET  "eth0"   // 以太网接口
#define LINKG_RESOURCE_INTERFACE_LOOPBACK  "lo"     // 本机回环接口

/****************************** GPIO硬件资源 ******************************/

#define LINKG_RESOURCE_GPIO_SBUS_SWITCH          "PD16" // SBUS通路切换控制
#define LINKG_RESOURCE_GPIO_LED_RUN              "PD1"  // 系统运行LED

#define LINKG_RESOURCE_GPIO_WIFI_RSSI_BLUE       "PD9"  // Wi-Fi RSSI蓝灯
#define LINKG_RESOURCE_GPIO_WIFI_RSSI_GREEN      "PD6"  // Wi-Fi RSSI绿灯
#define LINKG_RESOURCE_GPIO_WIFI_RSSI_RED        "PD5"  // Wi-Fi RSSI红灯

#define LINKG_RESOURCE_GPIO_CELLULAR_RSSI_BLUE   "PC0" // 蜂窝RSSI蓝灯
#define LINKG_RESOURCE_GPIO_CELLULAR_RSSI_GREEN  "PF3" // 蜂窝RSSI绿灯
#define LINKG_RESOURCE_GPIO_CELLULAR_RSSI_RED    "PF5" // 蜂窝RSSI红灯

#define LINKG_RESOURCE_GPIO_RESET_KEY            "PD8"  // 长按复位按键

#define LINKG_RESOURCE_GPIO_HWID1                "PB8"  // 硬件版本识别位1
#define LINKG_RESOURCE_GPIO_HWID2                "PB9"  // 硬件版本识别位2

/****************************** 蜂窝硬件 ******************************/

#define LINKG_RESOURCE_CELLULAR_SIM_INSERT_ACTIVE_HIGH true // SIM_DET高电平表示SIM插入

/****************************** 串口硬件资源 ******************************/

#define LINKG_RESOURCE_UART0_DEVICE "/dev/ttyS4" // 逻辑串口0
#define LINKG_RESOURCE_UART1_DEVICE "/dev/ttyS3" // 逻辑串口1
#define LINKG_RESOURCE_UART2_DEVICE "/dev/ttyS5" // 逻辑串口2

#ifdef __cplusplus
}
#endif

#endif
