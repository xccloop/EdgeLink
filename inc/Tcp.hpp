#pragma once

/*
    现在我们有了完整的流程，从用户态来说，我们可以把流程分为，
    初始化+发送数据+接受数据，在技术中我们分为，初始化+开启单个客户端连接+处理单个客户端连接+发送数据+接收数据
    
*/
#include <cstddef>
#include <netinet/in.h>
#include <cstdio>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>

#define TCPSERVE_INIT_FAIL -1
#define TCPSERVE_INIT_SUCCESS 0

#define READ_BUFFER_LENGTH 240

/*
    设计更新，我们才Tcpserve类来使创建服务端相关参数，再用TcpConnection来处理具体的客户端数据
*/
class TcpServe
{
public:
    TcpServe(unsigned int port,unsigned int backlog);
    ~TcpServe();

    TcpServe(const TcpServe &) = delete;
    TcpServe &operator=(const TcpServe &) = delete;

    bool init();

    int client_accept();

    int fd() const; //这样做可以让返回的fd设置为可读但不可写
    //用方只能把它用于 epoll.add()、比较、日志等，不能 close(get_fd())。fd 的关闭权仍属于 TcpServe，否则析构时会发生“重复关闭”或误关闭复用后的 fd。

private:
    int Tcp_fd{-1};//tcp套接字
    struct sockaddr_in addr;//sockaddr结构体
    unsigned int backlog;//listend的排队等待的最大连接数。

};

class TcpConnection
{
public:
    TcpConnection(int client_fd);
    ~TcpConnection();

    TcpConnection(const TcpConnection &) = delete;
    TcpConnection &operator=(const TcpConnection &) = delete;

    //这里使用ssize_t是因为这个类型为实际的数字，如果用其他的比如size_t，返回-1就会变成很大的数字
    ssize_t data_receive(char buffer[],size_t length);
    ssize_t data_send(const char buffer[],size_t length);

    int fd() const;

private:
    int client_fd{-1};
};