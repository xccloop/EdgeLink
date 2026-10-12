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

/* Only network address/status lines are retained; command echoes are excluded. */
#define ESP12S_DIAGNOSTIC_LINE_SIZE 96U
uint8_t esp12s_diagnostic_get(char *buffer, uint8_t size);

//获取ip
uint8_t esp12s_local_ip_get(char *buffer, uint8_t size);

/* APP发送新AT命令前仅清旧AT事件，不丢弃 TCP 半包。 */
void esp12s_response_reset(void);

/* 回调在 USART1 中断中执行；每次交付一个 +IPD 负载字节及连接编号。 */
typedef void (*esp12s_data_receive_callback_t)(uint8_t data, uint32_t generation);
void esp12s_data_receive_callback_set(esp12s_data_receive_callback_t callback);

/* 连接切换时递增，上层可据此识别旧连接的数据。 */
uint32_t esp12s_tcp_generation_get(void);

#endif
