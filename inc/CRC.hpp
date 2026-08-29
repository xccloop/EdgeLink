#include <iostream>

// 生成 CRC 校验码
uint32_t crc32_generate(const uint8_t* data, size_t len);

// 校验：返回 true 表示校验通过
bool crc32_check(const uint8_t* data, size_t len, uint32_t crc);