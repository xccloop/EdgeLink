#include "CRC.hpp"
#include "Frame.hpp"
#include "Ringbuffer.hpp"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace
{
int check(bool condition,const char *name)
{
    if(condition == false)
    {
        std::fprintf(stderr,"FAIL: %s\n",name);
        return 1;
    }
    return 0;
}

unsigned int make_frame(
    uint8_t output[],
    uint8_t type,
    uint16_t sequence,
    const uint8_t payload[],
    uint8_t payload_length,
    uint8_t source_node = 0x01,
    uint8_t target_node = 0x00)
{
    output[0] = 0x45;
    output[1] = 0x48;
    output[2] = 0x01;
    output[3] = type;
    output[4] = source_node;
    output[5] = target_node;
    output[6] = static_cast<uint8_t>(sequence >> 8);
    output[7] = static_cast<uint8_t>(sequence);
    output[8] = payload_length;
    if(payload_length > 0)
    {
        std::memcpy(output + FRAME_HEADER_LENGTH,payload,payload_length);
    }

    unsigned int crc_offset = FRAME_HEADER_LENGTH + payload_length;
    uint32_t crc = crc32_generate(output + 2,FRAME_HEADER_LENGTH - 2 + payload_length);
    output[crc_offset] = static_cast<uint8_t>(crc >> 24);
    output[crc_offset + 1] = static_cast<uint8_t>(crc >> 16);
    output[crc_offset + 2] = static_cast<uint8_t>(crc >> 8);
    output[crc_offset + 3] = static_cast<uint8_t>(crc);
    return crc_offset + FRAME_CRC_LENGTH;
}
}

int main()
{
    const uint8_t crc_vector[]{'1','2','3','4','5','6','7','8','9'};
    if(check(crc32_generate(crc_vector,sizeof(crc_vector)) == 0xCBF43926,"CRC32 known vector")) return 1;

    const uint8_t telemetry_payload[]{0x02,0x00,0x00,0x04,0xD2,0xFF};
    uint8_t telemetry[FRAME_MAX_LENGTH]{};
    unsigned int telemetry_length = make_frame(telemetry,Telemetry,0x1234,telemetry_payload,sizeof(telemetry_payload));

    Ringbuffer partial_ringbuffer(64);
    Frame frame{};
    if(check(partial_ringbuffer.write(telemetry,8) == 8,"partial write first")) return 1;
    if(check(frame_parser(&partial_ringbuffer,&frame) == FRAME_PARSE_PENDING,"partial remains pending")) return 1;
    if(check(partial_ringbuffer.data_space() == 8,"partial bytes retained")) return 1;
    if(check(partial_ringbuffer.write(telemetry + 8,telemetry_length - 8) == static_cast<int>(telemetry_length - 8),"partial write second")) return 1;
    if(check(frame_parser(&partial_ringbuffer,&frame) == FRAME_PARSE_SUCCESS,"partial completes")) return 1;
    if(check(frame.header.sequence == 0x1234 && frame.header.payloadLength == 6,"header decoded")) return 1;
    if(check(std::memcmp(frame.payload,telemetry_payload,sizeof(telemetry_payload)) == 0,"payload decoded")) return 1;
    if(check(partial_ringbuffer.data_space() == 0,"successful frame consumed")) return 1;

    const uint8_t heartbeat_payload[]{0x00};
    uint8_t heartbeat[FRAME_MAX_LENGTH]{};
    unsigned int heartbeat_length = make_frame(heartbeat,Heartbeat,0x1235,heartbeat_payload,0);
    uint8_t sticky[FRAME_MAX_LENGTH * 2]{};
    std::memcpy(sticky,telemetry,telemetry_length);
    std::memcpy(sticky + telemetry_length,heartbeat,heartbeat_length);
    Ringbuffer sticky_ringbuffer(64);
    sticky_ringbuffer.write(sticky,telemetry_length + heartbeat_length);
    if(check(frame_parser(&sticky_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.sequence == 0x1234,"sticky first frame")) return 1;
    if(check(frame_parser(&sticky_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.sequence == 0x1235,"sticky second frame")) return 1;
    if(check(sticky_ringbuffer.data_space() == 0,"sticky frames consumed")) return 1;

    uint8_t corrupted[FRAME_MAX_LENGTH]{};
    std::memcpy(corrupted,telemetry,telemetry_length);
    corrupted[2] = 0x02;
    uint8_t recovery[FRAME_MAX_LENGTH * 2 + 3]{};
    recovery[0] = 0xAA;
    recovery[1] = 0x45;
    recovery[2] = 0x00;
    std::memcpy(recovery + 3,corrupted,telemetry_length);
    std::memcpy(recovery + 3 + telemetry_length,heartbeat,heartbeat_length);
    Ringbuffer recovery_ringbuffer(64);
    recovery_ringbuffer.write(recovery,3 + telemetry_length + heartbeat_length);
    if(check(frame_parser(&recovery_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.sequence == 0x1235,"invalid candidate resynchronizes")) return 1;
    if(check(recovery_ringbuffer.data_space() == 0,"recovery consumes valid frame")) return 1;

    uint8_t bad_crc[FRAME_MAX_LENGTH]{};
    std::memcpy(bad_crc,telemetry,telemetry_length);
    bad_crc[telemetry_length - 1] ^= 0x01;
    uint8_t crc_recovery[FRAME_MAX_LENGTH * 2]{};
    std::memcpy(crc_recovery,bad_crc,telemetry_length);
    std::memcpy(crc_recovery + telemetry_length,heartbeat,heartbeat_length);
    Ringbuffer crc_recovery_ringbuffer(64);
    crc_recovery_ringbuffer.write(crc_recovery,telemetry_length + heartbeat_length);
    if(check(frame_parser(&crc_recovery_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.sequence == 0x1235,"bad CRC resynchronizes")) return 1;

    uint8_t short_telemetry[FRAME_MAX_LENGTH]{};
    unsigned int short_telemetry_length = make_frame(short_telemetry,Telemetry,0x1236,heartbeat_payload,0);
    uint8_t length_recovery[FRAME_MAX_LENGTH * 2]{};
    std::memcpy(length_recovery,short_telemetry,short_telemetry_length);
    std::memcpy(length_recovery + short_telemetry_length,heartbeat,heartbeat_length);
    Ringbuffer length_recovery_ringbuffer(64);
    length_recovery_ringbuffer.write(length_recovery,short_telemetry_length + heartbeat_length);
    if(check(frame_parser(&length_recovery_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.sequence == 0x1235,"short telemetry rejected")) return 1;

    Ringbuffer wrap_ringbuffer(24);
    uint8_t padding[20]{};
    uint8_t padding_output[20]{};
    wrap_ringbuffer.write(padding,sizeof(padding));
    wrap_ringbuffer.read(padding_output,sizeof(padding_output));
    wrap_ringbuffer.write(telemetry,telemetry_length);
    if(check(frame_parser(&wrap_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.sequence == 0x1234,"wrapped frame parses")) return 1;
    if(check(wrap_ringbuffer.data_space() == 0,"wrapped frame consumed")) return 1;

    Ringbuffer exact_ringbuffer(FRAME_MAX_LENGTH);
    exact_ringbuffer.write(telemetry,telemetry_length);
    if(check(frame_parser(&exact_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && exact_ringbuffer.data_space() == 0,"exact full frame parses")) return 1;

    Ringbuffer discard_ringbuffer(5);
    const uint8_t discard_data[]{0x01,0x02,0x03};
    discard_ringbuffer.write(discard_data,sizeof(discard_data));
    if(check(discard_ringbuffer.discard(10) == 3 && discard_ringbuffer.data_space() == 0,"over discard is bounded")) return 1;

    Ringbuffer undersized_ringbuffer(FRAME_MAX_LENGTH - 1);
    if(check(frame_parser(&undersized_ringbuffer,&frame) == FRAME_PARSE_ERROR,"undersized ringbuffer rejected")) return 1;

    const uint8_t command_payload[]{0x01};
    uint8_t command[FRAME_MAX_LENGTH]{};
    unsigned int command_length = make_frame(command,Command,0x1237,command_payload,sizeof(command_payload),0x00,0x01);
    Ringbuffer command_ringbuffer(64);
    command_ringbuffer.write(command,command_length);
    if(check(frame_parser(&command_ringbuffer,&frame) == FRAME_PARSE_SUCCESS && frame.header.targetNode == 0x01,"command direction accepted")) return 1;

    bool zero_capacity_rejected = false;
    try
    {
        Ringbuffer invalid_ringbuffer(0);
    }
    catch(const std::invalid_argument&)
    {
        zero_capacity_rejected = true;
    }
    if(check(zero_capacity_rejected,"zero capacity rejected")) return 1;

    if(check(frame_parser(nullptr,&frame) == FRAME_PARSE_ERROR,"null ringbuffer rejected")) return 1;
    if(check(frame_parser(&partial_ringbuffer,nullptr) == FRAME_PARSE_ERROR,"null frame rejected")) return 1;

    std::printf("frame_parser_tests=PASS\n");
    return 0;
}
