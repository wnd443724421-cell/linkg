#ifndef  USART_SERVER_H
#define  USART_SERVER_H

#ifdef __cplusplus
extern "C" {
#endif
//****************C****************************
#include "main.h"

		
#define  _usart_rec_len  50
#pragma pack(1)

typedef struct
{
	uint16_t len;
	uint8_t data[_usart_rec_len];
}tUSART_DATA;

#pragma pack()
void usart_sbus_init(void);
void usart_cpu_init(void);

//****************C END************************
#ifdef __cplusplus
}
#endif

#endif
