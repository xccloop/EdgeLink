#ifndef CAN_FRAME_H_
#define CAN_FRAME_H_


#include <stdint.h>

#define CAN_FRAME_SUCCESS  1U
#define CAN_FRAME_FAIL     0U

#define CAN_TELEMETRY_BASE_ID  0x200U
#define CAN_TELEMETRY_LENGTH   7U

/*
    我们真正希望用户调用这个来进行发送帧
*/
uint8_t can_frame_send(uint8_t node_id, uint16_t sequence,int32_t temperature,int8_t temperature_scale);

#endif