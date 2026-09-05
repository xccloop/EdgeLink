#pragma once

#include "Ringbuffer.hpp"
#include <cstddef>
#include <cstdint>

//TCP连接只承载固定格式的Telemetry，不再携带Type和可变PayloadLength。
constexpr uint8_t FRAME_VERSION = 0x04;
constexpr unsigned int FRAME_HEADER_LENGTH = 7;
constexpr unsigned int FRAME_TEMPERATURE_LENGTH = 4;
constexpr unsigned int FRAME_TEMPERATURE_SCALE_LENGTH = 1;
constexpr unsigned int FRAME_TELEMETRY_LENGTH = FRAME_TEMPERATURE_LENGTH
    + FRAME_TEMPERATURE_SCALE_LENGTH;
constexpr unsigned int FRAME_CRC_LENGTH = 4;
constexpr unsigned int FRAME_LENGTH = FRAME_HEADER_LENGTH + FRAME_TELEMETRY_LENGTH + FRAME_CRC_LENGTH;

constexpr int FRAME_PARSE_ERROR = -1;
constexpr int FRAME_PARSE_PENDING = 0;
constexpr int FRAME_PARSE_SUCCESS = 1;

/*
    Magic      固定 0x45 0x48，即 EH，用于在错误数据中寻找帧头
    Version    V4 固定 0x04；V1、V2、V3都是旧布局，不能混用
    SourceNode 发送节点号，第一版用 1 表示采集节点
    TargetNode 固定为 0，表示 EdgeHub
    Sequence   发送方递增，用于日志、去重和排错
    Temperature      int32，大端
    TemperatureScale int8，实际温度 = Temperature * 10^TemperatureScale
    CRC32      大端，计算范围从 Version 到 TemperatureScale，不包括 Magic 和 CRC 自身
*/

//帧头结构固定长度为7字节，无填充。
#pragma pack(push, 1)
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

//把TCP字节流中的完整固定Telemetry帧解析出来。
int tcp_frame_parser(Ringbuffer *ringbuffer,TcpFrame *frame);

static_assert(sizeof(TcpFrameHeader) == FRAME_HEADER_LENGTH, "TcpFrameHeader must be 7 bytes");
