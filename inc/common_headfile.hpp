#pragma once

#include "Tcp.hpp"
#include "Frame.hpp"
#include "CRC.hpp"
#include "Epoll.hpp"
#include <cstdio>
#include <sys/epoll.h>
#include <array>
#include <memory>
#include <unistd.h>
#include <unordered_map>

#define TCPSERVE_PORT 8888
#define TCPSERVE_BACKLOG 10
#define EPOLLEVENT_SIZE 20
#define RINGBUFFER_LENGTH 240
#define MAX_CLIENTS 20

enum class FdType
{
    Tcpserve,
    Tcpclient
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
