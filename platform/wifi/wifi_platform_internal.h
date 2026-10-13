/**
 * @file wifi_platform_internal.h
 * @brief LinkG Wi-Fi 平台层内部定义
 */

#ifndef WIFI_PLATFORM_INTERNAL_H
#define WIFI_PLATFORM_INTERNAL_H

#include "linkg_log.h"
#include "linkg_system_resources.h"

/****************************** 平台配置 ******************************/

#define WIFI_PLATFORM_INTERFACE_NAME           LINKG_RESOURCE_INTERFACE_WIFI                                     // Wi-Fi网络接口名称
#define LINKG_WIFI_ENABLE_EXTENDED_CHANNELS    1                                                                 // 0=用户信道，1=测试或已许可的扩展信道

/****************************** 驱动配置 ******************************/

#define WIFI_DRIVER_PLATFORM_MODULE_NAME       "plat_1105"                                                       // HI1105平台模块名称
#define WIFI_DRIVER_PLATFORM_MODULE_PATH       "/app/current/drivers/plat_1105.ko"                               // HI1105平台模块路径
#define WIFI_DRIVER_WIFI_MODULE_NAME           "wifi_1105"                                                       // HI1105 Wi-Fi模块名称
#define WIFI_DRIVER_WIFI_MODULE_PATH           "/app/current/drivers/wifi_1105.ko"                               // HI1105 Wi-Fi模块路径

/****************************** 硬件版本 ******************************/

/**
 * 当前测试板：
 *   HWID1=0, HWID2=1：国产PA
 *   HWID1=0, HWID2=0：进口PA
 *
 * 当前00测试板使用进口版本。
 * V2硬件版本接入国产PA。
 */

#define WIFI_DRIVER_HARDWARE_ID_DOMESTIC       1U                                                                // 国产PA硬件版本
#define WIFI_DRIVER_HARDWARE_ID_IMPORTED       0U                                                                // 进口PA硬件版本

#define WIFI_DRIVER_ENABLE_LQ10_COMPAT    	   1U 																 // 允许旧LQ10测试板使用Wi-Fi资源
#define WIFI_DRIVER_HARDWARE_ID_LQ10           3U 																 // LQ10测试板当前读取到的GPIO编码

/****************************** INI配置 ******************************/

#define WIFI_DRIVER_INI_DIRECTORY              "/app/current/drivers"                                            // HI1105驱动INI目录
#define WIFI_DRIVER_ACTIVE_INI_PATH            WIFI_DRIVER_INI_DIRECTORY "/cfg_hi1105.ini"                       // 当前生效INI软链接
#define WIFI_DRIVER_IMPORTED_NARROW_INI_TARGET "ini/imported/narr.ini"                                           // 进口版窄带INI目标
#define WIFI_DRIVER_IMPORTED_WIDE_INI_TARGET   "ini/imported/wide.ini"                                           // 进口版宽带INI目标
#define WIFI_DRIVER_DOMESTIC_NARROW_INI_TARGET "ini/domestic/narr.ini"                                           // 国产版窄带INI目标
#define WIFI_DRIVER_DOMESTIC_WIDE_INI_TARGET   "ini/domestic/wide.ini"                                           // 国产版宽带INI目标
#define WIFI_DRIVER_INI_MAX_SIZE               (128U * 1024U)                                                    // HI1105 INI最大文件大小

/****************************** 固件配置 ******************************/

#define WIFI_DRIVER_FIRMWARE_DIRECTORY         "/vendor/firmware/hi1105/pilot"                                   // HI1105固件根目录
#define WIFI_DRIVER_FIRMWARE_ACTIVE_PATH       WIFI_DRIVER_FIRMWARE_DIRECTORY "/active"                          // 当前硬件版本固件软链接
#define WIFI_DRIVER_FIRMWARE_IMPORTED_TARGET   "imported"                                                        // 进口版固件目标目录
#define WIFI_DRIVER_FIRMWARE_DOMESTIC_TARGET   "domestic"                                                        // 国产版固件目标目录

/****************************** 日志定义 ******************************/

#define WIFI_DRIVER_LOG_TAG                    "WIFI-DRIVER"                                                     // Wi-Fi驱动日志标签
#define WIFI_SERVICE_LOG_TAG                   "WIFI-SERVICE"                                                    // Wi-Fi服务日志标签

#ifndef LINKG_WIFI_DEBUG_ENABLE
#define LINKG_WIFI_DEBUG_ENABLE                0                                                                 // Wi-Fi调试日志开关
#endif

#if LINKG_WIFI_DEBUG_ENABLE
#define WIFI_DRIVER_DEBUG(fmt, ...)            LINKG_LOG_DEBUG("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)   // 驱动调试日志
#define WIFI_SERVICE_DEBUG(fmt, ...)           LINKG_LOG_DEBUG("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 服务调试日志
#else
#define WIFI_DRIVER_DEBUG(fmt, ...)            do { } while (0)                                                  // 驱动调试日志关闭
#define WIFI_SERVICE_DEBUG(fmt, ...)           do { } while (0)                                                  // 服务调试日志关闭
#endif

#define WIFI_DRIVER_INFO(fmt, ...)             LINKG_LOG_INFO("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)    // 驱动信息日志
#define WIFI_DRIVER_WARN(fmt, ...)             LINKG_LOG_WARN("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)    // 驱动警告日志
#define WIFI_DRIVER_ERROR(fmt, ...)            LINKG_LOG_ERROR("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)   // 驱动错误日志
#define WIFI_DRIVER_FATAL(fmt, ...)            LINKG_LOG_FATAL("%s: " fmt, WIFI_DRIVER_LOG_TAG, ##__VA_ARGS__)   // 驱动致命日志

#define WIFI_SERVICE_INFO(fmt, ...)            LINKG_LOG_INFO("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)   // 服务信息日志
#define WIFI_SERVICE_WARN(fmt, ...)            LINKG_LOG_WARN("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)   // 服务警告日志
#define WIFI_SERVICE_ERROR(fmt, ...)           LINKG_LOG_ERROR("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 服务错误日志
#define WIFI_SERVICE_FATAL(fmt, ...)           LINKG_LOG_FATAL("%s: " fmt, WIFI_SERVICE_LOG_TAG, ##__VA_ARGS__)  // 服务致命日志

#endif /* WIFI_PLATFORM_INTERNAL_H */
