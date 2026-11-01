/**
 * @file uart_diagnostic_internal.h
 * @brief UART硬件诊断模块内部接口
 */

#ifndef UART_DIAGNOSTIC_INTERNAL_H
#define UART_DIAGNOSTIC_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>

#include "linkg_uart_diagnostic.h"

/****************************** 测试接口 ******************************/

bool uart_diagnostic_cancel_requested(void);
void uart_diagnostic_result_update(uint32_t index, const linkg_uart_diagnostic_test_result_t *result);
int  uart_diagnostic_test_port(uint8_t uart_num, linkg_uart_diagnostic_path_t path, linkg_uart_diagnostic_test_result_t *result);

#endif
