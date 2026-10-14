#ifndef COMMON_CRC32_H_
#define COMMON_CRC32_H_

#include <stdint.h>

uint32_t crc32_generate(const uint8_t *data, uint32_t length);

uint8_t crc32_check(const uint8_t *data,
                    uint32_t length,
                    uint32_t expected_crc);

/*
    分块接口：begin -> update 若干次 -> finish。
    给「数据太长、没法一次放进内存」的场景用（例如 100+ KB 的固件镜像）：
    每读一小块就 update，最后 finish 出来的值和 crc32_generate 整段算完全一样。
*/
uint32_t crc32_begin(void);
uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length);
uint32_t crc32_finish(uint32_t crc);

#endif
