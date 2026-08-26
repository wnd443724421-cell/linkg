/**
 * @file wifi_service_internal.h
 * @brief LinkG Wi-Fi系统服务内部定义
 */

#ifndef WIFI_SERVICE_INTERNAL_H
#define WIFI_SERVICE_INTERNAL_H

#include "linkg_log.h"

/****************************** 运行路径 ******************************/

#define WIFI_SERVICE_RUNTIME_DIR             "/var/run/linkg/wifi"                                           // Wi-Fi服务运行目录
#define WIFI_SERVICE_RUNTIME_DIR_MODE        0755                                                            // 运行目录权限
#define WIFI_SERVICE_CONFIG_MODE             0600                                                            // 运行配置权限

/****************************** 服务参数 ******************************/

#define WIFI_SERVICE_TEMPLATE_MAX_SIZE       (2U * 1024U)                                                    // 服务模板最大长度
#define WIFI_SERVICE_STOP_WAIT_MS            5000U                                                           // 服务停止等待时间
#define WIFI_SERVICE_WAIT_INTERVAL_MS        100U                                                            // 服务状态轮询间隔

/****************************** 日志定义 ******************************/

#define WIFI_SERVICE_LOG_TAG                 "WIFI-SERVICE"                                                  // Wi-Fi服务日志标签

#ifndef LINKG_WIFI_DEBUG_ENABLE
#define LINKG_WIFI_DEBUG_ENABLE              0                                                               // Wi-Fi调试日志开关
#endif

#if LINKG_WIFI_DEBUG_ENABLE
#define WIFI_SERVICE_DEBUG(fmt, ...)         LINKG_LOG_DEBUG("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__) // 调试日志
#else
#define WIFI_SERVICE_DEBUG(fmt, ...)         do { } while (0)                                                 // 调试日志关闭时忽略
#endif

#define WIFI_SERVICE_INFO(fmt, ...)          LINKG_LOG_INFO("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 信息日志
#define WIFI_SERVICE_WARN(fmt, ...)          LINKG_LOG_WARN("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 警告日志
#define WIFI_SERVICE_ERROR(fmt, ...)         LINKG_LOG_ERROR("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__) // 错误日志

#endif
