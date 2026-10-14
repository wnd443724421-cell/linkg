/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2023 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32l0xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

void HAL_TIM_MspPostInit(TIM_HandleTypeDef *htim);

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);
void MX_IWDG_Init(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define SF_MODEL_Pin GPIO_PIN_14
#define SF_MODEL_GPIO_Port GPIOC
#define CPU_BEEP_Pin GPIO_PIN_15
#define CPU_BEEP_GPIO_Port GPIOC
#define KEY_WKUP_Pin GPIO_PIN_0
#define KEY_WKUP_GPIO_Port GPIOA
#define MCU_MODEL_Pin GPIO_PIN_1
#define MCU_MODEL_GPIO_Port GPIOA
#define USART1_TX_Pin GPIO_PIN_2
#define USART1_TX_GPIO_Port GPIOA
#define USART1_RX_Pin GPIO_PIN_3
#define USART1_RX_GPIO_Port GPIOA
#define ADC_BATT_Pin GPIO_PIN_4
#define ADC_BATT_GPIO_Port GPIOA
#define LED_PWR1_Pin GPIO_PIN_5
#define LED_PWR1_GPIO_Port GPIOA
#define LED_PWR2_Pin GPIO_PIN_6
#define LED_PWR2_GPIO_Port GPIOA
#define LED_PWR3_Pin GPIO_PIN_7
#define LED_PWR3_GPIO_Port GPIOA
#define LED_PWR4_Pin GPIO_PIN_0
#define LED_PWR4_GPIO_Port GPIOB
#define CTL_Pin GPIO_PIN_1
#define CTL_GPIO_Port GPIOB
#define BEEP_Pin GPIO_PIN_8
#define BEEP_GPIO_Port GPIOA
#define USART2_TX_Pin GPIO_PIN_9
#define USART2_TX_GPIO_Port GPIOA
#define USART2_RX_Pin GPIO_PIN_10
#define USART2_RX_GPIO_Port GPIOA
#define CHARGE_DETEC_Pin GPIO_PIN_14
#define CHARGE_DETEC_GPIO_Port GPIOA
#define CHARGE_DETEC_EXTI_IRQn EXTI4_15_IRQn
#define PWR_IN_DETE_Pin GPIO_PIN_15
#define PWR_IN_DETE_GPIO_Port GPIOA
#define SWITCH_RESET_Pin GPIO_PIN_3
#define SWITCH_RESET_GPIO_Port GPIOB
#define PWR_5V_ONOFF_Pin GPIO_PIN_6
#define PWR_5V_ONOFF_GPIO_Port GPIOB
#define PWR_OFF_Pin GPIO_PIN_7
#define PWR_OFF_GPIO_Port GPIOB

/* USER CODE BEGIN Private defines */
extern TIM_HandleTypeDef htim2;
extern ADC_HandleTypeDef hadc;
extern UART_HandleTypeDef hlpuart1;
extern UART_HandleTypeDef huart2;
int on_sbus(void);
int off_sbus(void);
extern void MX_IWDG_Init(void);
extern IWDG_HandleTypeDef hiwdg;
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */
