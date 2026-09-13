#ifndef CRC_H_
#define CRC_H_

#include <stdint.h>

// 生成 CRC-32 校验值。
uint32_t crc32_generate(const uint8_t* data, int len);

// 校验：返回 true 表示校验通过。
uint8_t crc32_check(const uint8_t* data, int len, uint32_t crc);

#endif