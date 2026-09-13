#include "tcp_frame.h"
#include "Config/config.h"
#include "Protocol/CRC/crc.h"

/*
    这个文件是用于定义TCP协议帧的文件并且提供TCP帧协议解析，协议内容与要edgehub定义一致
    与edgehub不同我们在这里定义的是frame的封装而不是frame的解析
    其实可以注意到我们并没有使用结构体来定义一个帧，是因为我们是发送方
    所以结构体其实没必要，我们只需要将数据给装进frame里面
    然后发送就好，发送交给esp12s，我们来做封装
*/

/*
    以下为edgehub的帧协议
    struct TcpFrameHeader
    {
        uint8_t  magic[2];
        uint8_t  version;
        uint8_t  sourceNode;
        uint8_t  targetNode;
        uint16_t sequence;
    };
    #pragma pack(pop)

    //完整TCP遥测帧；它是程序内对象，不可直接把它的内存发送到 TCP。
    struct TcpFrame
    {
        TcpFrameHeader header;
        int32_t temperature;
        int8_t temperatureScale;
        uint32_t crc32;
        int64_t receivedAtUs;    // EdgeHub完成完整帧校验时记录的Unix微秒时间戳，不属于TCP线上字节
    };
*/

uint8_t tcp_frame_encode(uint8_t frame[16], uint16_t sequence, const telemetry_sample_struct *message)
{
    uint32_t crc;
    if((frame == 0) || (message == 0))
    {
        return 0U;
    }
    frame[0] = 0x45U;
    frame[1] = 0x48U;
    frame[2] = 0x04U;
    frame[3] = board_id;
    frame[4] = 0U;
    frame[5] = (uint8_t)(sequence >> 8);
    frame[6] = (uint8_t)sequence;
    frame[7]  = (uint8_t)((uint32_t)message->temperature >> 24);
    frame[8]  = (uint8_t)((uint32_t)message->temperature >> 16);
    frame[9]  = (uint8_t)((uint32_t)message->temperature >> 8);
    frame[10] = (uint8_t)message->temperature;
    frame[11] = (uint8_t)message->temperature_scale;
    crc = crc32_generate(&frame[2], 10);
    frame[12] = (uint8_t)(crc >> 24);
    frame[13] = (uint8_t)(crc >> 16);
    frame[14] = (uint8_t)(crc >> 8);
    frame[15] = (uint8_t)crc;
    return 1U;
}
