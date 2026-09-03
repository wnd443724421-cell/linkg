/**
 * @file cellular_internal.h
 * @brief LinkG蜂窝模块内部公共定义
 * @author Dawn
 * @version 1.0.0
 * @date 2026-09-03
 */

#ifndef CELLULAR_INTERNAL_H
#define CELLULAR_INTERNAL_H

#include "linkg_log.h"

/****************************** 日志定义 ******************************/

#define LINKG_CELLULAR_LOG_TAG      "CELLULAR"                                                         // 蜂窝模块日志标签
#define LINKG_CELLULAR_DEBUG_ENABLE 0                                                                  // 蜂窝调试日志开关

#define CELLULAR_INFO(fmt, ...)     LINKG_LOG_INFO("%s: " fmt, LINKG_CELLULAR_LOG_TAG, ##__VA_ARGS__)  // 信息日志
#define CELLULAR_WARN(fmt, ...)     LINKG_LOG_WARN("%s: " fmt, LINKG_CELLULAR_LOG_TAG, ##__VA_ARGS__)  // 警告日志
#define CELLULAR_ERROR(fmt, ...)    LINKG_LOG_ERROR("%s: " fmt, LINKG_CELLULAR_LOG_TAG, ##__VA_ARGS__) // 错误日志
#define CELLULAR_FATAL(fmt, ...)    LINKG_LOG_FATAL("%s: " fmt, LINKG_CELLULAR_LOG_TAG, ##__VA_ARGS__) // 致命日志

#if LINKG_CELLULAR_DEBUG_ENABLE
#define CELLULAR_DEBUG(fmt, ...)    LINKG_LOG_DEBUG("%s: " fmt, LINKG_CELLULAR_LOG_TAG, ##__VA_ARGS__) // 调试日志
#else
#define CELLULAR_DEBUG(fmt, ...)    do { } while (0)                                                    // 调试日志关闭时忽略
#endif

#endif
