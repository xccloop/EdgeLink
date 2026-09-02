#pragma once

#include "Ringbuffer.hpp"
#include <cstddef>
#include <cstdint>

//constexpr:编译期常量，比const更高效，这里是定义一个常量
constexpr uint8_t FRAME_MAX_PAYLOAD = 6;
//修复：PayloadLength固定为uint8_t以后，帧头就是9字节，完整帧长度等于帧头+payload+CRC32
constexpr unsigned int FRAME_HEADER_LENGTH = 9;
constexpr unsigned int FRAME_CRC_LENGTH = 4;
constexpr unsigned int FRAME_MAX_LENGTH = FRAME_HEADER_LENGTH + FRAME_MAX_PAYLOAD + FRAME_CRC_LENGTH;

//修复：我们把解析结果分成参数错误、等待更多数据、成功得到一帧，半包属于正常等待而不是错误
constexpr int FRAME_PARSE_ERROR = -1;
constexpr int FRAME_PARSE_PENDING = 0;
constexpr int FRAME_PARSE_SUCCESS = 1;


/*
    Magic    固定 0x45 0x48，即 EH，用于在错误数据中寻找帧头
    Version  V1 固定 0x01
    Type     消息类型
    SourceNode    发送节点号，第一版用 1 表示采集节点
    TargetNode    0 表示 EdgeHub，命令下发时是目标节点
    Sequence 发送方递增，用于 ACK、日志和排错
    PayloadLength 固定uint8_t，单字节没有大小端问题，Telemetry V1固定为6
    Payload  根据 Type 解释
    CRC32    大端，计算范围从 Version 到 Payload，不包括 Magic 和 CRC 自身
*/

/*
    Type:
    0x01  Telemetry   采集值上报
    0x02  Command     网关下发命令
    0x03  Ack         命令确认
    0x04  Heartbeat   心跳
*/
enum Type : uint8_t
{
    Telemetry = 0x01,
    Command = 0x02,
    Ack = 0x03,
    Heartbeat = 0x04
};

/*
    Payload:
    SensorId(1) | Value(int32, 4) | Scale(int8, 1)
*/

// 帧头结构固定长度为9字节，无填充。
#pragma pack(push, 1)
struct FrameHeader
{
    uint8_t  magic[2];       // 固定 {0x45, 0x48}
    uint8_t  version;        // 固定 0x01
    uint8_t  type;           // Type 枚举值
    uint8_t  sourceNode;     // 发送节点号
    uint8_t  targetNode;     // 目标节点号
    uint16_t sequence;       // 客户端发送的第几次命令
    uint8_t payloadLength;   // payload命令长度，固定为uint8_t
};
#pragma pack(pop)

// 完整帧结构；它是程序内对象，不可直接把它的内存发送到 TCP。
struct Frame
{
    FrameHeader header;
    uint8_t payload[FRAME_MAX_PAYLOAD];
    uint32_t crc32;
};

//修复：把解析函数声明放到头文件里面，这样其他文件才能通过同一个接口使用解析器
int frame_parser(Ringbuffer *ringbuffer,Frame *frame);

//修复：如果以后不小心增加字段导致帧头不是9字节，编译阶段就直接提醒我们协议布局已经变化
static_assert(sizeof(FrameHeader) == FRAME_HEADER_LENGTH, "FrameHeader must be 9 bytes");
