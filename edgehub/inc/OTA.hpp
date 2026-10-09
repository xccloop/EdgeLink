#pragma once
#include "Can.hpp"
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>

// OTA 只处理业务数据；请求编号用于关联调用方，不代表 HTTP fd 或 CAN 查询编号。
class OTA
{
public:
    enum class Operation { SlotQuery, FirmwareSend };
    enum class Error
    {
        None, InvalidArgument, FileNotFound, FileOpenFailed, InvalidFile, InvalidImage,
        ReadFailed, CanSendFailed, InvalidReply, MetadataInvalid, NodePending, Timeout
    };
    enum class Slot { None, A, B };
    struct Result
    {
        uint64_t request_id{0};
        Operation operation{Operation::SlotQuery};
        uint8_t node{0};
        Error error{Error::None};
        std::string message;
        Slot slot{Slot::None};
        bool transfer_started{false};
        size_t submitted_bytes{0};
    };

    explicit OTA(Can& can) : can(can) {}
    // true：已有结果；false：查询已发出，等待 Node 回复。
    bool querySlot(uint64_t request_id, uint8_t node, Result& result);
    Result sendFirmware(uint64_t request_id, uint8_t node, const std::string& path);
    // 返回是否属于 OTA 帧；result.request_id 非零表示有请求结果。
    bool onCanFrame(const can_frame& frame, Result& result);
    bool pollTimeout(Result& result);
    void cancel(uint64_t request_id) { queries.erase(request_id); }
    bool hasPendingQueries() const { return !queries.empty(); }

private:
    struct Query
    {
        uint8_t node;
        uint16_t id;
        std::chrono::steady_clock::time_point deadline;
    };
    Can& can;
    uint16_t next_query_id{0};
    std::unordered_map<uint64_t, Query> queries;
    bool finishSlotQuery(uint8_t node, const can_frame& frame, Result& result);
    bool ota_image_header_valid(int file, size_t file_size);
    bool ota_can_frame(uint16_t sequence, const uint8_t* bytes, size_t length, uint8_t data[8]);

    static constexpr uint32_t CAN_OTA_DATA_BASE_ID = 0x380U;
    static constexpr uint32_t CAN_OTA_CTRL_BASE_ID = 0x400U;
    static constexpr uint32_t CAN_OTA_REPLY_BASE_ID = 0x480U;
    static constexpr uint32_t CAN_ID_SEGMENT_SIZE = 128U;
    static constexpr uint8_t CAN_OTA_DATA_LENGTH = 8U;
    static constexpr uint8_t CAN_OTA_REPLY_LENGTH = 4U;
    static constexpr uint8_t CAN_OTA_REPLY_KIND_PROGRESS = 1U;
    static constexpr uint8_t CAN_OTA_REPLY_KIND_RESULT = 2U;
    static constexpr uint8_t CAN_OTA_REPLY_KIND_SLOT = 3U;
    static constexpr uint32_t OTA_SLOT_A_BASE_ADDRESS = 0x08004000U;
    static constexpr uint32_t OTA_SLOT_B_BASE_ADDRESS = 0x08021800U;
};
