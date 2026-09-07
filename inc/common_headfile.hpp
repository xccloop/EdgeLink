#pragma once

#include "Tcp.hpp"
#include "TcpFrame.hpp"
#include "CRC.hpp"
#include "Epoll.hpp"
#include "Storage.hpp"
#include "Can.hpp"
#include <cstdint>
#include <cstdio>
#include <sys/epoll.h>
#include <array>
#include <memory>
#include <unistd.h>
#include <unordered_map>
//修复：main需要通过errno区分EINTR、EAGAIN和真正的socket错误，所以显式包含cerrno而不是依赖其他头文件间接提供
#include <cerrno>
#include <signal.h>

#define TCPSERVE_PORT 8888
#define TCPSERVE_BACKLOG 10
#define EPOLLEVENT_SIZE 20
#define RINGBUFFER_LENGTH 240
#define MAX_CLIENTS 20

enum class FdType
{
    Tcpserve,
    Tcpclient,
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
