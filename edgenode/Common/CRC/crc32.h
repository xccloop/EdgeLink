#ifndef COMMON_CRC32_H_
#define COMMON_CRC32_H_

#include <stdint.h>

uint32_t crc32_generate(const uint8_t *data, uint32_t length);

uint8_t crc32_check(const uint8_t *data,
                    uint32_t length,
                    uint32_t expected_crc);

#endif
