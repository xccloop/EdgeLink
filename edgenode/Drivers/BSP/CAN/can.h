#ifndef CAN_H_
#define CAN_H_

#include <stdint.h>

/* can0_data_send()返回0、1、2表示硬件已接收进对应发送邮箱；返回本值表示请求未被接受。 */
#define CAN0_TX_MAILBOX_NONE  3U

/* 返回1表示CAN控制器、过滤器和接收中断已完成初始化；返回0表示CAN控制器未进入工作状态。 */
uint8_t Can_init(void);
uint8_t can0_data_send(uint16_t standard_id, const uint8_t *data, uint8_t data_length);

#endif
