#pragma once

#include <cstddef>
#include <cstdint>

// 生成 CRC-32 校验值。
uint32_t crc32_generate(const uint8_t* data, size_t len);

// 校验：返回 true 表示校验通过。
bool crc32_check(const uint8_t* data, size_t len, uint32_t crc);
