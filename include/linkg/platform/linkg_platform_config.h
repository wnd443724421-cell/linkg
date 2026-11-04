/**
 * @file linkg_platform_config.h
 * @brief LinkG平台功能编译配置
 */

#ifndef LINKG_PLATFORM_CONFIG_H
#define LINKG_PLATFORM_CONFIG_H

/****************************** 系统服务 ******************************/

#ifndef LINKG_PLATFORM_SSH_ENABLE
#define LINKG_PLATFORM_SSH_ENABLE 1U // SSH服务开关，0=关闭，1=开启
#endif

#if (LINKG_PLATFORM_SSH_ENABLE != 0U) && (LINKG_PLATFORM_SSH_ENABLE != 1U)
#error "LINKG_PLATFORM_SSH_ENABLE must be 0 or 1"
#endif

#endif // LINKG_PLATFORM_CONFIG_H
