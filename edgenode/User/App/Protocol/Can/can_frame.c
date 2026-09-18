#include "can_frame.h"

/*
    这里仅做CAN帧协议封装。协议层不直接调用CAN BSP，避免“数据字段定义”和
    “硬件发送邮箱”耦合在一起；发送动作由Output/Can完成。
*/

uint8_t can_frame_encode(uint8_t data[CAN_TELEMETRY_LENGTH],
                         const telemetry_sample_struct *message)
{
    if((data == 0) || (message == 0))
    {
        return CAN_FRAME_FAIL;
    }

    data[0] = (uint8_t)(message->sequence >> 24);
    data[1] = (uint8_t)(message->sequence >> 16);
    data[2] = (uint8_t)((uint32_t)message->sequence >> 8);
    data[3] = (uint8_t)((uint32_t)message->sequence);

    data[4] = (uint8_t)((uint32_t)message->temperature >> 8);
    data[5] = (uint8_t)message->temperature;
    
    data[6] = (uint8_t)message->temperature_scale;

    return CAN_FRAME_SUCCESS;
}
