#ifndef CAN_FRAME_H_
#define CAN_FRAME_H_

#include <stdint.h>
#include "Model/message.h"

#define CAN_FRAME_SUCCESS  1U
#define CAN_FRAME_FAIL     0U

#define CAN_TELEMETRY_BASE_ID  0x200U
#define CAN_TELEMETRY_LENGTH   7U

/*
    这里只定义CAN遥测协议，不访问CAN硬件。
    Message提供统一业务数据；编码结果交给Output/Can中的发送出口写入CAN BSP。
*/
uint8_t can_frame_encode(uint8_t data[CAN_TELEMETRY_LENGTH], uint16_t sequence,
                         const telemetry_sample_struct *message);

#endif
