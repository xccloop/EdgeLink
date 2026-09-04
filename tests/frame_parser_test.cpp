#include "CRC.hpp"
#include "Frame.hpp"
#include "Message.hpp"
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

void write_i32_be(uint8_t output[],unsigned int offset,int32_t value)
{
    uint32_t raw = static_cast<uint32_t>(value);
    output[offset] = static_cast<uint8_t>(raw >> 24);
    output[offset + 1] = static_cast<uint8_t>(raw >> 16);
    output[offset + 2] = static_cast<uint8_t>(raw >> 8);
    output[offset + 3] = static_cast<uint8_t>(raw);
}

void make_frame(uint8_t output[],uint16_t sequence,int32_t temperature,int8_t temperature_scale,
    int32_t pressure,int8_t pressure_scale,uint8_t source_node = 0x01,uint8_t target_node = 0x00)
{
    output[0] = 0x45;
    output[1] = 0x48;
    output[2] = FRAME_VERSION;
    output[3] = source_node;
    output[4] = target_node;
    output[5] = static_cast<uint8_t>(sequence >> 8);
    output[6] = static_cast<uint8_t>(sequence);

    constexpr unsigned int temperature_offset = FRAME_HEADER_LENGTH;
    constexpr unsigned int temperature_scale_offset = temperature_offset + FRAME_TEMPERATURE_LENGTH;
    constexpr unsigned int pressure_offset = temperature_scale_offset + FRAME_TEMPERATURE_SCALE_LENGTH;
    constexpr unsigned int pressure_scale_offset = pressure_offset + FRAME_PRESSURE_LENGTH;
    constexpr unsigned int crc_offset = pressure_scale_offset + FRAME_PRESSURE_SCALE_LENGTH;
    write_i32_be(output,temperature_offset,temperature);
    output[temperature_scale_offset] = static_cast<uint8_t>(temperature_scale);
    write_i32_be(output,pressure_offset,pressure);
    output[pressure_scale_offset] = static_cast<uint8_t>(pressure_scale);

    uint32_t crc = crc32_generate(output + 2,FRAME_HEADER_LENGTH - 2 + FRAME_TELEMETRY_LENGTH);
    output[crc_offset] = static_cast<uint8_t>(crc >> 24);
    output[crc_offset + 1] = static_cast<uint8_t>(crc >> 16);
    output[crc_offset + 2] = static_cast<uint8_t>(crc >> 8);
    output[crc_offset + 3] = static_cast<uint8_t>(crc);
}
}

int main()
{
    const uint8_t crc_vector[]{'1','2','3','4','5','6','7','8','9'};
    if(check(crc32_generate(crc_vector,sizeof(crc_vector)) == 0xCBF43926,"CRC32 known vector")) return 1;
    if(check(FRAME_LENGTH == 21,"fixed frame length")) return 1;

    uint8_t telemetry[FRAME_LENGTH]{};
    make_frame(telemetry,0x1234,2534,-2,101325,0);

    Ringbuffer partial_ringbuffer(64);
    Frame frame{};
    if(check(partial_ringbuffer.write(telemetry,10) == 10,"partial write first")) return 1;
    if(check(frame_parser(&partial_ringbuffer,&frame) == FRAME_PARSE_PENDING,"partial remains pending")) return 1;
    if(check(partial_ringbuffer.data_space() == 10,"partial bytes retained")) return 1;
    if(check(partial_ringbuffer.write(telemetry + 10,FRAME_LENGTH - 10)
        == static_cast<int>(FRAME_LENGTH - 10),"partial write second")) return 1;
    if(check(frame_parser(&partial_ringbuffer,&frame) == FRAME_PARSE_SUCCESS,"partial completes")) return 1;
    if(check(frame.header.sequence == 0x1234,"header decoded")) return 1;
    if(check(frame.temperature == 2534 && frame.temperatureScale == -2,"temperature decoded")) return 1;
    if(check(frame.pressure == 101325 && frame.pressureScale == 0,"pressure decoded")) return 1;
    if(check(frame.receivedAtUs > 0,"receive timestamp recorded")) return 1;
    if(check(partial_ringbuffer.data_space() == 0,"successful frame consumed")) return 1;

    Message message{};
    if(check(Message_handle(&message,&frame) == 0,"message conversion succeeds")) return 1;
    if(check(message.nodeId == 0x01 && message.sequence == 0x1234,"message header copied")) return 1;
    if(check(message.temperature == 2534 && message.temperatureScale == -2
        && message.pressure == 101325 && message.pressureScale == 0,"message telemetry copied")) return 1;
    if(check(message.receivedAtUs == frame.receivedAtUs,"message receive time copied")) return 1;
    if(check(Message_handle(nullptr,&frame) == -1 && Message_handle(&message,nullptr) == -1,
        "message null rejected")) return 1;

    uint8_t second_telemetry[FRAME_LENGTH]{};
    make_frame(second_telemetry,0x1235,-500,-128,100000,-2);
    uint8_t sticky[FRAME_LENGTH * 2]{};
    std::memcpy(sticky,telemetry,FRAME_LENGTH);
    std::memcpy(sticky + FRAME_LENGTH,second_telemetry,FRAME_LENGTH);
    Ringbuffer sticky_ringbuffer(64);
    sticky_ringbuffer.write(sticky,sizeof(sticky));
    if(check(frame_parser(&sticky_ringbuffer,&frame) == FRAME_PARSE_SUCCESS
        && frame.header.sequence == 0x1234,"sticky first frame")) return 1;
    if(check(frame_parser(&sticky_ringbuffer,&frame) == FRAME_PARSE_SUCCESS
        && frame.header.sequence == 0x1235 && frame.temperature == -500
        && frame.temperatureScale == -128 && frame.pressureScale == -2,"signed values decoded")) return 1;
    if(check(sticky_ringbuffer.data_space() == 0,"sticky frames consumed")) return 1;

    uint8_t corrupted[FRAME_LENGTH]{};
    std::memcpy(corrupted,telemetry,FRAME_LENGTH);
    corrupted[FRAME_LENGTH - 1] ^= 0x01;
    uint8_t crc_recovery[FRAME_LENGTH * 2]{};
    std::memcpy(crc_recovery,corrupted,FRAME_LENGTH);
    std::memcpy(crc_recovery + FRAME_LENGTH,second_telemetry,FRAME_LENGTH);
    Ringbuffer crc_recovery_ringbuffer(64);
    crc_recovery_ringbuffer.write(crc_recovery,sizeof(crc_recovery));
    if(check(frame_parser(&crc_recovery_ringbuffer,&frame) == FRAME_PARSE_SUCCESS
        && frame.header.sequence == 0x1235,"bad CRC resynchronizes")) return 1;

    uint8_t invalid_node[FRAME_LENGTH]{};
    make_frame(invalid_node,0x1236,2534,-2,101325,0,0x01,0x01);
    uint8_t node_recovery[FRAME_LENGTH * 2]{};
    std::memcpy(node_recovery,invalid_node,FRAME_LENGTH);
    std::memcpy(node_recovery + FRAME_LENGTH,second_telemetry,FRAME_LENGTH);
    Ringbuffer node_recovery_ringbuffer(64);
    node_recovery_ringbuffer.write(node_recovery,sizeof(node_recovery));
    if(check(frame_parser(&node_recovery_ringbuffer,&frame) == FRAME_PARSE_SUCCESS
        && frame.header.sequence == 0x1235,"invalid node resynchronizes")) return 1;

    uint8_t legacy_v2[17]{};
    legacy_v2[0] = 0x45;
    legacy_v2[1] = 0x48;
    legacy_v2[2] = 0x02;
    legacy_v2[3] = 0x01;
    legacy_v2[4] = 0x00;
    legacy_v2[5] = 0x12;
    legacy_v2[6] = 0x36;
    legacy_v2[7] = 0x02;
    legacy_v2[8] = 0x00;
    legacy_v2[9] = 0x00;
    legacy_v2[10] = 0x04;
    legacy_v2[11] = 0xD2;
    legacy_v2[12] = 0xFE;
    uint32_t legacy_crc = crc32_generate(legacy_v2 + 2,11);
    legacy_v2[13] = static_cast<uint8_t>(legacy_crc >> 24);
    legacy_v2[14] = static_cast<uint8_t>(legacy_crc >> 16);
    legacy_v2[15] = static_cast<uint8_t>(legacy_crc >> 8);
    legacy_v2[16] = static_cast<uint8_t>(legacy_crc);
    Ringbuffer legacy_v2_ringbuffer(64);
    legacy_v2_ringbuffer.write(legacy_v2,sizeof(legacy_v2));
    if(check(frame_parser(&legacy_v2_ringbuffer,&frame) == FRAME_PARSE_PENDING
        && legacy_v2_ringbuffer.data_space() == 0,"isolated legacy V2 rejected")) return 1;

    uint8_t version_recovery[sizeof(legacy_v2) + FRAME_LENGTH]{};
    std::memcpy(version_recovery,legacy_v2,sizeof(legacy_v2));
    std::memcpy(version_recovery + sizeof(legacy_v2),second_telemetry,FRAME_LENGTH);
    Ringbuffer version_recovery_ringbuffer(64);
    version_recovery_ringbuffer.write(version_recovery,sizeof(version_recovery));
    if(check(frame_parser(&version_recovery_ringbuffer,&frame) == FRAME_PARSE_SUCCESS
        && frame.header.sequence == 0x1235,"legacy V2 frame rejected")) return 1;

    Ringbuffer exact_ringbuffer(FRAME_LENGTH);
    exact_ringbuffer.write(telemetry,FRAME_LENGTH);
    if(check(frame_parser(&exact_ringbuffer,&frame) == FRAME_PARSE_SUCCESS
        && exact_ringbuffer.data_space() == 0,"exact full frame parses")) return 1;

    Ringbuffer discard_ringbuffer(5);
    const uint8_t discard_data[]{0x01,0x02,0x03};
    discard_ringbuffer.write(discard_data,sizeof(discard_data));
    if(check(discard_ringbuffer.discard(10) == 3 && discard_ringbuffer.data_space() == 0,"over discard is bounded")) return 1;

    Ringbuffer undersized_ringbuffer(FRAME_LENGTH - 1);
    if(check(frame_parser(&undersized_ringbuffer,&frame) == FRAME_PARSE_ERROR,"undersized ringbuffer rejected")) return 1;

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
