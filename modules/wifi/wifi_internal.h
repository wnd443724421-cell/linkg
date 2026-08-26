/**
 * @file wifi_internal.h
 * @brief LinkG Wi-Fi模块内部定义
 * @author Dawn
 * @version 1.2.0
 * @date 2026-08-26
 */

#ifndef WIFI_INTERNAL_H
#define WIFI_INTERNAL_H

#include <pthread.h>
#include <stdint.h>

#include "linkg_log.h"
#include "linkg_network_config.h"
#include "linkg_wifi_config.h"

#include "wifi_runtime.h"

/****************************** 日志定义 ******************************/

#define LINKG_WIFI_LOG_TAG       "WIFI"                                                               // Wi-Fi模块日志标签
#define LINKG_WIFI_DEBUG_ENABLE  0                                                                    // Wi-Fi调试日志开关
#define WIFI_INFO(fmt, ...)      LINKG_LOG_INFO("%s: " fmt, LINKG_WIFI_LOG_TAG, ##__VA_ARGS__)        // 信息日志
#define WIFI_WARN(fmt, ...)      LINKG_LOG_WARN("%s: " fmt, LINKG_WIFI_LOG_TAG, ##__VA_ARGS__)        // 警告日志
#define WIFI_ERROR(fmt, ...)     LINKG_LOG_ERROR("%s: " fmt, LINKG_WIFI_LOG_TAG, ##__VA_ARGS__)       // 错误日志
#define WIFI_FATAL(fmt, ...)     LINKG_LOG_FATAL("%s: " fmt, LINKG_WIFI_LOG_TAG, ##__VA_ARGS__)       // 致命日志

#if LINKG_WIFI_DEBUG_ENABLE
#define WIFI_DEBUG(fmt, ...)     LINKG_LOG_DEBUG("%s: " fmt, LINKG_WIFI_LOG_TAG, ##__VA_ARGS__)       // 调试日志
#else
#define WIFI_DEBUG(fmt, ...)     do { } while (0)                                                      // 调试日志关闭时忽略
#endif

/****************************** 生命周期 ******************************/

typedef enum
{
    LINKG_WIFI_LIFECYCLE_UNINITIALIZED = 0, // 模块未初始化
    LINKG_WIFI_LIFECYCLE_INITIALIZING,      // 模块正在初始化
    LINKG_WIFI_LIFECYCLE_STOPPED,           // 模块已初始化但未启动
    LINKG_WIFI_LIFECYCLE_STARTING,          // 模块正在启动
    LINKG_WIFI_LIFECYCLE_RUNNING,           // 模块正在运行
    LINKG_WIFI_LIFECYCLE_STOPPING,          // 模块正在停止
    LINKG_WIFI_LIFECYCLE_DEINITIALIZING,    // 模块正在反初始化
    LINKG_WIFI_LIFECYCLE_ERROR              // 模块处于异常状态
} linkg_wifi_lifecycle_t;

/****************************** 模块上下文 ******************************/

typedef struct
{
    pthread_mutex_t             lock;      // 模块状态互斥锁
    linkg_wifi_lifecycle_t      lifecycle; // 模块生命周期状态
    linkg_device_role_t         role;      // 当前设备角色
    uint8_t                     node_id;   // LinkG节点编号
    linkg_wifi_config_t         config;    // Wi-Fi运行配置
    linkg_network_ipv4_config_t ipv4;      // Wi-Fi接口IPv4配置
    wifi_runtime_t              runtime;   // Wi-Fi内部逻辑运行状态
} linkg_wifi_context_t;

#endif
