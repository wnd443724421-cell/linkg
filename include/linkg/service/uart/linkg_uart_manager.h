/**
 * @file linkg_uart_manager.h
 * @brief LinkG串口透传与SBUS服务公共接口
 */

#ifndef LINKG_UART_MANAGER_H
#define LINKG_UART_MANAGER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** SBUS缓冲模式 ******************************/

typedef enum
{
    LINKG_UART_SBUS_PROFILE_WIFI = 0, // Wi-Fi单链路，目标2帧
    LINKG_UART_SBUS_PROFILE_BACKUP    // 冗余或5G，目标5帧
} linkg_uart_sbus_profile_t;

/****************************** 生命周期 ******************************/

int linkg_uart_manager_init(void);
int linkg_uart_manager_start(void);
int linkg_uart_manager_stop(void);
int linkg_uart_manager_deinit(void);

/****************************** 运行配置 ******************************/

int linkg_uart_manager_set_sbus_profile(linkg_uart_sbus_profile_t profile);
int linkg_uart_manager_set_debug(bool enabled);

#ifdef __cplusplus
}
#endif

#endif
