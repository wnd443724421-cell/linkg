/**
 * @file bsp_uart.h
 * @brief 双 UART 配置与非阻塞字节接口
 */

#ifndef LINKG_BSP_UART_H
#define LINKG_BSP_UART_H                 // 头文件保护

#include <stdbool.h>
#include <stdint.h>

/****************************** 模块常量 ******************************/

#define BSP_UART_ERROR_PARITY  (1U << 0) // 奇偶校验错误
#define BSP_UART_ERROR_FRAME   (1U << 1) // 停止位错误
#define BSP_UART_ERROR_NOISE   (1U << 2) // 噪声错误
#define BSP_UART_ERROR_OVERRUN (1U << 3) // 接收寄存器溢出

/****************************** 类型定义 ******************************/

typedef enum
{
    BSP_UART_SBUS  = 0, // LPUART1，外部反相线路
    BSP_UART_HOST  = 1, // USART2，T113 正常极性线路
    BSP_UART_COUNT = 2  // UART 数量
} bsp_uart_port_t;

/****************************** 生命周期 ******************************/

int  bsp_uart_start(void);
void bsp_uart_stop(void);
void bsp_uart_enable_rx_irq(void);

/****************************** 数据接收 ******************************/

int bsp_uart_receive(bsp_uart_port_t port, uint8_t *byte, uint32_t *errors);

/****************************** 数据发送 ******************************/

int  bsp_uart_try_write(bsp_uart_port_t port, uint8_t byte);
void bsp_uart_set_tx_irq(bsp_uart_port_t port, bool enabled);
bool bsp_uart_tx_complete(bsp_uart_port_t port);

#endif
