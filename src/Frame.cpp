#include "Frame.hpp"
#include "CRC.hpp"
#include "Ringbuffer.hpp"
#include <cstdint>
#include <cstring>

//我们先来细想一下数据到来edgehub的时候我，我们来解析数据是什么样子的
//假设数据到来（data），首先我们判断data数据是否满足要求，也就是是否满足我们的帧协议，然后我们把数据存储到结构体frmae，后续的拿数据操作我们就通过frame来进行操作
//更正，我们的帧协议同时又数据有效性和业务有效性，我们在数组层面判断数据有效性而在结构体层面判断业务有效性

//我们要澄清一件事：tcp会把每次到来的一字节存储进入ringbuffer，但是他不会知道我们的帧协议是什么样子的，他知道自己发哦是那个一个0x48，但他不知道这个数据是什么意思
//所以当数据存储进ringbuffer里面的时候，我们的第一步是要去看ringbuffer里面有没有我们定义的数据协议帧，怎么判断呢，我们知道ringbuffer满足先进先出的概念
//因此我们可以从开头看到结尾，一直看然后要是满足魔数头并且满足完整帧长度就可以读取这个数据，所以这里面有一个关键的点--ringbbfer要支持我们看数据而不是只能拿数据

namespace
{
constexpr uint8_t FRAME_MAGIC_FIRST = 0x45;
constexpr uint8_t FRAME_MAGIC_SECOND = 0x48;
constexpr uint8_t FRAME_VERSION = 0x01;

//这个函数只负责判断Type是不是当前协议已经定义的四种类型
bool frame_type_valid(uint8_t type)
{
    return type == Telemetry
        || type == Command
        || type == Ack
        || type == Heartbeat;
}

//Telemetry V1的payload格式固定为6字节，其他类型当前只使用公共的最大长度限制，等协议定义完成以后再增加各自规则
bool frame_payload_length_valid(uint8_t type,uint8_t payload_length)
{
    if(type == Telemetry)
    {
        return payload_length == FRAME_MAX_PAYLOAD;
    }
    return true;
}

//Command是EdgeHub发给节点的方向，其他三种当前都是节点发给EdgeHub，所以节点号需要根据Type分别判断
bool frame_node_valid(uint8_t type,uint8_t source_node,uint8_t target_node)
{
    if(type == Command)
    {
        return source_node == 0 && target_node != 0;
    }
    return source_node != 0 && target_node == 0;
}

//CRC32在网络帧里面固定使用大端存放，所以这里把连续的四个字节重新组合成主机中使用的uint32_t
uint32_t frame_crc_read(const uint8_t data[],unsigned int crc_offset)
{
    return (static_cast<uint32_t>(data[crc_offset]) << 24)
        | (static_cast<uint32_t>(data[crc_offset + 1]) << 16)
        | (static_cast<uint32_t>(data[crc_offset + 2]) << 8)
        | static_cast<uint32_t>(data[crc_offset + 3]);
}
}

int frame_parser(Ringbuffer *ringbuffer,Frame *frame)
{
    //修复：指针为空时不能继续访问对象，否则会在data_space或者写Frame时直接崩溃
    if(ringbuffer == nullptr || frame == nullptr)
    {
        return FRAME_PARSE_ERROR;
    }

    //修复：Ringbuffer小于最大帧长时可能在一帧收齐以前就永久写满，所以解析器一开始就拒绝不满足协议容量的缓冲区
    int ringbuffer_capacity = ringbuffer->data_space() + ringbuffer->free_space();
    if(ringbuffer_capacity < static_cast<int>(FRAME_MAX_LENGTH))
    {
        return FRAME_PARSE_ERROR;
    }

    while(true)
    {
        int current_data_length = ringbuffer->data_space();
        if(current_data_length < 2)
        {
            //当前连Magic需要的两个字节都没有收齐，这属于正常半包，需要等待下一次数据到来
            return FRAME_PARSE_PENDING;
        }

        unsigned int magic_offset = 0;
        //修复：Magic需要同时查看两个字节，所以循环必须保证magic_offset+1仍然在有效数据里面
        while(magic_offset + 1 < static_cast<unsigned int>(current_data_length))
        {
            int first_data = ringbuffer->see(magic_offset);
            int second_data = ringbuffer->see(magic_offset + 1);
            if(first_data == FRAME_MAGIC_FIRST && second_data == FRAME_MAGIC_SECOND)
            {
                break;
            }
            magic_offset++;
        }

        if(magic_offset + 1 >= static_cast<unsigned int>(current_data_length))
        {
            //修复：完全找不到Magic时可以丢弃垃圾，但是最后一个0x45要保留，因为下一批数据的第一个字节可能正好是0x48
            unsigned int keep_length = ringbuffer->see(static_cast<unsigned int>(current_data_length - 1)) == FRAME_MAGIC_FIRST ? 1U : 0U;
            ringbuffer->discard(static_cast<unsigned int>(current_data_length) - keep_length);
            return FRAME_PARSE_PENDING;
        }

        if(magic_offset > 0)
        {
            //修复：Magic前面的数据已经确定不是协议帧，及时丢弃避免每次调用都重复扫描并最终占满Ringbuffer
            ringbuffer->discard(magic_offset);
            current_data_length = ringbuffer->data_space();
        }

        if(current_data_length < static_cast<int>(FRAME_HEADER_LENGTH))
        {
            //PayloadLength在帧头最后一个字节，固定9字节帧头没收齐以前不能读取它
            return FRAME_PARSE_PENDING;
        }

        int payload_length_data = ringbuffer->see(8);
        if(payload_length_data < 0)
        {
            return FRAME_PARSE_ERROR;
        }

        uint8_t payload_length = static_cast<uint8_t>(payload_length_data);
        int version_data = ringbuffer->see(2);
        int type_data = ringbuffer->see(3);
        int source_node_data = ringbuffer->see(4);
        int target_node_data = ringbuffer->see(5);
        if(version_data < 0 || type_data < 0 || source_node_data < 0 || target_node_data < 0)
        {
            return FRAME_PARSE_ERROR;
        }

        uint8_t type = static_cast<uint8_t>(type_data);
        if(payload_length > FRAME_MAX_PAYLOAD
            || version_data != FRAME_VERSION
            || frame_type_valid(type) == false
            || frame_payload_length_valid(type,payload_length) == false
            || frame_node_valid(
                type,
                static_cast<uint8_t>(source_node_data),
                static_cast<uint8_t>(target_node_data)) == false)
        {
            //修复：固定帧头收齐以后要先验证字段再等待整帧，否则伪Magic可能用一个假长度把后面的真帧一直堵住
            ringbuffer->discard(1);
            continue;
        }

        unsigned int frame_length = FRAME_HEADER_LENGTH + payload_length + FRAME_CRC_LENGTH;
        if(static_cast<unsigned int>(current_data_length) < frame_length)
        {
            //已经知道这一帧应该有多长，但是TCP数据还没有收齐，这时候不能移动读指针
            return FRAME_PARSE_PENDING;
        }

        uint8_t candidate[FRAME_MAX_LENGTH]{};
        for(unsigned int i = 0; i < frame_length; ++i)
        {
            int current_data = ringbuffer->see(i);
            if(current_data < 0)
            {
                return FRAME_PARSE_ERROR;
            }
            candidate[i] = static_cast<uint8_t>(current_data);
        }

        unsigned int crc_offset = FRAME_HEADER_LENGTH + payload_length;
        uint32_t expected_crc = frame_crc_read(candidate,crc_offset);
        //CRC计算范围从Version开始直到Payload结束，所以跳过前面两个Magic并且不包含最后四个CRC字节
        bool crc_valid = crc32_check(candidate + 2,FRAME_HEADER_LENGTH - 2 + payload_length,expected_crc);
        if(crc_valid == false)
        {
            //修复：CRC错误时不能丢掉整个候选帧，否则可能把后面的真Magic一起丢掉，只丢一个字节重新搜索
            ringbuffer->discard(1);
            continue;
        }

        //修复：所有字段和CRC都通过以后才写入临时Frame，避免失败候选把调用方原来的Frame改成半成品
        Frame parsed_frame{};
        parsed_frame.header.magic[0] = candidate[0];
        parsed_frame.header.magic[1] = candidate[1];
        parsed_frame.header.version = candidate[2];
        parsed_frame.header.type = candidate[3];
        parsed_frame.header.sourceNode = candidate[4];
        parsed_frame.header.targetNode = candidate[5];
        //Sequence在网络帧中固定使用大端，两个字节都需要组合，不能只读取第一个字节
        parsed_frame.header.sequence = (static_cast<uint16_t>(candidate[6]) << 8)
            | static_cast<uint16_t>(candidate[7]);
        parsed_frame.header.payloadLength = payload_length;
        if(payload_length > 0)
        {
            std::memcpy(parsed_frame.payload,candidate + FRAME_HEADER_LENGTH,payload_length);
        }
        parsed_frame.crc32 = expected_crc;

        *frame = parsed_frame;
        //修复：成功以后消费完整一帧，下一次调用才能继续解析粘在后面的第二帧
        ringbuffer->discard(frame_length);
        return FRAME_PARSE_SUCCESS;
    }
}
