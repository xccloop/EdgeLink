#ifndef COLLECTION_H_
#define COLLECTION_H_

#include <stdint.h>

#define COLLECTION_SUCCESS  1U
#define COLLECTION_FAIL     0U

/*
    读取BMP280温度后，立刻记录本次读取完成时的开机毫秒数。
    temperature使用固定小数，实际温度 = temperature * 10^temperature_scale。
*/
uint8_t collection_temperature_get(int32_t *temperature,
                                   int8_t *temperature_scale,
                                   uint32_t *sample_uptime_ms);

#endif
