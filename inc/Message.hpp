#pragma once

#include <cstdint>

struct Frame;

struct Message
{
    uint8_t nodeId;        // 来自 sourceNode
    uint16_t sequence;     // 用于排错、未来去重
    int32_t temperature;
    int8_t temperatureScale;
    int32_t pressure;
    int8_t pressureScale;
    int64_t receivedAtUs;  // EdgeHub 收到且校验成功的时间
};

//调用方必须只传入frame_parser已经成功校验过的Frame。
int Message_handle(Message *message,const Frame *frame);
