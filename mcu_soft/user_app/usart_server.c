#include "usart_server.h"
#include "stdio.h"
#include "string.h"
#include "debug.h"
tUSART_DATA usart_cpu_data;
tUSART_DATA usart_sbus_data;

#define usart_ht_de(n)  HAL_GPIO_WritePin(EN1_485_GPIO_Port,EN1_485_Pin,(GPIO_PinState)n)
#define usart_lcd_de(n)  HAL_GPIO_WritePin(EN2_485_GPIO_Port,EN2_485_Pin,(GPIO_PinState)n)
static tUSART_DATA usart_cpu_data_buf, usart_sbus_data_buf;


typedef struct
{
    uint8_t timeout;  //超时时间(ms)

} tUSART_TIMEOUT;
//static uint16_t  usart2_timeout = 0, usart3_timeout = 0, usart4_timeout = 0;



static int usart_rxIdle_it_init(UART_HandleTypeDef* huart, uint8_t* data, uint16_t len)
{
//HAL_UARTEx_ReceiveToIdle_IT   HAL_UART_Receive_IT
    HAL_StatusTypeDef status;
    uint8_t cnt = 0;
    do
    {
        huart->RxState = HAL_UART_STATE_READY;

        status = HAL_UARTEx_ReceiveToIdle_IT(huart, data, len);
        if(cnt++ > 2)
        {
            HAL_NVIC_SystemReset();
        }
    }
    while(status != HAL_OK);
    return 0;
}

void usart_sbus_init(void)
{
    //sbus串口1 接线端口
    memset(&usart_sbus_data_buf, 0, sizeof(usart_sbus_data_buf));
    if(usart_rxIdle_it_init(&hlpuart1, usart_sbus_data_buf.data, sizeof(usart_sbus_data_buf.data)) < 0)
        debug_printf("init sbus usart err\r\n");
}
void usart_cpu_init(void)
{
    //串口2和cpu连接
    memset(&usart_cpu_data_buf, 0, sizeof(usart_cpu_data_buf));
    if(usart_rxIdle_it_init(&huart2, usart_cpu_data_buf.data, sizeof(usart_cpu_data_buf.data)) < 0)
        debug_printf("init cpu usart err\r\n");
}

void HAL_UART_ErrorCallback(UART_HandleTypeDef* huart)
{
    if(huart->Instance == LPUART1)
    {
        //调试
        usart_sbus_init();

    }
    else if(huart->Instance == USART2)
    {
        //串口屏
        debug_printf("usart2 err lcd\r\n");
        usart_cpu_init();

    }

}

void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef* huart, uint16_t Size)
{
    if(huart->Instance == LPUART1)
    {
        //调试
        if(usart_sbus_data.len == 0)
        {
            memcpy(usart_sbus_data.data, usart_sbus_data_buf.data, Size);
            usart_sbus_data.len = Size;
            usart_sbus_init();

            if(Size == 25)
            {

                huart2.gState = HAL_UART_STATE_READY;
                HAL_UART_Transmit_IT(&huart2, usart_sbus_data.data, Size);

            }          
            usart_sbus_data.len = 0;
        }


    }
    else if(huart->Instance == USART2)
    {
        //HAL_UART_RxEventTypeTypeDef ev=HAL_UARTEx_GetRxEventType(huart);
        //debug_printf("event:0x%x\r\n",ev);
        //串口屏
        if(usart_cpu_data.len == 0)
        {

            memcpy(usart_cpu_data.data, usart_cpu_data_buf.data, Size);
            usart_cpu_data.len = Size;
            usart_cpu_init();
            if(Size == 25)
            {

                hlpuart1.gState = HAL_UART_STATE_READY;
                HAL_UART_Transmit_IT(&hlpuart1, usart_cpu_data.data, Size);
            }
            usart_cpu_data.len = 0;
        }



    }

}
void HAL_UART_RxCpltCallback(UART_HandleTypeDef* huart)
{

    if(huart->Instance == LPUART1)
    {
        //调试
//        if(usart_sbus_data.len == 0)
//        {
//            memcpy(usart_sbus_data.data, usart_sbus_data_buf.data, sizeof(usart_sbus_data_buf.data));
//            usart_sbus_data.len = sizeof(usart_sbus_data_buf.data);
//					  if(Size==25)
//						{
//							HAL_UART_Transmit_IT(&huart2,usart_sbus_data.data,Size);
//						}
//        }
        usart_sbus_init();

    }
    else if(huart->Instance == USART2)
    {
        //串口屏
//        if(usart_cpu_data.len == 0)
//        {
//            memcpy(usart_cpu_data.data, usart_cpu_data_buf.data, sizeof(usart_cpu_data_buf.data));
//            usart_cpu_data.len = sizeof(usart_cpu_data_buf.data);
//
//        }
        usart_cpu_init();

    }
}
