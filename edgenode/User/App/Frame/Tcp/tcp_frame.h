#ifndef TCP_FRAME_H_
#define TCP_FRAME_H_

#include <stdint.h>

uint8_t tcp_frame_encode(uint8_t frame[16],uint16_t sequence, int32_t temperature, int8_t temperature_scale);

#endif