#pragma once

#include "Can.hpp"
#include "Tcp.hpp"
#include "Epoll.hpp"
#include "Ringbuffer.hpp"
#include "Storage.hpp"
#include "Http.hpp"
#include "Httphandle.hpp"
#include <cstdint>
#include <cstdio>
#include <sys/epoll.h>
#include <array>
#include <memory>
#include <unistd.h>
#include <unordered_map>
#include <signal.h>

#define TCPSERVE_PORT 8888
#define TCPSERVE_BACKLOG 10
#define HTTPSERVE_PORT 8080
#define HTTPSERVE_BACKLOG 5
#define EPOLLEVENT_SIZE 20
#define RINGBUFFER_LENGTH 240
#define MAX_CLIENTS 20

enum class FdType
{
    Tcpserve,
    Tcpclient,
    Httpserve,
    Httpclient,
    Can
};

//这里是用于描述每个链接上的单独客户端
struct ClientState
{
    explicit ClientState(int fd)
        : connection(fd), receive_ringbuffer(RINGBUFFER_LENGTH)
    {
    }

    TcpConnection connection;
    Ringbuffer receive_ringbuffer;
};


struct HttpClientState
{
    explicit HttpClientState(int fd)
        : connection(fd)
    {
    }

    TcpConnection connection;
    Http http;
    std::string response;
    size_t sent_bytes{0};
    size_t received_bytes{0};  // 跨接收调用累计请求字节，限制请求大小。
};

/*
    这个类对外开放的理应是一个init，一个run，init用于代替我们main函数的初始化，run代替while循环
*/
class Gateruntime
{
public:
    Gateruntime();
    ~Gateruntime() = default;

    bool init();
    bool run(volatile sig_atomic_t *g_running);

private:
    TcpServe _tcpserve;
    Can _can;
    Epoll _epoll;
    Storage _storage;
    TcpServe _httpserve;

    std::unordered_map<int, FdType> fd_table;
    std::array<std::unique_ptr<ClientState>,MAX_CLIENTS> clients{};
    std::array<std::unique_ptr<HttpClientState>,MAX_CLIENTS> httpclients{};

    bool dispatchEvent(int fd, uint32_t event_mask);

    bool handleTcpServe();
    int findFreeClientSlot() const;
    int findClientSlot(int fd) const;
    bool registerTcpclient(int client_fd, int slot);
    void rejectClient(int client_fd);

    bool handleTcpclient(int fd,uint32_t event_mask);
    void drainTcpclient(int slot, bool &close_client);
    void parseTcpFrames(int slot, bool &close_client);
    bool sendTcpAck(int slot, const Message &message);
    bool sendCanAck(const Message &message);
    void closeTcpclient(int fd, int slot);
    void closeUntrackedTcpclient(int fd);

    bool handleCan(unsigned int event_mask);

    bool handleHttpServe();
    int findFreeHttpClientSlot() const;
    bool registerHttpClient(int client_fd, int slot);

    bool handleHttpClient(int fd,uint32_t event_mask);
    int findHttpClientSlot(int fd) const;
    void drainHttpclient(int slot, bool &close_client);
    void sendHttpResponse(int slot, bool &close_client);

    static constexpr uint32_t CAN_TELEMETRY_BASE_ID = 0x280U;
    static constexpr uint32_t CAN_ACK_BASE_ID = 0x300U;
    static constexpr uint32_t CAN_TELEMETRY_NODE_MAX = 127U;
    static constexpr uint8_t CAN_TELEMETRY_LENGTH = 7U;
    static constexpr uint8_t CAN_ACK_LENGTH = 5U;
};
