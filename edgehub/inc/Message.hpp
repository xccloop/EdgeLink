#pragma once

#include <cstdint>

struct TcpFrame;

struct Message
{
    uint8_t nodeId;        // 来自 sourceNode
    uint32_t sequence;     // 节点生成的递增序号，与 nodeId 一起作为去重键
    int16_t temperature;
    int8_t temperatureScale;
    int64_t receivedAtUs;  // EdgeHub 收到且校验成功的时间
};

//调用方必须只传入tcp_frame_parser已经成功校验过的TcpFrame。
int Message_handle(Message *message,const TcpFrame *frame);
