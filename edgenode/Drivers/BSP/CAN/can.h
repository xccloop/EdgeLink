#ifndef CAN_H_
#define CAN_H_

#include <stdint.h>

/* can0_data_send()返回0、1、2表示硬件已接收进对应发送邮箱；返回本值表示请求未被接受。 */
#define CAN0_TX_MAILBOX_NONE  3U

void Can_init();
uint8_t can0_data_send(uint16_t standard_id, const uint8_t *data, uint8_t data_length);

#endif
