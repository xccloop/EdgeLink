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
        None, Busy, InvalidArgument, FileNotFound, FileOpenFailed, InvalidFile, InvalidImage,
        ReadFailed, CanSendFailed, InvalidReply, MetadataInvalid, NodePending, Timeout, NodeRejected
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
        bool relay_verified{false};
        bool transfer_submitted{false};
    };

    explicit OTA(Can& can) : can(can) {}
    // true：查询已发出，等待 Node 回复；false：启动失败，错误原因写入 result。
    bool querySlot(uint64_t request_id, uint8_t node, Result& result);
    ~OTA();
    OTA(const OTA&) = delete;
    OTA& operator=(const OTA&) = delete;
    // true：启动成功；false：启动失败，错误原因写入 result。
    bool sendFirmware(uint64_t request_id, uint8_t node, const std::string& path, Result& result);
    // 每次发送最多 8 帧；true 表示本次传输结束，结果写入 result。
    bool serviceSend(Result& result);
    bool isSending() const { return send.file >= 0; }
    // 返回是否属于 OTA 帧；result.request_id 非零表示有请求结果。
    bool onCanFrame(const can_frame& frame, Result& result);
    bool pollTimeout(Result& result);
    void cancel(uint64_t request_id);
    bool hasPendingQueries() const { return !queries.empty(); }

private:
    struct FirmwareSendState
    {
        int file{-1};
        uint64_t request_id{0};
        uint8_t node{0};
        size_t file_size{0};
        size_t sent_bytes{0};
        uint16_t next_sequence{0};
        uint16_t acked_next{0};
        uint16_t highest_sent{0};
        uint16_t total_frames{0};
        unsigned retries{0};
        bool waiting_result{false};
        std::chrono::steady_clock::time_point deadline;
    };
    FirmwareSendState send;
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
    void finishSend(Result& result, Error error, const char* message);
    bool ota_image_header_valid(int file, size_t file_size);
    bool ota_can_frame(uint16_t sequence, const uint8_t* bytes, size_t length, uint8_t data[8]);

    static constexpr uint32_t CAN_OTA_DATA_BASE_ID = 0x380U;
    static constexpr uint32_t CAN_OTA_QUERY_BASE_ID = 0x400U;
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
