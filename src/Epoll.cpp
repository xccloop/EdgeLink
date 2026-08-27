/*
    既然已经实现了阻塞式的tcp服务端客户端请求，接下来我们来尝试服务端支持多客户端连接并且非阻塞
    我们采用epoll
    在Tcpserve和Tcpconnection中我们对客户端和服务端进行操作实际上都是在对对应的fd进行操作
    如果可以做到高效管理文件I/O(fd)就可以做到高效管理客户端连接这也就是epoll的核心操作
    epoll的核心是只有当数据来临的时候，才去接受管理数据
    epoll 不像 select 和 poll 那样需要轮询所有文件描述符，而是通过事件通知机制，只有当文件描述符上有事件发生时才会通知进程。
    每次取就绪集合的位置固定。并且一定程度上实现异步解耦。
    epoll 在处理大量文件描述符时性能更高，能够有效地支持数万、数十万甚至数百万级别的并发连接。 其性能不会随着文件描述符数量的增加而显著下降。
    epoll 使用了内核中的红黑树和就绪列表 等高效数据结构，优化了事件的存储和查找。
*/

#include "Epoll.hpp"
#include <sys/epoll.h>
#include <cstddef>
#include <netinet/in.h>
#include <cstdio>
#include <iostream>
#include <cstring>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <cerrno>


/*


    我们依旧先采用完整流程->class封装->逻辑检查的做法来实现

void close_fd(int &fd)
{
    if(fd != -1)
    {
        close(fd);
        fd = -1;
    }
}

void Epoll_serve()
{
    //这一段是Tcp服务端的基础代码
    int listenfd = socket(AF_INET, SOCK_STREAM, 0);
    if (listenfd < 0) {
        std::cerr << "create socket error: " << strerror(errno) << std::endl;
    }
    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9527);
    addr.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(listenfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        std::cerr << "bind socket error: " << strerror(errno) << std::endl;
        close_fd(listenfd);
    }

    if (listen(listenfd, 5) < 0) {
        std::cerr << "listen socket error: " << strerror(errno) << std::endl;
        close_fd(listenfd);
    }

    //与之前一样，linux中我们使用fd来进行文件操作，这里要求传入一个数，实际上传入正整数都可以，这是历史遗留问题
    //与之对应的一个函数为epoll_create1();这个函数等形参为配置选择
    int epoll_fd = epoll_create(1);

    //与TCP通信的sockaddr_in结构体对应，这里采用epoll_event来进行相关配置
    struct epoll_event epoll_value;
    epoll_value.events = EPOLLIN;//这个设置epoll 为监听可读事件
    epoll_value.data.fd = listenfd;//这里要求我们设置listen的fd，因此我们需要返回Tcp文件来实现相关代码

    //接下来是关联epoll，第一个是epoll的fd，第二个是让我们在EPOLL_CTL_DEL/ADD/MOD三选一
    //这三个分别为设置这个epoll_fd对于另外一个fd的操作，可以为添加，修改，或者是移动
    //比如我们现有的epoll_value结构体它用来监听可读事件，我们要先通过ADD操作将他挂载到epoll树上这样epoll才能对他监听，如果我们想让它改为监听可读可写事件需要用MODIFY
    //注意，如果想要使用EPOLL_CTL_MOD那么我们需要新增一个epoll_event结构体，用于存放新的配置
    //而想要删除则是DEL，第三个为被操作的fd，最后一个则是我们对应的epoll_value结构体
    epoll_ctl(epoll_fd,EPOLL_CTL_ADD,listenfd,&epoll_value);

    //现在我们已经做好了基础配置该开始真正进行epoll处理循环了
    
    //这里我们创建了一个epoll_events类型的数组events，这是什么意思呢,我们需要先看他的服务对象也就是epoll_wait
    //【修正点1】：epoll_wait 返回的是本次“实际就绪”的有效事件个数（nready）。
    //注意：这些就绪事件可能来自监听fd（新连接），也可能来自已连接的客户端fd（有数据到达），并不是只返回 listenfd。
    //【修正点2】：events数组的大小（这里是5）决定了本次最多能“搬运”多少个就绪事件（类似一个能装5个快递的篮子）。
    //实际取出了多少个由返回值 nready 决定（8个就绪事件，篮子只能装5个，就分两次取，第一次取5个，第二次取剩下的3个）。
    //【修正点3】：epoll_wait 会把就绪的事件信息填充到 events 数组里，但 events[i] 本身是结构体，不是直接的 fd。
    //我们要通过 events[i].data.fd 才能拿到具体的文件描述符（可能是listenfd，也可能是client_fd）。
    struct epoll_event events[5];
    int client_fd;
    struct sockaddr_in client_addr;
    socklen_t client_length = sizeof(client_addr);
    while(1)
    {
        //epoll_wait就是epoll处理数据的核心方式
        int nready = epoll_wait(epoll_fd, events, sizeof(events)/sizeof(events[0]), 10);
        if(nready < 0)
        {
            perror("epoll wait fail");
        }
    

    //【修正点4】：现在我们有了存放着就绪事件的 events 数组。
    //注意：不要直接把 events[i] 当成 fd 来用，必须取出 events[i].data.fd。
    //接下来的逻辑（虽然代码还没写，但思路应该是）：
    //遍历 i 从 0 到 nready-1，如果 fd == listenfd，则 accept 新连接；
    //否则，根据 events[i].events 判断是 EPOLLIN（可读）还是 EPOLLOUT（可写），进行 recv 或 send 操作。

    //我们此时来回顾一下全流程
    //第一步服务器创建socket套接字-listenfd
    //第二步我们让这listenfd进行bind和listen开始监听
    //第三步我们引入epoll-这个管fd变动的东西
    //现在我们思考，epoll管的fd到底是什么
    //我们先回顾之前阻塞网络通信的流程，客户端于服务端进行TCP三次握手建立连接然后accept建立通信，注意到了吗，这里有一个TCP三次握手
    //实际上当TCP三次握手完毕之后，服务端listen_fd会变为可读，而变更状态正好是fd数值发生变动，
    //此时epoll监测到了listen_fd发生变动，说明客户端已经来了我们要正式accept了，因此在流程中epoll监测的是listen_fd，只有当listen_fd发生变动的时候，才是客户端连接成功
    //值得注意的是，TCP三次握手连接后，listen_fd之会变为可读数据，因为这相当于“队列从空变成非空”，意味着 “有东西可以取出来了”也就是可以读了，如果是可以写，相当于修改fd，这是错误的
    //再accept创建client_fd之后我们同样需要把client_fd挂载到epoll树上，这样的操作是为了后续我们对已经创立好连接的客户端进行操作
    //比如收数据，对方关闭连接，对方数据已满，这都是需要考虑的
    //我们已经有了足够的信息，足够开始写fd操作了

        for(int i = 0; i<nready; i++)//我们这里采用遍历有效fd来进行操作
        {
            int fd =events[i].data.fd;
            if(fd == listenfd)
            {
                client_fd = accept(listenfd, (struct sockaddr*)&client_addr, &client_length);
                if(client_fd == -1)
                {
                    perror("client accept fail");
                }
                struct epoll_event client_ev;       // 必须重新定义一个结构体
                client_ev.events = EPOLLIN;         // 监听客户端数据
                client_ev.data.fd = client_fd;      // 务必填成 client_fd 自己的 fd
                epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &client_ev);  
            }
            else {
                //这里则需要对客户端的数据进行相关操作，在此省略
            }
        }
        //这就是事件分发模型，当events[i].data.fd == listen_fd说明需要有新的客户端来了要创立连接，
        //当events[i].data.fd == client_fd，说明要对之前的客户端来数据了，我们需要对旧的客户端进行操作比如接受数据，比如发送数据
        //另外和listen_fd只能变为可读不同，client_fd可以是可读也可以是可写
        //listen_fd变为可读是瞬间的事，不会出现永久可读或者是一次可读+变回去触发两次epoll
    }

    //最后我们区分水平触发和边缘触发，实际上是epoll等待数据变更形式的不同，也就是这一行：epoll_value.events = EPOLLIN
    //我们发现只有可读的时候才会触发epoll，如果我们写为 epoll_value.events = EPOLLIN | EPOLLOUT;（注意是赋值=和按位或|）
    //也就是变为可读和变为可写都会触发，那对应的while(1)的逻辑也要变换（需要增加对 EPOLLOUT 的处理分支）
    //我们当前的做法是LT水平触发，这相对简单，只要fd变动我们有充足的时间读完一整串数据

    //因此我们来具体理解：LT,ET是两种策略，我们在写epoll_value.events的配置时候如果不加入EPOLLET那么默认就是LT，
    //不管这个配置里写了 EPOLLIN 还是 EPOLLOUT，只要没加 EPOLLET，就都是 LT 模式。
    //当处在LT的时候，数据（内核缓冲区里的字节）会等到我们把它全部取完（读到缓冲区为空），可读状态才会消失。
    //那为什么会有可读/可写两种呢？因为我们的客户端有 recv（接收数据）和 send（发送数据）两种操作，我们需要分开监听。

    //当处在ET模式（即 events 里加了 | EPOLLET），数据到来时内核只会在“空->非空”的瞬间通知一次。
    //注意：如果在这次通知中没有一次性把数据全部读完（比如只 recv 了一次就退出），
    //剩下的数据（内核缓冲区里的字节）并不会因为“时间过了”而物理消失，
    //它们依然好好躺在内核缓冲区里！真正“消失”的是内核给你的“通知权限”。
    //因为 ET 认为“我已经喊过你了”，在缓冲区再次变空之前，它不会再喊第二次。
    //直到下一次客户端发来新数据，触发新的“空->非空”变化，你才有机会把旧数据连同新数据一起取走。
    //所以，ET 模式下的编程铁律是：必须在通知到来时，用 while 循环反复 recv，直到返回 EAGAIN 错误（缓冲区彻底读空）。

    //顺带一提，这里我们说的“数据”绝对不是指 struct epoll_event events[5] 这个数组，
    //events 数组只负责搬运文件描述符（fd）和事件类型（EPOLLIN/OUT）。
    //真正的业务数据（字节流）是存放在客户端对应的内核接收缓冲区中，
    //需要通过 recv 复制到我们自己定义的 char buffer[] 或 std::string 等应用层变量里。
}

*/

void close_fd(int &fd)
{
    if(fd != -1)
    {
        close(fd);
        fd = -1;
    }
}

Epoll::Epoll() {
    epoll_fd = -1;
}

Epoll::~Epoll() {
    close_fd(epoll_fd);
}

int Epoll::create() {
    this->epoll_fd = epoll_create(1);
    if (this->epoll_fd == -1) {
        perror("epoll create fail");
        return -1;
    }
    return 0;
}

int Epoll::add(int fd, int epoll_event) {
    if (this->epoll_fd == -1) {
        perror("epoll add fail");
        return -1;
    }
    struct epoll_event ev;
    ev.events = epoll_event;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, fd, &ev) == -1) {
        perror("epoll add fail");
        return -1;
    }
    return 0;
}

int Epoll::del(int fd, int epoll_event) {
    if (this->epoll_fd == -1) {
        perror("epoll delete fail");
        return -1;
    }
    struct epoll_event ev;
    ev.events = epoll_event;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, &ev) == -1) {
        perror("epoll delete fail");
        return -1;
    }
    return 0;
}

int Epoll::mod(int fd, int epoll_event) {
    if (this->epoll_fd == -1) {
        perror("epoll modify fail");
        return -1;
    }
    struct epoll_event ev;
    ev.events = epoll_event;
    ev.data.fd = fd;
    if (epoll_ctl(epoll_fd, EPOLL_CTL_MOD, fd, &ev) == -1) {
        perror("epoll modify fail");
        return -1;
    }
    return 0;
}

// ---------- 无参重载（默认 LT） ----------
int Epoll::add(int fd) {
    return add(fd, EPOLLIN);   // 默认 LT 读事件
}

int Epoll::del(int fd) {
    if (this->epoll_fd == -1) {
        perror("epoll delete fail");
        return -1;
    }
    // 删除时事件参数被忽略，直接传 NULL
    if (epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL) == -1) {
        perror("epoll delete fail");
        return -1;
    }
    return 0;
}

int Epoll::mod(int fd) {
    return mod(fd, EPOLLIN);   // 默认 LT 读事件
}


int Epoll::wait(epoll_event events[], int length, int timeout_ms)
{
    if (epoll_fd < 0 || events == nullptr || length <= 0)
    {
        errno = EINVAL;
        return -1;
    }

    return epoll_wait(epoll_fd, events, length, timeout_ms);
}