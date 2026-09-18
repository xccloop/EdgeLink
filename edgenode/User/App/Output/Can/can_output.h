#ifndef CAN_OUTPUT_H_
#define CAN_OUTPUT_H_

#include <stdint.h>
#include "Model/message.h"

/*
    CAN输出是APP到CAN BSP的唯一出口：它选择CAN ID，调用协议编码，并把结果交给硬件发送。
    协议字段定义保留在Protocol/Can，避免上层调用者同时理解协议和发送邮箱。
*/
uint8_t can_telemetry_send(uint8_t node_id,
                           const telemetry_sample_struct *message);

#endif
