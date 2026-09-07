//这个文件我们来实现CAN通信
#include "Can.hpp"
#include <linux/can.h>
#include <sys/types.h>
#include <unistd.h>
//在linux中，CAN通信与TCP通信的底层都是socket，也就是说它也可以接入epoll
//同样的我们先把完整的CAN流程写出来然后再封装为类

/*
void CAN_serve()
{
    int Can_fd;
    struct sockaddr_can addr {};
    struct ifreq ifr {};

    // 1. 创建 CAN socket
    Can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);

    if (Can_fd < 0)
    {
        perror("socket");
        return;
    }

    // 2. 指定 can0
    strcpy(ifr.ifr_name, "can0");

    // 3. 查询 can0 的接口索引
    if (ioctl(Can_fd, SIOCGIFINDEX, &ifr) < 0)
    {
        perror("ioctl");
        close(Can_fd);
        return;
    }

    // 4. 设置 CAN 地址
    addr.can_family = AF_CAN;
    addr.can_ifindex = ifr.ifr_ifindex;

    // 5. 绑定到 can0
    if (bind(Can_fd,
             reinterpret_cast<struct sockaddr *>(&addr),
             sizeof(addr)) < 0)
    {
        perror("bind");
        close(Can_fd);
        return;
    }

    printf("CAN socket bind can0 success\n");
    //上面干的事情和socketTCP差不多，都是创建套接字绑定端口，不过CAN绑定的是具体的
    //下面就是收数据和发送数据
    
    //与TCP的自定义帧不同，can有专门的帧协议
    //struct can_frame {
    //canid_t can_id;//CAN 标识符
    //__u8 can_dlc;//数据场的长度
    //__u8 data[8];//数据
    //};
    //edegheub为数据接受端，对于发送数据考虑为bootloader的方案，因此我们这里暂且简单示例
    struct can_frame can_frame{};
    can_frame.can_id = 0x123;
    can_frame.can_dlc = 1;
    can_frame.data[0] = 0xAB;
    int nbytes = write(Can_fd, &can_frame, sizeof(can_frame)); //发送数据
    if(nbytes != sizeof(can_frame))
    {
        printf("Error\n!");
    }//如果 nbytes 不等于帧长度，就说明发送失败

    //与TCP设置非阻塞一致，我们可以将can以同样的方式设置非阻塞
    int flags = fcntl(Can_fd, F_GETFL, 0);
    if (flags < 0)
    {
        perror("fcntl F_GETFL");
        return;
    }
    if (fcntl(Can_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        perror("fcntl F_SETFL");
        return;
    }

    //我们还可以设置过滤帧，即如果我们只想要某一个can_id的数据，该怎么做
    struct can_filter rfilter[1];//这里的1代表着有一条过滤规则，改成2代表着有两条
    rfilter[0].can_id = 0x123;
    rfilter[0].can_mask = CAN_SFF_MASK;
    //这里出现了一个CAN_SFF_MASK标志位，我们常见的标志位有CAN_SFF_MASK，CAN_EFF_MASK，CAN_ERR_MASK，其中CAN_ERR_MASK是用与can的错误检查的
    //CAN_SFF_MASK为基本的can_frame也就是11位，我们在can_mask设置为CAN_SFF_MASK代表着11位我们都用于检验是否是我们想要的can_id
    //也就是can_fd & CAN_SFF_MASK(0x07FF),我们知道与用于提取数据，如果相与后得到的数据就是0x123我们才收取，这一步由内核完成，我们不用进行额外操作
    //CAN_EFF_MASK，它用于当我们的can_frame为29位的时候采用，也就是我们的can_frame采用拓展帧（frame.can_id = 0x123456 | CAN_EFF_FLAG;）
    //这样子我们定义了can_id的拓展帧，拓展帧和普通帧存储数据最多都是8位，但是can_id可以变长，意味着有更多的身份信息可以存放

    //这里的读取数据非常的简单，实际上这个receive_frame是用于接受数据的，当CAN收到数据的时候就会存储到这个receive_frame，read函数就会帮我们干这件事
    struct can_frame recive_frame{};
    int mbytes = read(Can_fd,&recive_frame, sizeof(recive_frame));


    //最后是本地回环功能,开启后所有的发送帧都会被回环到与 CAN 总线接口对应的套接字上。 
    //默认情况下，发送 CAN 报文的套接字不想接收自己发送的报文，因此发送套接字上的回环功能是关闭的。可以在需要的时候改变这一默认行为
    int ro = 1; // 0 表示关闭( 默认), 1 表示开启
    setsockopt(Can_fd, SOL_CAN_RAW, CAN_RAW_RECV_OWN_MSGS, &ro, sizeof(ro));

    close(Can_fd);//别忘了关闭fd
}

*/

Can::Can()
{
    this->can_fd = -1;
}

Can::~Can()
{
    close(this->can_fd);
}

bool Can::init()
{
    struct sockaddr_can addr;
    struct ifreq ifr {};
    can_fd = socket(PF_CAN, SOCK_RAW, CAN_RAW);//创建套接字
    if (can_fd < 0)
    {
        return false;
    }
    strcpy(ifr.ifr_name, "can0");//找到can0，也就是ifconfig -a显示的can0，注意要先插入usb-can才有
    if (ioctl(can_fd, SIOCGIFINDEX, &ifr) < 0)//获取网络接口 can0 的索引
    {
        close(can_fd);
        return false;
    }
    addr.can_family = AF_CAN;//CAN
    addr.can_ifindex = ifr.ifr_ifindex;//can0的索引
    //这句话中，reinterpret_cast是最强制的强制转换，他把sockaddr_can类型强制转化为了socket通用的sockaddr
    //和tcp的bind一样，第二个都必须强制转化为sockaddr，这是历史遗留问题，暂且就当这么写吧
    if (bind(can_fd, reinterpret_cast<struct sockaddr *>(&addr), sizeof(addr)) < 0)//绑定套接字和can0
    {
        close(can_fd);
        return false;
    }
    return true;
}

int Can::receive(can_frame &receive)
{
    ssize_t nbytes = read(can_fd, &receive, sizeof(receive));
    if(nbytes < 0)
    {
        return -1;
    }
    return nbytes;
}

bool Can::send(uint32_t can_id, const uint8_t *data, uint8_t len)
{
    if(can_fd < 0 || len > 8)
    {
        return false;
    }

    struct can_frame can_frame;
    can_frame.can_id = can_id;
    can_frame.can_dlc = len;
    std::memcpy(can_frame.data,data,len);
    ssize_t nbytes = write(can_fd, &can_frame, sizeof(can_frame));
    
    if(nbytes != sizeof(can_frame))
    {
        return false;
    }

    return true;
}

bool Can::setnoblocking()
{
    //与TCP设置非阻塞一致，我们可以将can以同样的方式设置非阻塞
    int flags = fcntl(can_fd, F_GETFL, 0);
    if (flags < 0)
    {
        return false;
    }
    if (fcntl(can_fd, F_SETFL, flags | O_NONBLOCK) < 0)
    {
        return false;
    }
    return true;
} 

int Can::fd() const
{
    return can_fd;
}

// ---------- 过滤（累加） ----------
bool Can::addFilter(uint32_t can_id) {
    if (can_fd < 0) return false;

    struct can_filter f;
    f.can_id = can_id;
    f.can_mask = CAN_SFF_MASK;  // 标准帧全匹配（0x7FF）
    filters_.push_back(f); //我们在头文件中定义了一个vector的filters，这句话的意思是将我们刚刚定义好的f也就是一条过滤规则给加入到vector这个可变数组当中

    // 立即将整个列表应用到内核
    return applyFiltersToKernel();
}

bool Can::clearFilters()
{
    if (can_fd < 0)
        return false;

    filters_.clear();

    struct can_filter filter {};
    filter.can_id = 0;
    filter.can_mask = 0;

    if (setsockopt(can_fd,
                   SOL_CAN_RAW,
                   CAN_RAW_FILTER,
                   &filter,
                   sizeof(filter)) < 0)
    {
        perror("setsockopt clear filter");
        return false;
    }

    return true;
}

// 私有函数：将 filters_ 列表设置到内核
bool Can::applyFiltersToKernel() {
    if (can_fd < 0) return false;
    if (filters_.empty()) {
        // 无过滤规则 -> 接收所有
        if (setsockopt(can_fd, SOL_CAN_RAW, CAN_RAW_FILTER, nullptr, 0) < 0) {
            perror("setsockopt empty filter");
            return false;
        }
        return true;
    }

    if (setsockopt(can_fd, SOL_CAN_RAW, CAN_RAW_FILTER,
                   filters_.data(),
                   filters_.size() * sizeof(struct can_filter)) < 0) {
        perror("setsockopt add filter");
        return false;
    }
    return true;
}