#include "can_output.h"
#include "CAN/can.h"
#include "Protocol/Can/can_frame.h"
#include <stdio.h>

/*
    这个函数完成一条Message的CAN输出。
    节点ID决定CAN仲裁ID，协议层只负责填充7字节数据；这里再请求CAN BSP分配发送邮箱。
    这样替换CAN硬件驱动时不需要改CAN帧字段，修改协议字段时也不需要接触硬件发送逻辑。
*/
uint8_t can_telemetry_send(uint8_t node_id,
                           const telemetry_sample_struct *message)
{
    uint8_t data[CAN_TELEMETRY_LENGTH];
    uint8_t mailbox;
    uint16_t can_id;

    if((node_id == 0U) || (node_id > 127U) || (message == 0))
    {
        printf("noid id fail\r\n");
        return CAN_FRAME_FAIL;
    }

    if(can_frame_encode(data, message) == CAN_FRAME_FAIL)
    {
        printf("frame encode fail\r\n");
        return CAN_FRAME_FAIL;
    }

    can_id = CAN_TELEMETRY_BASE_ID + node_id;
    mailbox = can0_data_send(can_id, data, CAN_TELEMETRY_LENGTH);

    return (mailbox == CAN0_TX_MAILBOX_NONE) ? CAN_FRAME_FAIL : CAN_FRAME_SUCCESS;
}

/*
    固件回复帧的载荷：
        data[0]    = kind
        data[1..2] = contiguous（高位在前，和固件数据帧的序号同端序）
        data[3]    = success
    节点号校验和"邮箱满返回 FAIL"都照 can_telemetry_send 来。
*/
uint8_t can_ota_reply_send(uint8_t node_id,
                           const ota_reply_request_t *request)
{
    uint8_t data[CAN_OTA_REPLY_LENGTH];
    uint8_t mailbox;
    uint16_t can_id;

    if((node_id == 0U) || (node_id > 127U) || (request == 0))
    {
        return CAN_FRAME_FAIL;
    }

    data[0] = request->kind;
    data[1] = (uint8_t)(request->contiguous >> 8);
    data[2] = (uint8_t)(request->contiguous & 0xFFU);
    data[3] = request->success;

    can_id = CAN_OTA_REPLY_BASE_ID + node_id;
    mailbox = can0_data_send(can_id, data, CAN_OTA_REPLY_LENGTH);

    return (mailbox == CAN0_TX_MAILBOX_NONE) ? CAN_FRAME_FAIL : CAN_FRAME_SUCCESS;
}
