#include "tcp_frame.h"
#include "Config/config.h"
#include "Protocol/CRC/crc.h"

/*
    这个文件是用于定义TCP协议帧的文件并且提供TCP帧协议解析，协议内容与要edgehub定义一致
    与edgehub不同我们在这里定义的是frame的封装而不是frame的解析
    其实可以注意到我们并没有使用结构体来定义一个帧，是因为我们是发送方
    所以结构体其实没必要，我们只需要将数据给装进frame里面
    然后发送就好，发送交给esp12s，我们来做封装
*/

static uint32_t tcp_u32_read_be(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24U) |
           ((uint32_t)data[1] << 16U) |
           ((uint32_t)data[2] << 8U) |
           (uint32_t)data[3];
}

uint8_t tcp_frame_encode(uint8_t frame[TCP_FRAME_LENGTH],
                         const telemetry_sample_struct *message)
{
    uint32_t crc;
    if((frame == 0) || (message == 0))
    {
        return 0U;
    }
    frame[0] = 0x45U;
    frame[1] = 0x48U;
    frame[2] = TCP_FRAME_VERSION_V5;
    frame[3] = board_id;
    frame[4] = 0U;

    frame[5] = (uint8_t)(message->sequence >> 24);
    frame[6] = (uint8_t)(message->sequence >> 16);
    frame[7]  = (uint8_t)(message->sequence >> 8);
    frame[8]  = (uint8_t)((uint32_t)message->sequence);

    frame[9]  = (uint8_t)((uint32_t)message->temperature >> 8);
    frame[10] = (uint8_t)message->temperature;

    frame[11] = (uint8_t)message->temperature_scale;

    crc = crc32_generate(&frame[2], TCP_FRAME_CRC_COVER_LENGTH);
    frame[TCP_FRAME_CRC_OFFSET] = (uint8_t)(crc >> 24);
    frame[TCP_FRAME_CRC_OFFSET + 1U] = (uint8_t)(crc >> 16);
    frame[TCP_FRAME_CRC_OFFSET + 2U] = (uint8_t)(crc >> 8);
    frame[TCP_FRAME_CRC_OFFSET + 3U] = (uint8_t)crc;
    return 1U;
}

uint8_t tcp_ack_decode(const uint8_t frame[TCP_FRAME_LENGTH],
                       uint8_t expected_node_id,
                       uint32_t *sequence,
                       uint8_t *ack_status)
{
    uint32_t received_crc;

    if((frame == 0) || (sequence == 0) || (ack_status == 0) ||
       (expected_node_id == 0U))
    {
        return 0U;
    }

    if((frame[0] != 0x45U) || (frame[1] != 0x48U) ||
       (frame[2] != TCP_FRAME_VERSION_V5) ||
       (frame[3] != 0U) || (frame[4] != expected_node_id) ||
       (frame[9] != 0U) || (frame[10] != 0U))
    {
        return 0U;
    }

    received_crc = tcp_u32_read_be(&frame[TCP_FRAME_CRC_OFFSET]);
    if((frame[11] != TCP_ACK_STATUS_SUCCESS) ||
       (crc32_check(&frame[2], TCP_FRAME_CRC_COVER_LENGTH, received_crc) == 0U))
    {
        return 0U;
    }

    *sequence = tcp_u32_read_be(&frame[5]);
    *ack_status = TCP_ACK_STATUS_SUCCESS;
    return 1U;
}
