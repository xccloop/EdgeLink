#include "OTA.hpp"
#include "OtaImageVerify.h"
#include <cstring>
#include <cerrno>
#include <cstdio>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

/*
    此函数用于
*/
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

/**
 * @brief 校验 OTA 固件文件并初始化固件发送上下文
 *
 * 该函数不执行实际的固件数据发送。它负责校验请求参数、检查 OTA
 * 发送状态、打开并验证固件文件，并初始化后续固件传输所需的发送状态。
 *
 * @param request_id 请求唯一标识符
 * @param node       目标节点 ID，有效范围为 1~127
 * @param path       OTA 固件镜像文件路径
 * @param result     输出参数，用于返回操作结果及错误信息
 *
 * @return true  固件文件校验通过，发送上下文初始化成功
 * @return false 参数、状态或固件文件校验失败
 */
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
    if(isSending() || isSendingTcp())
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
    if(!ota_image_valid(file, static_cast<size_t>(info.st_size)))
    {
        close(file);
        result.error = Error::InvalidImage;
        result.message = "invalid OTA image or signature; use a signed firmware image";
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
    if(isSendingTcp()) close(tcp_send.file);
}

void OTA::cancel(uint64_t request_id)
{
    queries.erase(request_id);
    if(isSendingTcp() && tcp_send.request_id == request_id)
    {
        close(tcp_send.file);
        tcp_send = TcpSendState{};
    }
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

/**
 * @brief 检查固件镜像头是否合法且完整
 *
 * @param file      固件文件描述符
 * @param file_size 固件文件大小，单位为字节
 *
 * @return true  完整镜像CRC和签名校验通过
 * @return false 镜像不完整、CRC错误或签名校验失败
 */
bool OTA::ota_image_valid(int file, size_t file_size)
{
    return ota_image_verify_file(file, file_size) != 0;
}

/**
* @brief 校验固件镜像并初始化 TCP OTA 发送上下文，实际发送由事件循环推进。
*
* 复用 sendFirmware() 的参数检查、传输互斥和镜像文件校验。
* 校验通过后，将临时保存在 send 中的文件句柄及文件信息交给 tcp_send，
* 分配非零的传输编号，再清空 send，避免同一文件句柄被两份上
*
* @param request_id 请求唯一标识符
* @param node       目标节点 ID，有效范围为 1~127
* @param path       OTA 固件镜像文件路径
* @param result     输出参数，用于返回操作结果及错误信息
*
* @return true  固件文件校验通过，发送上下文初始化成功
* @return false 参数、状态或固件文件校验失败
*/
bool OTA::sendFirmwareTcp(uint64_t request_id, uint8_t node, const std::string& path, Result& result)
{
    // sendFirmware 只打开并检查镜像，不发送 CAN；复用它的文件检查。
    if(!sendFirmware(request_id, node, path, result)) return false;
    tcp_send.file = send.file;
    tcp_send.request_id = request_id;
    tcp_send.node = node;
    tcp_send.file_size = send.file_size;
    if(++next_transfer_id == 0U) ++next_transfer_id;
    tcp_send.transfer_id = next_transfer_id;
    send = FirmwareSendState{};
    return true;
}

/** 
 * @brief 
 *
 * @param result:输入参数，用于传入
 *
 * @return true:
 * @return false:
 */
bool OTA::serviceTcpSend(Result& result)
{
    result = Result{};
    if(!isSendingTcp()) return false;
    if(tcp_send.queued && std::chrono::steady_clock::now() >= tcp_send.deadline)
    {
        if(!tcp_send.waiting_ack || tcp_send.retries >= 3U)
        {
            finishTcpSend(result, Error::Timeout, "TCP firmware send or node confirmation timed out");
            return true;
        }
        ++tcp_send.retries;
        tcp_send.queued = false;
        tcp_send.waiting_ack = false;
    }
    if(tcp_send.frame_length != 0U) return false;

    size_t remaining = tcp_send.file_size - tcp_send.confirmed_bytes;
    size_t wanted = remaining < 768U ? remaining : 768U;
    size_t length = 0;
    while(length < wanted)
    {
        ssize_t count = pread(tcp_send.file, tcp_send.frame + 9U + length,
                              wanted - length, tcp_send.confirmed_bytes + length);
        if(count < 0 && errno == EINTR) continue;
        if(count <= 0)
        {
            finishTcpSend(result, Error::ReadFailed, "firmware file read failed");
            return true;
        }
        length += static_cast<size_t>(count);
    }
    uint32_t transfer = tcp_send.transfer_id;
    tcp_send.frame[0] = 0x02U;
    tcp_send.frame[1] = static_cast<uint8_t>(transfer >> 24);
    tcp_send.frame[2] = static_cast<uint8_t>(transfer >> 16);
    tcp_send.frame[3] = static_cast<uint8_t>(transfer >> 8);
    tcp_send.frame[4] = static_cast<uint8_t>(transfer);
    tcp_send.frame[5] = static_cast<uint8_t>(tcp_send.block >> 8);
    tcp_send.frame[6] = static_cast<uint8_t>(tcp_send.block);
    tcp_send.frame[7] = static_cast<uint8_t>(wanted >> 8);
    tcp_send.frame[8] = static_cast<uint8_t>(wanted);
    tcp_send.frame_length = wanted + 9U;
    return false;
}

/**
 * @brief 获取当前待发送的 TCP OTA 帧及其长度
 *
 * 该函数仅返回当前已经准备好的 TCP 帧数据，不执行实际发送。
 *
 * @param data   输出参数，返回待发送帧数据的首地址
 * @param length 输出参数，返回待发送帧的字节长度
 *
 * @return true  成功获取待发送帧
 * @return false 当前无可用帧、帧已入队或未处于 TCP 发送状态
 */
bool OTA::tcpFrame(const uint8_t*& data, size_t& length) const
{
    if(!isSendingTcp() || tcp_send.queued || tcp_send.frame_length == 0U) return false;
    data = tcp_send.frame;
    length = tcp_send.frame_length;
    return true;
}

/**
 * @brief 标记当前固件帧已交给发送流程，并设置发送超时。
 *
 * 防止事件循环重复取出同一帧安排发送。
 * 本函数只更新状态，不复制数据，也不调用 socket 发送。
 * 实际数据由 Gateruntime 放入连接发送缓冲并发送。
 */
void OTA::tcpFrameQueued()
{
    tcp_send.queued = true;

    // 整帧应在 5 秒内提交给 socket，否则结束本次传输。
    tcp_send.deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

/**
 * @brief 当前完整固件帧已提交给 socket，更新提交进度并开始等待 Node 应答。
 *
 * 根据已确认字节数和当前块有效数据长度，计算本块结束位置，
 * 更新累计已提交字节数。普通块应答超时为 2 秒，
 * 尾块需要等待 Node 完成整包 Flash 读回 CRC 校验，超时为 10 秒。
 *
 * @param request_id 本次发送所属的请求编号，用于防止无关或旧请求更新发送状态
 *
 * @note 提交给 socket 不代表 Node 已收到或写入成功；
 *       confirmed_bytes 只在收到对应块的成功应答后更新。
 */
void OTA::tcpFrameSent(uint64_t request_id)
{
    // 没有 TCP 传输，或者请求编号不匹配，不更新状态。
    if(!isSendingTcp() || tcp_send.request_id != request_id) return;

    // 扣除 9 字节协议头，计算当前块在镜像文件中的结束位置。
    size_t end = tcp_send.confirmed_bytes + tcp_send.frame_length - 9U;

    // 重传同一块时，累计已提交字节数不能重复增加。
    if(end > tcp_send.submitted_bytes) tcp_send.submitted_bytes = end;

    // 整帧已提交，接下来等待 Node 的处理结果。
    tcp_send.waiting_ack = true;

    // 尾块包含整包校验，给予更长的应答时间。
    tcp_send.deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(end == tcp_send.file_size ? 10 : 2);
}

bool OTA::onTcpReply(uint64_t request_id, const uint8_t* data, size_t length, Result& result)
{
    result = Result{};
    if(!isSendingTcp() || request_id != tcp_send.request_id || length != 8U || data[0] != 0x02U)
        return false;
    uint32_t transfer = (static_cast<uint32_t>(data[1]) << 24) |
        (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 8) | data[4];
    uint16_t block = (static_cast<uint16_t>(data[5]) << 8) | data[6];
    if(transfer != tcp_send.transfer_id || !tcp_send.waiting_ack) return false;
    if(data[7] == 1U)
    {
        if(block != tcp_send.block) return false; // 旧成功回复不重复推进。
        tcp_send.confirmed_bytes += tcp_send.frame_length - 9U;
        if(tcp_send.confirmed_bytes == tcp_send.file_size)
        {
            finishTcpSend(result, Error::None, "");
            result.relay_verified = true;
            return true;
        }
        ++tcp_send.block;
        tcp_send.frame_length = 0U;
        tcp_send.queued = false;
        tcp_send.waiting_ack = false;
        tcp_send.retries = 0;
    }
    else if(data[7] == 2U)
    {
        if(block != tcp_send.block || tcp_send.retries >= 3U)
        {
            finishTcpSend(result, Error::NodeRejected, "node rejected firmware; start a new transfer");
            return true;
        }
        ++tcp_send.retries;
        tcp_send.queued = false;
        tcp_send.waiting_ack = false;
    }
    else
    {
        finishTcpSend(result, Error::InvalidReply, "invalid TCP firmware reply status");
        return true;
    }
    return false;
}

/**
 * @brief 结束本次 TCP 固件传输，保存结果并释放发送资源。
 *
 * 正常完成或发生错误时调用。将当前发送上下文中的请求编号、
 * 目标节点和已提交字节数等信息保存到 result，再关闭镜像文件，
 * 清空 tcp_send，为下一次传输做准备。
 *
 * @param result  输出参数，保存本次传输结果，供调用方生成 HTTP 响应
 * @param error   结束原因；Error::None 表示正常完成，其他值表示失败
 * @param message 结果说明字符串；正常完成时可传入空字符串 ""
 *
 * @note 本函数不设置 relay_verified，整包校验成功的标志由调用方设置。
 */
void OTA::finishTcpSend(Result& result, Error error, const char* message)
{
    // 先保存说明，避免 message 指向 result.message 时被下面的清空操作影响。
    std::string detail(message);

    // 清除旧结果，再填写本次传输的信息。
    result = Result{};
    result.request_id = tcp_send.request_id;
    result.operation = Operation::FirmwareSend;
    result.node = tcp_send.node;
    result.transfer_started = true;
    result.submitted_bytes = tcp_send.submitted_bytes;
    result.transfer_submitted = tcp_send.submitted_bytes == tcp_send.file_size;
    result.error = error;
    result.message = detail;

    // 先关闭文件，再清空发送状态，避免丢失文件句柄。
    if(tcp_send.file >= 0) close(tcp_send.file);
    tcp_send = TcpSendState{};
}