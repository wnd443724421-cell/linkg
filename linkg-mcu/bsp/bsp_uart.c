
/**
 * @file bsp_uart.c
 * @brief 双UART初始化、寄存器收发与错误清除
 * @author Dawn
 * @version 2.0.0
 * @date 2026-10-14
 */

#include "bsp_uart.h"

#include <errno.h>
#include <stddef.h>

#include "stm32l0xx_hal.h"

/****************************** 模块常量 ******************************/

#define BSP_UART_BAUD_RATE 100000U                                              // 两侧统一波特率
#define BSP_UART_DATA_PINS (GPIO_PIN_2 | GPIO_PIN_3 | GPIO_PIN_9 | GPIO_PIN_10) // 四根数据引脚

/****************************** 全局上下文 ******************************/

static UART_HandleTypeDef g_uart_sbus;   // SBUS侧UART句柄，仅供HAL生命周期使用
static UART_HandleTypeDef g_uart_host;   // Host侧UART句柄，仅供HAL生命周期使用
static bool               g_uart_active; // UART是否已成功启动

/****************************** 内部辅助 ******************************/

/**
 * @brief 将逻辑端口转换为硬件寄存器地址。
 */
static USART_TypeDef *bsp_uart_registers(bsp_uart_port_t port)
{
    if (port == BSP_UART_SBUS)
    {
        return LPUART1;
    }

    if (port == BSP_UART_HOST)
    {
        return USART2;
    }

    return NULL;
}

/**
 * @brief 配置8数据位、偶校验、2停止位及指定极性。
 */
static int bsp_uart_configure(UART_HandleTypeDef *uart, USART_TypeDef *instance, bool inverted)
{
    HAL_StatusTypeDef result;

    uart->Instance                      = instance;
    uart->Init.BaudRate                 = BSP_UART_BAUD_RATE;
    uart->Init.WordLength               = UART_WORDLENGTH_9B;
    uart->Init.StopBits                 = UART_STOPBITS_2;
    uart->Init.Parity                   = UART_PARITY_EVEN;
    uart->Init.Mode                     = UART_MODE_TX_RX;
    uart->Init.HwFlowCtl                = UART_HWCONTROL_NONE;
    uart->Init.OverSampling             = UART_OVERSAMPLING_16;
    uart->Init.OneBitSampling           = UART_ONE_BIT_SAMPLE_DISABLE;
    uart->AdvancedInit.AdvFeatureInit   = UART_ADVFEATURE_TXINVERT_INIT | UART_ADVFEATURE_RXINVERT_INIT;
    uart->AdvancedInit.TxPinLevelInvert = inverted ? UART_ADVFEATURE_TXINV_ENABLE : UART_ADVFEATURE_TXINV_DISABLE;
    uart->AdvancedInit.RxPinLevelInvert = inverted ? UART_ADVFEATURE_RXINV_ENABLE : UART_ADVFEATURE_RXINV_DISABLE;

    // HAL的9位字长包含校验位，实际载荷为8位。
    result = HAL_UART_Init(uart);

    if (result == HAL_TIMEOUT)
    {
        return -ETIMEDOUT;
    }

    if (result == HAL_BUSY)
    {
        return -EBUSY;
    }

    return result == HAL_OK ? 0 : -EIO;
}

/**
 * @brief 在UART极性和空闲电平已就绪后连接复用引脚。
 */
static void bsp_uart_connect_pins(void)
{
    GPIO_InitTypeDef config = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    // SBUS侧LPUART1，PA2为TX，PA3为RX。
    config.Pin       = GPIO_PIN_2;
    config.Mode      = GPIO_MODE_AF_PP;
    config.Pull      = GPIO_NOPULL;
    config.Speed     = GPIO_SPEED_FREQ_HIGH;
    config.Alternate = GPIO_AF6_LPUART1;

    HAL_GPIO_Init(GPIOA, &config);

    config.Pin  = GPIO_PIN_3;
    config.Pull = GPIO_PULLDOWN;

    HAL_GPIO_Init(GPIOA, &config);

    // Host侧USART2，PA9为TX，PA10为RX。
    config.Pin       = GPIO_PIN_9;
    config.Pull      = GPIO_NOPULL;
    config.Alternate = GPIO_AF4_USART2;

    HAL_GPIO_Init(GPIOA, &config);

    config.Pin  = GPIO_PIN_10;
    config.Pull = GPIO_PULLUP;

    HAL_GPIO_Init(GPIOA, &config);
}

/****************************** 生命周期 ******************************/

/**
 * @brief 初始化UART外设时钟。
 */
void HAL_UART_MspInit(UART_HandleTypeDef *uart)
{
    if (uart->Instance == LPUART1)
    {
        __HAL_RCC_LPUART1_CLK_ENABLE();
    }
    else if (uart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_ENABLE();
    }
}

/**
 * @brief 关闭UART外设时钟。
 */
void HAL_UART_MspDeInit(UART_HandleTypeDef *uart)
{
    if (uart->Instance == LPUART1)
    {
        __HAL_RCC_LPUART1_CLK_DISABLE();
    }
    else if (uart->Instance == USART2)
    {
        __HAL_RCC_USART2_CLK_DISABLE();
    }
}

/**
 * @brief 初始化双UART并配置数据引脚。
 *
 * 调用方必须已经将CTL拉低，断开UART直通通路。
 */
int bsp_uart_start(void)
{
    int result;

    bsp_uart_stop();

    result = bsp_uart_configure(&g_uart_sbus, LPUART1, true);
    if (result == 0)
    {
        result = bsp_uart_configure(&g_uart_host, USART2, false);
    }

    if (result != 0)
    {
        bsp_uart_stop();
        return result;
    }

    bsp_uart_connect_pins();

    // 两个UART中断使用相同优先级，避免彼此抢占。
    HAL_NVIC_SetPriority(LPUART1_IRQn, 1U, 0U);
    HAL_NVIC_SetPriority(USART2_IRQn, 1U, 0U);

    g_uart_active = true;

    return 0;
}

/**
 * @brief 停止UART收发并将数据引脚恢复为模拟输入状态。
 */
void bsp_uart_stop(void)
{
    GPIO_InitTypeDef config = {0};

    HAL_NVIC_DisableIRQ(LPUART1_IRQn);
    HAL_NVIC_DisableIRQ(USART2_IRQn);

    __DSB();
    __ISB();

    g_uart_active = false;

    __HAL_RCC_LPUART1_CLK_ENABLE();
    __HAL_RCC_USART2_CLK_ENABLE();

    LPUART1->CR1 = 0U;
    USART2->CR1  = 0U;
    LPUART1->CR3 = 0U;
    USART2->CR3  = 0U;

    __HAL_RCC_GPIOA_CLK_ENABLE();

    config.Pin  = BSP_UART_DATA_PINS;
    config.Mode = GPIO_MODE_ANALOG;
    config.Pull = GPIO_NOPULL;

    HAL_GPIO_Init(GPIOA, &config);

    g_uart_sbus.Instance = LPUART1;
    g_uart_host.Instance = USART2;

    (void)HAL_UART_DeInit(&g_uart_sbus);
    (void)HAL_UART_DeInit(&g_uart_host);

    __HAL_RCC_LPUART1_FORCE_RESET();
    __HAL_RCC_USART2_FORCE_RESET();
    __HAL_RCC_LPUART1_RELEASE_RESET();
    __HAL_RCC_USART2_RELEASE_RESET();

    HAL_NVIC_ClearPendingIRQ(LPUART1_IRQn);
    HAL_NVIC_ClearPendingIRQ(USART2_IRQn);

    __DSB();
}

/**
 * @brief 在桥接状态已发布后使能UART接收及错误中断。
 */
void bsp_uart_enable_rx_irq(void)
{
    if (!g_uart_active)
    {
        return;
    }

    SET_BIT(LPUART1->CR3, USART_CR3_EIE);
    SET_BIT(USART2->CR3, USART_CR3_EIE);

    SET_BIT(LPUART1->CR1, USART_CR1_RXNEIE | USART_CR1_PEIE);
    SET_BIT(USART2->CR1, USART_CR1_RXNEIE | USART_CR1_PEIE);

    HAL_NVIC_EnableIRQ(LPUART1_IRQn);
    HAL_NVIC_EnableIRQ(USART2_IRQn);
}

/****************************** 数据接收 ******************************/

/**
 * @brief 读取至多一个真实字节，并清除UART硬件错误。
 */
int bsp_uart_receive(bsp_uart_port_t port, uint8_t *byte, uint32_t *errors)
{
    USART_TypeDef *uart;
    uint32_t       status;
    uint32_t       clear = 0U;

    uart = bsp_uart_registers(port);

    if (uart == NULL || byte == NULL || errors == NULL)
    {
        return -EINVAL;
    }

    *errors = 0U;

    if (!g_uart_active)
    {
        return -EIO;
    }

    status = uart->ISR;

    if ((status & USART_ISR_PE) != 0U)
    {
        *errors |= BSP_UART_ERROR_PARITY;
        clear   |= USART_ICR_PECF;
    }

    if ((status & USART_ISR_FE) != 0U)
    {
        *errors |= BSP_UART_ERROR_FRAME;
        clear   |= USART_ICR_FECF;
    }

    if ((status & USART_ISR_NE) != 0U)
    {
        *errors |= BSP_UART_ERROR_NOISE;
        clear   |= USART_ICR_NCF;
    }

    if ((status & USART_ISR_ORE) != 0U)
    {
        *errors |= BSP_UART_ERROR_OVERRUN;
        clear   |= USART_ICR_ORECF;
    }

    uart->ICR = clear;

    if ((status & USART_ISR_RXNE) == 0U)
    {
        return 0;
    }

    *byte = (uint8_t)uart->RDR;

    return 1;
}

/****************************** 数据发送 ******************************/

/**
 * @brief TXE就绪时写入一个字节，忙时不消费数据。
 */
int bsp_uart_try_write(bsp_uart_port_t port, uint8_t byte)
{
    USART_TypeDef *uart;

    uart = bsp_uart_registers(port);

    if (uart == NULL)
    {
        return -EINVAL;
    }

    if (!g_uart_active)
    {
        return -EIO;
    }

    if ((uart->ISR & USART_ISR_TXE) == 0U)
    {
        return -EAGAIN;
    }

    uart->TDR = byte;

    return 0;
}

/**
 * @brief 根据待发队列状态启停TXE中断。
 */
void bsp_uart_set_tx_irq(bsp_uart_port_t port, bool enabled)
{
    USART_TypeDef *uart;

    uart = bsp_uart_registers(port);

    if (uart == NULL || !g_uart_active)
    {
        return;
    }

    if (enabled)
    {
        SET_BIT(uart->CR1, USART_CR1_TXEIE);
    }
    else
    {
        CLEAR_BIT(uart->CR1, USART_CR1_TXEIE);
    }
}

/**
 * @brief 查询UART最后一个停止位是否发送完成。
 */
bool bsp_uart_tx_complete(bsp_uart_port_t port)
{
    USART_TypeDef *uart;

    uart = bsp_uart_registers(port);

    if (uart == NULL || !g_uart_active)
    {
        return true;
    }

    return (uart->ISR & USART_ISR_TC) != 0U;
}
