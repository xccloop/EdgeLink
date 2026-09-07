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
#include <vector>

class Can
{
public:
    Can();
    ~Can();

    Can(const Can&) = delete;
    Can &operator = (const Can &) = delete;

    bool init();

    int receive(can_frame &receive);
    bool send(uint32_t can_id, const uint8_t* data, uint8_t len);

    bool setnoblocking();//设置成非阻塞，同样是为了适配epoll
    
    // 添加一条过滤规则：只接收指定 ID（标准帧）
    // 多次调用会累加，最终同时过滤多个 ID
    bool addFilter(uint32_t can_id);
    // 清除所有过滤规则（恢复接收所有帧）
    bool clearFilters();

    int fd() const;
private:
    int can_fd;
    std::vector<struct can_filter> filters_;

    // 内部函数：将当前 filters_ 列表应用到内核
    bool applyFiltersToKernel();
};