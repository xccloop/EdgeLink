#pragma once

#include <cstddef>
#include <string>
#include <sys/types.h>

class TcpClient
{
public:
    TcpClient();
    ~TcpClient();

    // socket 由当前对象负责关闭，禁止复制对象后重复关闭。
    TcpClient(const TcpClient &) = delete;
    TcpClient &operator=(const TcpClient &) = delete;

    // 阻塞等待连接成功，请在后台线程中调用；当前不设置超时。
    bool connectWifi(const std::string &wifiSsid, const std::string &password);

    // 使用 IPv4 地址阻塞连接；返回 true 表示 TCP 连接已建立。
    // 请在同一个后台线程中使用本对象；重连会关闭旧 TCP 连接。
    bool connectServer(const std::string &serverIp, int serverPort);

    // 阻塞发送，返回实际发送字节数；-1 表示失败。
    // 可能只发送一部分，调用者需要继续发送剩余数据。
    ssize_t send(const void *data, size_t length);

    // 阻塞接收，返回实际接收字节数；0 表示对端结束发送，-1 表示失败。
    // 不自动添加字符串结束符，也不保证一次收到完整 HTTP 响应。
    ssize_t receive(void *buffer, size_t capacity);

    std::string getSsid() const;
    std::string getIpAddress() const;

private:
    int socketFd;
    std::string ssid;
    std::string ipaddress;
};
