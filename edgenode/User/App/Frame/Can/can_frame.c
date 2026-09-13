#include "can_frame.h"
#include "CAN/can.h"

/*
    这里我们做帧协议封装发送，由于暂时没有加入bootloard，所以我们不做帧协议解析
*/

uint8_t can_frame_send(uint8_t node_id, uint16_t sequence, const telemetry_sample_struct *message)
{
    uint8_t data[CAN_TELEMETRY_LENGTH];
    uint16_t can_id;
    uint8_t mailbox;

    if((node_id == 0U) || (node_id > 127U) || (message == 0))
    {
        return CAN_FRAME_FAIL;
    }

    can_id = CAN_TELEMETRY_BASE_ID + node_id;

    data[0] = (uint8_t)(sequence >> 8);
    data[1] = (uint8_t)sequence;

    data[2] = (uint8_t)((uint32_t)message->temperature >> 24);
    data[3] = (uint8_t)((uint32_t)message->temperature >> 16);
    data[4] = (uint8_t)((uint32_t)message->temperature >> 8);
    data[5] = (uint8_t)message->temperature;
    data[6] = (uint8_t)message->temperature_scale;

    mailbox = can0_data_send(can_id, data, CAN_TELEMETRY_LENGTH);

    /*
        CAN0_TX_MAILBOX_NONE
        0 / 1 / 2  → CAN 硬件已接收数据，放入对应发送邮箱
        3          → 本次请求未被 CAN 硬件接受
    */
    return (mailbox == CAN0_TX_MAILBOX_NONE) ? CAN_FRAME_FAIL : CAN_FRAME_SUCCESS;
}
