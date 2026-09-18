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

uint8_t can_ack_decode(uint16_t standard_id,
                       const uint8_t data[CAN_ACK_LENGTH],
                       uint8_t data_length,
                       uint8_t expected_node_id,
                       uint32_t *sequence,
                       uint8_t *ack_status)
{
    if((data == 0) || (sequence == 0) || (ack_status == 0) ||
       (expected_node_id == 0U) || (expected_node_id > 127U) ||
       (standard_id != (CAN_ACK_BASE_ID + expected_node_id)) ||
       (data_length != CAN_ACK_LENGTH))
    {
        return CAN_FRAME_FAIL;
    }

    if(data[4] != 0U)
    {
        return CAN_FRAME_FAIL;
    }

    *sequence = ((uint32_t)data[0] << 24U) |
                ((uint32_t)data[1] << 16U) |
                ((uint32_t)data[2] << 8U) |
                (uint32_t)data[3];
    *ack_status = 0U;

    return CAN_FRAME_SUCCESS;
}
