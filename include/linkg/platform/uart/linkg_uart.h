/**
 * @file linkg_uart.h
 * @brief Linux平台通用串口接口
 */

#ifndef LINKG_UART_H
#define LINKG_UART_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/****************************** 串口配置 ******************************/

typedef struct
{
    int  baudrate;         // 波特率
    int  data_bits;        // 数据位
    int  stop_bits;        // 停止位
    char parity;           // 校验方式
    bool hw_flow_control;  // 是否启用硬件流控
    bool exclusive;        // 是否独占串口设备
} uart_config_t;

/****************************** 生命周期 ******************************/

int  linux_uart_open(const char *dev, const uart_config_t *config);
int  linux_uart_close(int fd);

/****************************** 数据收发 ******************************/

int  linux_uart_write(int fd, const void *buf, int len);
int  linux_uart_read(int fd, void *buf, int len);

/****************************** 控制接口 ******************************/

void linux_uart_flush(int fd);

#ifdef __cplusplus
}
#endif

#endif
