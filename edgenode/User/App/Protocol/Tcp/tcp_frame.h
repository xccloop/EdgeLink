#ifndef TCP_FRAME_H_
#define TCP_FRAME_H_

#include <stdint.h>
#include "Model/message.h"

/* Message提供统一业务数据；当前TCP V4协议只编码其中的温度和倍率。 */
uint8_t tcp_frame_encode(uint8_t frame[16], const telemetry_sample_struct *message);

#endif
