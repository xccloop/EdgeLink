#include "CRC.hpp"
#include <cerrno>

//这个文件是用于CRC校验的文件，这个文件需要提供两个方法
//第一个CRC加密，第二个CRC校验
//这两步写出来就可以了，不用写多

//先尝试对一段字节进行CRC校验和计算

//注意：这种方式是获取的，因为在计算时传输到网络中，字节序可能会发生改变，所以需要规定双方是大端/小端
//htons -> 小端转网络大端  htonl -> 主机转网络大端 实际使用过程中需要注意双方需要用相同的函数
uint32_t crc32_generate(const uint8_t* data, size_t len)
{
    if(data == nullptr)
    {
        errno = EINVAL;
        return 0;
    }

    uint32_t crc = 0xFFFFFFFF;
    for(size_t i = 0; i < len; ++i)
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
bool crc32_check(const uint8_t* data, size_t len, uint32_t expected_crc)
{
    //修复：空指针不是一段可以校验的数据，否则expected_crc刚好为0时可能被误判成校验成功
    if(data == nullptr)
    {
        errno = EINVAL;
        return false;
    }

    return crc32_generate(data, len) == expected_crc;
}
