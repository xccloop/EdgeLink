#include "CRC.hpp"
#include <cstddef>
#include <cerrno>

//这个文件是用于CRC校验的文件，这个文件需要提供两个方法
//第一个CRC加密，第二个CRC校验
//因此直接写两个函数就可以，不用写类了

//我们先尝试对任意字节进行CRC校验和计算

//注意这种方式是可取的，但是在计算机传输到过程中，字节序可能会发生改变，因此我们要规定双方采用大端序/小端序
//htos -> 小端序转化大端序 htonl -> 大端序转化小端序，实际使用过程中需要注意数据要加同样的函数
uint32_t crc32_generate(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        errno = EINVAL;
        return 0;
    }

    uint32_t crc = 0xFFFFFFFF; //人为创造一个32位的crc寄存器，后续的操作其实就是对这个寄存器操作
    for (size_t i = 0; i < len; ++i)//对数据进行处理
     {
        crc ^= data[i]; //与数据异或
        for (int j = 0; j < 8; ++j) 
        //现在我们得到了数据数组中的某一段数据，我们对他进行处理，强制是8是因为我们的数组类型位uint8，如果是int得提前转化，CRC仅支持8位
        {
            if (crc & 1)//这里是按位与，与的规则是有0变0，这一步相当于排除前面31位，仅仅只关注最后一位是否为1或者0，如果是1，右移然后异或，如果不是直接异或
                crc = (crc >> 1) ^ 0xEDB88320;
            else
                crc >>= 1;
        }
        //至于为什么要用这种算式，懒得学明白了
    }

    return crc ^ 0xFFFFFFFF;  // 最终异或
}

//校验的做法为，将得到的结果再次进行CRC计算，看得出的结果是否与我们发送的CRC校验码一致
//这是数学公式决定，说白了就是先计算一边得到结果，然后传给对方，对方再计算一边看是否与校验码相等
bool crc32_verify(const uint8_t* data, size_t len, uint32_t expected_crc) {
    return crc32_generate(data, len) == expected_crc;
}