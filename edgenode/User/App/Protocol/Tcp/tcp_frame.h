#ifndef TCP_FRAME_H_
#define TCP_FRAME_H_

#include <stdint.h>
#include "Model/message.h"

#define TCP_FRAME_LENGTH      16U
#define TCP_FRAME_VERSION_V5  0x05U
#define TCP_FRAME_CRC_OFFSET  12U
#define TCP_FRAME_CRC_COVER_LENGTH 10U

#define TCP_ACK_STATUS_SUCCESS 0x00U

/* Message 提供统一业务数据；TCP V5 遥测固定占 16 字节。 */
uint8_t tcp_frame_encode(uint8_t frame[TCP_FRAME_LENGTH],
                         const telemetry_sample_struct *message);

/*
    此函数只校验一条已经从 TCP 字节流中取出的完整 ACK 帧。
    它不读取 USART，也不写 Flash；只有 ack_status 为 0x00 时才返回成功。
*/
uint8_t tcp_ack_decode(const uint8_t frame[TCP_FRAME_LENGTH],
                       uint8_t expected_node_id,
                       uint32_t *sequence,
                       uint8_t *ack_status);

#endif
