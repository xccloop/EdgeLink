#ifndef MESSAGE_H_
#define MESSAGE_H_

#include <stdint.h>

#define MESSAGE_SUCCESS  1U
#define MESSAGE_FAIL     0U

typedef struct
{
    uint32_t sample_uptime_ms;
    uint32_t sequence;//负责保存序列号
    int16_t temperature;
    int8_t temperature_scale;
} telemetry_sample_struct;

/* 获取一次温度采集结果，并封装为Storage、TCP Frame和CAN Frame共用的内部模型。 */
uint8_t message_collect(telemetry_sample_struct *message);

uint8_t message_sequence_init(uint32_t next_sequence);
#endif
