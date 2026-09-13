#ifndef CAN_FRAME_H_
#define CAN_FRAME_H_

#include <stdint.h>
#include "Frame/Message/message.h"


#include <stdint.h>

#define CAN_FRAME_SUCCESS  1U
#define CAN_FRAME_FAIL     0U

#define CAN_TELEMETRY_BASE_ID  0x200U
#define CAN_TELEMETRY_LENGTH   7U

/*
    我们真正希望用户调用这个来进行发送帧
*/
/* Message提供统一业务数据；当前CAN遥测帧只编码其中的温度和倍率。 */
uint8_t can_frame_send(uint8_t node_id, uint16_t sequence, const telemetry_sample_struct *message);

#endif
