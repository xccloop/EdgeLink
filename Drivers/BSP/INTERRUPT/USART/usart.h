#ifndef USART_H_
#define USART_H_

#include <stdint.h>
//同样，编写中断函数无需我们手动定义

extern volatile uint8_t ch340_receive_data;

/* ESP12S最近一次完整AT命令结果，发送新命令前由应用层置为ESP12S_RESPONSE_NONE */
typedef enum
{
    ESP12S_RESPONSE_NONE = 0,
    ESP12S_RESPONSE_OK,
    ESP12S_RESPONSE_ERROR
} esp12s_response_t;

extern volatile esp12s_response_t esp12s_receive_data;

#endif
