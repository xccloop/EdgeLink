#include "OTA.hpp"
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

static uint32_t le32(const uint8_t* p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
        ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

bool OTA::onCanFrame(const can_frame& frame, Result& result)
{
    result = Result{};
    if((frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0U)
    {
        return false;
    }

    uint32_t id = frame.can_id & CAN_SFF_MASK;
    uint32_t base;
    const char* kind;
    if(id >= CAN_OTA_DATA_BASE_ID && id < CAN_OTA_DATA_BASE_ID + CAN_ID_SEGMENT_SIZE)
    {
        base = CAN_OTA_DATA_BASE_ID;
        kind = "data";
    }
    else if(id >= CAN_OTA_QUERY_BASE_ID && id < CAN_OTA_QUERY_BASE_ID + CAN_ID_SEGMENT_SIZE)
    {
        base = CAN_OTA_QUERY_BASE_ID;
        kind = "query";
    }
    else if(id >= CAN_OTA_REPLY_BASE_ID && id < CAN_OTA_REPLY_BASE_ID + CAN_ID_SEGMENT_SIZE)
    {
        base = CAN_OTA_REPLY_BASE_ID;
        kind = "reply";
    }
    else
    {
        return false;
    }

    uint32_t node = id - base;
    // 节点 0 保留；查询帧在此只校验经典 CAN 的长度范围。
    if(node == 0U || frame.can_dlc > CAN_MAX_DLEN ||
       (base == CAN_OTA_DATA_BASE_ID && frame.can_dlc != CAN_OTA_DATA_LENGTH) ||
       (base == CAN_OTA_REPLY_BASE_ID && frame.can_dlc != CAN_OTA_REPLY_LENGTH &&
        frame.can_dlc != 8U))
    {
        fprintf(stderr, "CAN OTA invalid: id=0x%03X dlc=%u\n", id,
            static_cast<unsigned int>(frame.can_dlc));
        return true;
    }

    if(base == CAN_OTA_REPLY_BASE_ID)
    {
        if(frame.data[0] == CAN_OTA_REPLY_KIND_SLOT && frame.can_dlc == 8U)
        {
            finishSlotQuery(static_cast<uint8_t>(node), frame, result);
            return true;
        }
        if(frame.can_dlc != CAN_OTA_REPLY_LENGTH || frame.data[0] == CAN_OTA_REPLY_KIND_SLOT)
        {
            fprintf(stderr, "CAN OTA invalid reply length: node=%u kind=%u dlc=%u\n",
                node, static_cast<unsigned int>(frame.data[0]),
                static_cast<unsigned int>(frame.can_dlc));
            return true;
        }
        uint16_t contiguous = (static_cast<uint16_t>(frame.data[1]) << 8) |
            static_cast<uint16_t>(frame.data[2]);
        if(!isSending() || node != send.node) return true;
        if(frame.data[0] == CAN_OTA_REPLY_KIND_PROGRESS && frame.data[3] == 0U)
        {
            uint32_t next = static_cast<uint32_t>(contiguous) + 1U;
            // ACK 只能确认已经发出的数据，重复/迟到确认不能让窗口倒退。
            if(next <= send.acked_next || next > send.highest_sent) return true;
            printf("CAN OTA progress: node=%u contiguous=%u\n", node,
                static_cast<unsigned int>(contiguous));
            send.acked_next = static_cast<uint16_t>(next);
            if(send.next_sequence < send.acked_next) send.next_sequence = send.acked_next;
            send.retries = 0;
            send.waiting_result = send.acked_next == send.total_frames;
            send.deadline = std::chrono::steady_clock::now() +
                std::chrono::milliseconds(send.waiting_result ? 5000 : 500);
        }
        else if(frame.data[0] == CAN_OTA_REPLY_KIND_RESULT && frame.data[3] <= 1U)
        {
            if(frame.data[3] == 0U)
                finishSend(result, Error::NodeRejected, "node rejected firmware image");
            else if(send.highest_sent == send.total_frames &&
                    static_cast<uint32_t>(contiguous) + 1U == send.total_frames)
            {
                finishSend(result, Error::None, "");
                result.relay_verified = true;
            }
        }
        else
        {
            fprintf(stderr, "CAN OTA invalid reply: node=%u kind=%u success=%u\n", node,
                static_cast<unsigned int>(frame.data[0]),
                static_cast<unsigned int>(frame.data[3]));
        }
        return true;
    }

    printf("CAN OTA %s observed: node=%u dlc=%u data=", kind, node,
        static_cast<unsigned int>(frame.can_dlc));
    for(uint8_t index = 0; index < frame.can_dlc; ++index)
    {
        printf("%02X", static_cast<unsigned int>(frame.data[index]));
    }
    printf("\n");
    return true;
}



bool OTA::querySlot(uint64_t request_id, uint8_t node, Result& result)
{
    result = Result{};
    result.request_id = request_id;
    result.node = node;
    if(request_id == 0U || node == 0U || node > 127U || queries.count(request_id) != 0U)
    {
        result.error = Error::InvalidArgument;
        result.message = "invalid request or node";
        return false;
    }
    bool in_use;
    do
    {
        ++next_query_id;
        if(next_query_id == 0U) ++next_query_id;
        in_use = false;
        for(const auto& pending : queries)
        {
            if(pending.second.id == next_query_id)
            {
                in_use = true;
                break;
            }
        }
    } while(in_use);
    uint8_t data[4] = {1U, static_cast<uint8_t>(next_query_id >> 8),
        static_cast<uint8_t>(next_query_id), 0U};
    if(!can.send(CAN_OTA_QUERY_BASE_ID + node, data, sizeof(data)))
    {
        result.error = Error::CanSendFailed;
        result.message = "CAN query send failed";
        return false;
    }
    queries.emplace(request_id, Query{node, next_query_id,
        std::chrono::steady_clock::now() + std::chrono::seconds(5)});
    return true;
}

bool OTA::finishSlotQuery(uint8_t node, const can_frame& frame, Result& result)
{
    uint16_t id = (static_cast<uint16_t>(frame.data[1]) << 8) | frame.data[2];
    uint32_t target = (static_cast<uint32_t>(frame.data[4]) << 24) |
        (static_cast<uint32_t>(frame.data[5]) << 16) |
        (static_cast<uint32_t>(frame.data[6]) << 8) | frame.data[7];
    for(auto it = queries.begin(); it != queries.end(); ++it)
    {
        const Query& query = it->second;
        if(query.node != node || query.id != id) continue;
        result = Result{};
        result.request_id = it->first;
        result.node = node;
        if(std::chrono::steady_clock::now() >= query.deadline)
        {
            result.error = Error::Timeout;
            result.message = "node query timed out";
        }
        else if(frame.data[3] == 0U &&
                (target == OTA_SLOT_A_BASE_ADDRESS || target == OTA_SLOT_B_BASE_ADDRESS))
            result.slot = target == OTA_SLOT_A_BASE_ADDRESS ? Slot::A : Slot::B;
        else if(frame.data[3] == 1U && target == 0U)
        {
            result.error = Error::MetadataInvalid;
            result.message = "node metadata invalid";
        }
        else if(frame.data[3] == 2U && target == 0U)
        {
            result.error = Error::NodePending;
            result.message = "node has an unconfirmed slot";
        }
        else
        {
            result.error = Error::InvalidReply;
            result.message = "invalid node query reply";
        }
        queries.erase(it);
        return true;
    }
    return false;
}

bool OTA::pollTimeout(Result& result)
{
    const auto now = std::chrono::steady_clock::now();
    for(auto it = queries.begin(); it != queries.end(); ++it)
    {
        if(now < it->second.deadline) continue;
        result = Result{};
        result.request_id = it->first;
        result.node = it->second.node;
        result.error = Error::Timeout;
        result.message = "node query timed out";
        queries.erase(it);
        return true;
    }
    return false;
}

bool OTA::sendFirmware(uint64_t request_id, uint8_t node, const std::string& path, Result& result)
{
    result = Result{};
    result.request_id = request_id;
    result.operation = Operation::FirmwareSend;
    result.node = node;
    if(request_id == 0U || node == 0U || node > 127U || path.empty() ||
       path.find('\0') != std::string::npos)
    {
        result.error = Error::InvalidArgument;
        result.message = "invalid node or firmware path";
        return false;
    }
    if(isSending())
    {
        result.error = Error::Busy;
        result.message = "firmware transfer already in progress";
        return false;
    }
    int file = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if(file < 0)
    {
        result.error = errno == ENOENT ? Error::FileNotFound : Error::FileOpenFailed;
        result.message = "cannot open firmware file";
        return false;
    }
    struct stat info{};
    if(fstat(file, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 264 || info.st_size > 118 * 1024)
    {
        close(file);
        result.error = Error::InvalidFile;
        result.message = "expected an OTA image file of 264 to 120832 bytes";
        return false;
    }
    if(!ota_image_header_valid(file, static_cast<size_t>(info.st_size)))
    {
        close(file);
        result.error = Error::InvalidImage;
        result.message = "invalid OTA image header; use a packaged firmware image";
        return false;
    }
    send.file = file;
    send.request_id = request_id;
    send.node = node;
    send.file_size = (size_t)info.st_size;
    send.sent_bytes = 0;
    send.next_sequence = 0;
    send.total_frames = static_cast<uint16_t>((send.file_size + 5U) / 6U);
    send.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    return true;
}

bool OTA::serviceSend(Result& result)
{
    result = Result{};
    if(isSending() == false) return false;
    auto now = std::chrono::steady_clock::now();
    if(now >= send.deadline)
    {
        if(send.waiting_result || send.retries >= 3U)
        {
            finishSend(result, Error::Timeout,
                "node confirmation timed out; relay verification is unknown");
            return true;
        }
        ++send.retries;
        send.next_sequence = send.acked_next;
        send.deadline = now + std::chrono::milliseconds(500);
    }
    if(send.waiting_result) return false;

    for(unsigned frame = 0; frame < 8 && send.next_sequence < send.total_frames &&
        send.next_sequence - send.acked_next < 8; ++frame)
    {
        uint8_t bytes[6];
        size_t offset = (size_t)send.next_sequence * 6;
        size_t remaining = send.file_size - offset;
        size_t wanted = remaining < sizeof(bytes) ? remaining : sizeof(bytes);
        size_t length = 0;
        while(length < wanted)
        {
            ssize_t count = pread(send.file, bytes + length, wanted - length, offset + length);
            if(count < 0 && errno == EINTR) continue;
            if(count <= 0) break;
            length += (size_t)count;
        }
        if(length != wanted)
        {
            result.error = Error::ReadFailed;
            result.message = "firmware file read failed";
            break;
        }
        uint8_t data[8];
        ota_can_frame(send.next_sequence, bytes, length, data);
        if(!can.send(CAN_OTA_DATA_BASE_ID + send.node, data, sizeof(data)))
        {
            result.error = Error::CanSendFailed;
            result.message = "CAN send failed; transfer is incomplete";
            break;
        }
        ++send.next_sequence;
        if(send.next_sequence > send.highest_sent) send.highest_sent = send.next_sequence;
        send.sent_bytes = static_cast<size_t>(send.highest_sent) * 6U;
        if(send.sent_bytes > send.file_size) send.sent_bytes = send.file_size;
    }
    if(result.error == Error::None) return false;
    finishSend(result, result.error, result.message.c_str());
    return true;
}

void OTA::finishSend(Result& result, Error error, const char* message)
{
    // 先复制 message：调用方可能传入 result.message.c_str()。
    std::string detail(message);
    result = Result{};
    result.request_id = send.request_id;
    result.operation = Operation::FirmwareSend;
    result.node = send.node;
    result.transfer_started = true;
    result.submitted_bytes = send.sent_bytes;
    result.transfer_submitted = send.highest_sent == send.total_frames;
    result.error = error;
    result.message = detail;
    close(send.file);
    send = FirmwareSendState{};
}

OTA::~OTA()
{
    if(isSending()) close(send.file);
}

void OTA::cancel(uint64_t request_id)
{
    queries.erase(request_id);
    if(isSending() && send.request_id == request_id)
    {
        close(send.file);
        send = FirmwareSendState{};
    }
}

bool OTA::ota_can_frame(uint16_t sequence, const uint8_t* bytes, size_t length, uint8_t data[8])
{
    if(bytes == nullptr || data == nullptr || length == 0U || length > 6U) return false;
    data[0] = static_cast<uint8_t>(sequence >> 8);
    data[1] = static_cast<uint8_t>(sequence);
    memset(data + 2, 0xFF, 6);
    memcpy(data + 2, bytes, length);
    return true;
}

bool OTA::ota_image_header_valid(int file, size_t file_size)
{
    if(file_size < 256U) return false;
    // Identify a packaged Node OTA image before putting any file bytes on CAN.
    uint8_t header[256];
    ssize_t header_bytes;
    do { header_bytes = pread(file, header, sizeof(header), 0); }
    while(header_bytes < 0 && errno == EINTR);
    uint32_t header_crc = 0xFFFFFFFFU;
    if(header_bytes == sizeof(header))
    {
        for(size_t i = 0; i < 252U; ++i)
        {
            header_crc ^= header[i];
            for(unsigned bit = 0; bit < 8U; ++bit)
                header_crc = (header_crc >> 1) ^ ((header_crc & 1U) ? 0xEDB88320U : 0U);
        }
    }
    if(header_bytes != sizeof(header) || le32(header) != 0x3141544FU ||
       header[4] != 1U || header[5] != 0U || header[6] != 0U || header[7] != 1U ||
       le32(header + 8) != 1U ||
       (le32(header + 16) != OTA_SLOT_A_BASE_ADDRESS && le32(header + 16) != OTA_SLOT_B_BASE_ADDRESS) ||
       le32(header + 20) != file_size - sizeof(header) ||
       (header_crc ^ 0xFFFFFFFFU) != le32(header + 252))
    {
        return false;
    }
    return true;
}
