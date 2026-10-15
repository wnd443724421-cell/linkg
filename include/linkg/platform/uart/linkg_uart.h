/**
 * @file linkg_uart.h
 * @brief Linux平台通用串口接口
 */

#ifndef LINKG_UART_H
#define LINKG_UART_H

#include <stdbool.h>
#include <stdint.h>

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

#define LINKG_UART_BAUD_ERROR_MAX_PPM 20000U // 最大允许分频误差：2%，不包含晶振误差

typedef struct
{
    uint32_t baud_base; // 驱动上报的UART输入时钟除以16
    uint32_t divisor;   // 整数分频值
    double   actual;    // 按驱动时钟计算的波特率，非示波器实测
    double   error_pct; // 相对请求波特率的百分比误差
} linux_uart_baud_info_t;

/****************************** 波特率诊断 ******************************/

int linux_uart_baud_info(int fd, uint32_t baudrate, linux_uart_baud_info_t *out);

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
