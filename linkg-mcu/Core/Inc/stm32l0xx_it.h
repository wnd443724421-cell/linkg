/**
 * @file stm32l0xx_it.h
 * @brief 异常与 UART 中断入口
 */

#ifndef LINKG_STM32L0XX_IT_H
#define LINKG_STM32L0XX_IT_H  // 头文件保护

/****************************** 中断入口 ******************************/

void NMI_Handler(void);
void HardFault_Handler(void);
void SVC_Handler(void);
void PendSV_Handler(void);
void SysTick_Handler(void);
void USART2_IRQHandler(void);
void LPUART1_IRQHandler(void);

#endif
