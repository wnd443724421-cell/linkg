#ifndef LINKG_UART_H
#define LINKG_UART_H
#include <stdbool.h>
typedef struct
{
    int  baudrate;
    int  data_bits;
    int  stop_bits;
    char parity;
    bool hw_flow_control;
    bool exclusive;
} uart_config_t;
#endif
