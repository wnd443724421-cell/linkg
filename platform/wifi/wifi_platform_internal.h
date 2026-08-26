/**
 * @file wifi_platform_internal.h
 * @brief LinkG Wi-Fi平台层内部定义
 */

#ifndef WIFI_PLATFORM_INTERNAL_H
#define WIFI_PLATFORM_INTERNAL_H

#include "linkg_log.h"
#include "linkg_system_resources.h"

/****************************** 平台配置 ******************************/

#define WIFI_PLATFORM_INTERFACE_NAME        LINKG_RESOURCE_INTERFACE_WIFI                                    // Wi-Fi网络接口名称
#define LINKG_WIFI_ENABLE_EXTENDED_CHANNELS 1                                                                // 0=用户信道，1=测试或已许可的扩展信道

/****************************** 驱动配置 ******************************/

#define WIFI_DRIVER_PLATFORM_MODULE_NAME    "plat_1105"                                                      // HI1105平台模块名称
#define WIFI_DRIVER_PLATFORM_MODULE_PATH    "/app/current/drivers/plat_1105.ko"                              // HI1105平台模块路径
#define WIFI_DRIVER_WIFI_MODULE_NAME        "wifi_1105"                                                      // HI1105 Wi-Fi模块名称
#define WIFI_DRIVER_WIFI_MODULE_PATH        "/app/current/drivers/wifi_1105.ko"                              // HI1105 Wi-Fi模块路径
#define WIFI_DRIVER_NARROWBAND_INI_PATH     "/app/current/drivers/cfg_hi1105_narr.ini"                       // HI1105窄带INI路径
#define WIFI_DRIVER_WIDEBAND_INI_PATH       "/app/current/drivers/cfg_hi1105_wide.ini"                       // HI1105宽带INI路径
#define WIFI_DRIVER_ACTIVE_INI_PATH         "/app/current/drivers/cfg_hi1105.ini"                            // HI1105当前生效INI路径
#define WIFI_DRIVER_INI_MAX_SIZE            (128U * 1024U)                                                   // HI1105 INI最大长度

/****************************** 日志定义 ******************************/

#define WIFI_DRIVER_LOG_TAG                 "WIFI-DRIVER"                                                    // Wi-Fi驱动操作日志标签
#define WIFI_SERVICE_LOG_TAG                "WIFI-SERVICE"                                                   // Wi-Fi系统服务日志标签

#ifndef LINKG_WIFI_DEBUG_ENABLE
#define LINKG_WIFI_DEBUG_ENABLE             0                                                                // Wi-Fi调试日志开关
#endif

#if LINKG_WIFI_DEBUG_ENABLE
#define WIFI_DRIVER_DEBUG(fmt, ...)         LINKG_LOG_DEBUG("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)  // 驱动调试日志
#define WIFI_SERVICE_DEBUG(fmt, ...)        LINKG_LOG_DEBUG("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__) // 服务调试日志
#else
#define WIFI_DRIVER_DEBUG(fmt, ...)         do { } while (0)                                                 // 驱动调试日志
#define WIFI_SERVICE_DEBUG(fmt, ...)        do { } while (0)                                                 // 服务调试日志
#endif

#define WIFI_DRIVER_INFO(fmt, ...)          LINKG_LOG_INFO("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)   // 驱动信息日志
#define WIFI_DRIVER_WARN(fmt, ...)          LINKG_LOG_WARN("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)   // 驱动警告日志
#define WIFI_DRIVER_ERROR(fmt, ...)         LINKG_LOG_ERROR("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)  // 驱动错误日志
#define WIFI_DRIVER_FATAL(fmt, ...)         LINKG_LOG_FATAL("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)  // 驱动致命错误日志

#define WIFI_SERVICE_INFO(fmt, ...)         LINKG_LOG_INFO("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 服务信息日志
#define WIFI_SERVICE_WARN(fmt, ...)         LINKG_LOG_WARN("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 服务警告日志
#define WIFI_SERVICE_ERROR(fmt, ...)        LINKG_LOG_ERROR("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__) // 服务错误日志
#define WIFI_SERVICE_FATAL(fmt, ...)        LINKG_LOG_FATAL("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__) // 服务致命错误日志

#endif
