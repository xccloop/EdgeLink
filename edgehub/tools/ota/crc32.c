#include "crc32.h"

#include <stddef.h>

#define CRC32_INIT_VALUE        0xFFFFFFFFUL
#define CRC32_FINAL_XOR_VALUE   0xFFFFFFFFUL
#define CRC32_REFLECTED_POLY    0xEDB88320UL

uint32_t crc32_begin(void)
{
    return CRC32_INIT_VALUE;
}

uint32_t crc32_update(uint32_t crc, const uint8_t *data, uint32_t length)
{
    uint32_t byte_index;
    uint32_t bit_index;

    if (data == NULL)
    {
        return crc;
    }

    for (byte_index = 0U; byte_index < length; byte_index++)
    {
        crc ^= data[byte_index];

        for (bit_index = 0U; bit_index < 8U; bit_index++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (crc >> 1U) ^ CRC32_REFLECTED_POLY;
            }
            else
            {
                crc >>= 1U;
            }
        }
    }

    return crc;
}

uint32_t crc32_finish(uint32_t crc)
{
    return crc ^ CRC32_FINAL_XOR_VALUE;
}

uint32_t crc32_generate(const uint8_t *data, uint32_t length)
{
    if (data == NULL)
    {
        return 0U;
    }

    /* 一次性版本就是分块版本的特例，两条路永远算出同一个值。 */
    return crc32_finish(crc32_update(crc32_begin(), data, length));
}

uint8_t crc32_check(const uint8_t *data,
                    uint32_t length,
                    uint32_t expected_crc)
{
    if (data == NULL)
    {
        return 0U;
    }

    return (crc32_generate(data, length) == expected_crc) ? 1U : 0U;
}
