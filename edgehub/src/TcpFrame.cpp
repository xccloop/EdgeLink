#include "TcpFrame.hpp"
#include "CRC.hpp"
#include "Ringbuffer.hpp"
#include <chrono>
#include <cstdint>
#include <cstring>

namespace
{
constexpr uint8_t FRAME_MAGIC_FIRST = 0x45;
constexpr uint8_t FRAME_MAGIC_SECOND = 0x48;
bool telemetry_frame_node_valid(uint8_t source_node,uint8_t target_node)
{
    return source_node >= 1U && source_node <= 127U && target_node == 0U;
}

uint32_t frame_crc_read(const uint8_t data[],unsigned int crc_offset)
{
    return (static_cast<uint32_t>(data[crc_offset]) << 24)
        | (static_cast<uint32_t>(data[crc_offset + 1]) << 16)
        | (static_cast<uint32_t>(data[crc_offset + 2]) << 8)
        | static_cast<uint32_t>(data[crc_offset + 3]);
}

uint32_t frame_u32_read(const uint8_t data[],unsigned int offset)
{
    return (static_cast<uint32_t>(data[offset]) << 24)
        | (static_cast<uint32_t>(data[offset + 1]) << 16)
        | (static_cast<uint32_t>(data[offset + 2]) << 8)
        | static_cast<uint32_t>(data[offset + 3]);
}

int16_t frame_i16_read(const uint8_t data[],unsigned int offset)
{
    uint16_t raw = (static_cast<uint16_t>(data[offset]) << 8)
        | static_cast<uint16_t>(data[offset + 1]);
    return raw <= 0x7FFFU ? static_cast<int16_t>(raw) :
        static_cast<int16_t>(static_cast<int32_t>(raw) - 0x10000L);
}

int8_t frame_i8_read(const uint8_t data[],unsigned int offset)
{
    int16_t raw = data[offset];
    if(raw <= 0x7F)
    {
        return static_cast<int8_t>(raw);
    }
    return static_cast<int8_t>(raw - 0x100);
}
}

int64_t frame_received_at_us()
{
    using namespace std::chrono;
    return duration_cast<microseconds>(system_clock::now().time_since_epoch()).count();
}

int tcp_frame_parser(Ringbuffer *ringbuffer,TcpFrame *frame)
{
    if(ringbuffer == nullptr || frame == nullptr)
    {
        return FRAME_PARSE_ERROR;
    }

    int ringbuffer_capacity = ringbuffer->data_space() + ringbuffer->free_space();
    if(ringbuffer_capacity < static_cast<int>(FRAME_LENGTH))
    {
        return FRAME_PARSE_ERROR;
    }

    while(true)
    {
        int current_data_length = ringbuffer->data_space();
        if(current_data_length < 2)
        {
            return FRAME_PARSE_PENDING;
        }

        unsigned int magic_offset = 0;
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
            unsigned int keep_length = ringbuffer->see(static_cast<unsigned int>(current_data_length - 1))
                == FRAME_MAGIC_FIRST ? 1U : 0U;
            ringbuffer->discard(static_cast<unsigned int>(current_data_length) - keep_length);
            return FRAME_PARSE_PENDING;
        }

        if(magic_offset > 0)
        {
            ringbuffer->discard(magic_offset);
            current_data_length = ringbuffer->data_space();
        }

        if(current_data_length < static_cast<int>(FRAME_HEADER_LENGTH))
        {
            return FRAME_PARSE_PENDING;
        }

        int version_data = ringbuffer->see(2);
        int source_node_data = ringbuffer->see(3);
        int target_node_data = ringbuffer->see(4);
        if(version_data < 0 || source_node_data < 0 || target_node_data < 0)
        {
            return FRAME_PARSE_ERROR;
        }

        if(version_data != FRAME_VERSION
            || telemetry_frame_node_valid(
                static_cast<uint8_t>(source_node_data),
                static_cast<uint8_t>(target_node_data)) == false)
        {
            ringbuffer->discard(1);
            continue;
        }

        if(static_cast<unsigned int>(current_data_length) < FRAME_LENGTH)
        {
            return FRAME_PARSE_PENDING;
        }

        uint8_t candidate[FRAME_LENGTH]{};
        for(unsigned int i = 0; i < FRAME_LENGTH; ++i)
        {
            int current_data = ringbuffer->see(i);
            if(current_data < 0)
            {
                return FRAME_PARSE_ERROR;
            }
            candidate[i] = static_cast<uint8_t>(current_data);
        }

        constexpr unsigned int temperature_offset = FRAME_HEADER_LENGTH;
        constexpr unsigned int temperature_scale_offset = temperature_offset + FRAME_TEMPERATURE_LENGTH;
        constexpr unsigned int crc_offset = temperature_scale_offset + FRAME_TEMPERATURE_SCALE_LENGTH;
        uint32_t expected_crc = frame_crc_read(candidate,crc_offset);
        bool crc_valid = crc32_check(candidate + 2,
            FRAME_HEADER_LENGTH - 2 + FRAME_TELEMETRY_LENGTH,expected_crc);
        if(crc_valid == false)
        {
            ringbuffer->discard(1);
            continue;
        }

        TcpFrame parsed_frame{};
        parsed_frame.header.magic[0] = candidate[0];
        parsed_frame.header.magic[1] = candidate[1];
        parsed_frame.header.version = candidate[2];
        parsed_frame.header.sourceNode = candidate[3];
        parsed_frame.header.targetNode = candidate[4];
        parsed_frame.header.sequence = frame_u32_read(candidate,5);
        parsed_frame.temperature = frame_i16_read(candidate,temperature_offset);
        parsed_frame.temperatureScale = frame_i8_read(candidate,temperature_scale_offset);
        parsed_frame.crc32 = expected_crc;
        parsed_frame.receivedAtUs = frame_received_at_us();

        *frame = parsed_frame;
        ringbuffer->discard(FRAME_LENGTH);
        return FRAME_PARSE_SUCCESS;
    }
}

bool tcp_ack_encode(uint8_t frame[FRAME_LENGTH], uint8_t node_id,
                    uint32_t sequence, uint8_t ack_status)
{
    if(frame == nullptr || node_id == 0U || node_id > 127U)
    {
        return false;
    }

    frame[0] = FRAME_MAGIC_FIRST;
    frame[1] = FRAME_MAGIC_SECOND;
    frame[2] = FRAME_VERSION;
    frame[3] = 0U;
    frame[4] = node_id;
    frame[5] = static_cast<uint8_t>(sequence >> 24);
    frame[6] = static_cast<uint8_t>(sequence >> 16);
    frame[7] = static_cast<uint8_t>(sequence >> 8);
    frame[8] = static_cast<uint8_t>(sequence);
    frame[9] = 0U;
    frame[10] = 0U;
    frame[11] = ack_status;

    uint32_t crc = crc32_generate(frame + 2, 10U);
    frame[12] = static_cast<uint8_t>(crc >> 24);
    frame[13] = static_cast<uint8_t>(crc >> 16);
    frame[14] = static_cast<uint8_t>(crc >> 8);
    frame[15] = static_cast<uint8_t>(crc);
    return true;
}

