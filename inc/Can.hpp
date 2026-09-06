#pragma once
#include <linux/can.h>
#include <sys/socket.h>
#include <net/if.h>
#include <sys/ioctl.h>
#include <linux/can/raw.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <fcntl.h>
#include <string>


class Can
{
public:
    bool open(const std::string& ifname);

    bool setReceiveFilter(canid_t id);

    bool send(canid_t id,
              const uint8_t* data,
              uint8_t len);

    bool receive(struct can_frame& frame);

    Can();
    ~Can();

    Can(const Can &) = delete;
    Can &operator=(const Can &) = delete; //记得禁用拷贝函数，can_fd不可复制
private:
    int fd_ = -1;
};