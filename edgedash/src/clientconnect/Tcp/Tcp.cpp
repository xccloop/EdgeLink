#include "Tcp.hpp"
#include "net/NetManager.h"
#include <arpa/inet.h>
#include <cerrno>
#include <cstdio>
#include <sys/socket.h>
#include <unistd.h>
#include <vector>

TcpClient::TcpClient()
{
    this->socketFd = -1;
}

TcpClient::~TcpClient()
{
    if (this->socketFd != -1)
    {
        ::close(this->socketFd);
    }
}

bool TcpClient::connectWifi(const std::string &wifiSsid, const std::string &password)
{
    if (wifiSsid.empty())
    {
        return false;
    }

    WifiManager *wifi = NETMANAGER->getWifiManager();
    if (wifi == NULL || !wifi->isSupported())
    {
        return false;
    }

    if (!wifi->isWifiEnable())
    {
        wifi->enableWifi(true);
    }

    bool scanRequested = false;
    bool connectRequested = false;

    while (true)
    {
        if (wifi->isWifiEnable())
        {
            if (wifi->isConnected())
            {
                const WifiInfo *info = wifi->getConnectionInfo();
                const char *currentIp = wifi->getIp();
                if (info != NULL && info->getSsid() == wifiSsid &&
                    currentIp != NULL && currentIp[0] != '\0' &&
                    std::string(currentIp) != "0.0.0.0")
                {
                    // 保存此时的地址；SDK 后续的 DHCP 更新不会自动写入本对象。
                    this->ssid = info->getSsid();
                    this->ipaddress = currentIp;
                    return true;
                }
            }

            if (!scanRequested)
            {
                wifi->scan();
                scanRequested = true;
            }

            if (!connectRequested)
            {
                std::vector<WifiInfo> accessPoints;
                wifi->getWifiScanInfosLock(accessPoints);
                for (size_t i = 0; i < accessPoints.size(); i++)
                {
                    if (accessPoints[i].getSsid() == wifiSsid)
                    {
                        wifi->connect(wifiSsid, password, accessPoints[i].getEncryption());
                        connectRequested = true;
                        break;
                    }
                }
            }
        }

        Thread::sleep(200);
    }
}

bool TcpClient::connectServer(const std::string &serverIp, int serverPort)
{
    if (serverPort < 1 || serverPort > 65535)
    {
        return false;
    }

    struct sockaddr_in serverAddress = {};
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(serverPort);

    int result = inet_pton(AF_INET, serverIp.c_str(), &serverAddress.sin_addr);
    if (result != 1)
    {
        return false;
    }

    if (this->socketFd != -1)
    {
        ::close(this->socketFd);
        this->socketFd = -1;
    }

    this->socketFd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (this->socketFd == -1)
    {
        perror("TCP socket failed");
        return false;
    }

    result = ::connect(this->socketFd, (struct sockaddr *)&serverAddress,
                       sizeof(serverAddress));
    if (result == -1)
    {
        perror("TCP connect failed");
        ::close(this->socketFd);
        this->socketFd = -1;
        return false;
    }

    return true;
}

ssize_t TcpClient::send(const void *data, size_t length)
{
    if (this->socketFd == -1)
    {
        errno = ENOTCONN;
        return -1;
    }

    if (data == NULL && length > 0)
    {
        errno = EINVAL;
        return -1;
    }

    ssize_t sentBytes;
    do
    {
        // 对端已关闭时返回错误，避免 SIGPIPE 信号终止整个应用。
        sentBytes = ::send(this->socketFd, data, length, MSG_NOSIGNAL);
    } while (sentBytes == -1 && errno == EINTR);

    return sentBytes;
}

ssize_t TcpClient::receive(void *buffer, size_t capacity)
{
    if (this->socketFd == -1)
    {
        errno = ENOTCONN;
        return -1;
    }

    if (buffer == NULL || capacity == 0)
    {
        errno = EINVAL;
        return -1;
    }

    ssize_t receivedBytes;
    do
    {
        receivedBytes = ::recv(this->socketFd, buffer, capacity, 0);
    } while (receivedBytes == -1 && errno == EINTR);

    return receivedBytes;
}

std::string TcpClient::getSsid() const
{
    return this->ssid;
}

std::string TcpClient::getIpAddress() const
{
    return this->ipaddress;
}
