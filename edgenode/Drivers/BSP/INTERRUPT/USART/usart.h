#ifndef USART_H_
#define USART_H_

#include <stdint.h>
//同样，编写中断函数无需我们手动定义

extern volatile uint8_t ch340_receive_data;

/* ESP12S完整AT回应事件，由USART1接收中断识别后放入队列。 */
typedef enum
{
    ESP12S_RESPONSE_NONE = 0,
    ESP12S_RESPONSE_OK,
    ESP12S_RESPONSE_ERROR,
    ESP12S_RESPONSE_FAIL,
    ESP12S_RESPONSE_PROMPT,
    ESP12S_RESPONSE_SEND_OK,
    ESP12S_RESPONSE_CONNECT,
    ESP12S_RESPONSE_CLOSED,
    ESP12S_RESPONSE_WIFI_CONNECTED,
    ESP12S_RESPONSE_WIFI_GOT_IP,
    ESP12S_RESPONSE_BUSY,
    ESP12S_RESPONSE_READY
} esp12s_response_t;

/* APP从回应队列取出一个事件；返回0表示当前没有新的完整回应。 */
uint8_t esp12s_response_get(esp12s_response_t *response);

/* APP发送新AT命令前调用，丢弃旧事件并重新同步当前行解析状态。 */
void esp12s_response_reset(void);

#endif
