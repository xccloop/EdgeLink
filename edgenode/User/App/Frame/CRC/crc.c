#include "crc.h"
#include <stddef.h>
#include <stdint.h>
/*
    这个文件我们提供CRC校验函数
*/

uint32_t crc32_generate(const uint8_t* data, int len)
{
    if(data == NULL)
    {
        return 0;
    }

    uint32_t crc = 0xFFFFFFFF;
    for(int i = 0; i < len; ++i)
    {
        crc ^= data[i];
        for(int j = 0; j < 8; ++j)
        {
            if(crc & 1)
            {
                crc = (crc >> 1) ^ 0xEDB88320;
            }
            else
            {
                crc >>= 1;
            }
        }
    }

    return crc ^ 0xFFFFFFFF;
}

//校验的含义为：对于用过的结果再次进行CRC计算，得出的结果是否跟发送的CRC校验一致
//用数学方式来说：实际上等价于一边得到结果然后传给另一边，对方再计算一遍看是否校验一致
uint8_t crc32_check(const uint8_t* data, int len, uint32_t expected_crc)
{
    //修复：空指针不是一段可以校验的数据，否则expected_crc刚好为0时可能被误判成校验成功
    if(data == NULL)
    {

        return 0U;
    }

    return crc32_generate(data, len) == expected_crc;
}