#ifndef CAN_FRAME_H_
#define CAN_FRAME_H_

#include <stdint.h>
#include "Model/message.h"

#define CAN_FRAME_SUCCESS  1U
#define CAN_FRAME_FAIL     0U

#define CAN_TELEMETRY_BASE_ID  0x280U
#define CAN_TELEMETRY_LENGTH   7U
#define CAN_ACK_BASE_ID        0x300U
#define CAN_ACK_LENGTH         5U

/*
    这里只定义CAN遥测协议，不访问CAN硬件。
    Message提供统一业务数据；编码结果交给Output/Can中的发送出口写入CAN BSP。
*/
uint8_t can_frame_encode(uint8_t data[CAN_TELEMETRY_LENGTH],
                         const telemetry_sample_struct *message);

/*
    仅解析标准 CAN ACK 帧。CAN 控制器已完成帧级 CRC 校验；此处继续校验
    仲裁 ID、DLC 和目标节点，避免错误节点的 ACK 确认本节点记录。
*/
uint8_t can_ack_decode(uint16_t standard_id,
                       const uint8_t data[CAN_ACK_LENGTH],
                       uint8_t data_length,
                       uint8_t expected_node_id,
                       uint32_t *sequence,
                       uint8_t *ack_status);

#endif
