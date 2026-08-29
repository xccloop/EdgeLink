/*
    这个文件是TCP通信的实现，树莓派是服务端，因此我们想的是开放一个端口，后续的节点连接这个端口
*/

#include "Tcp.hpp"
#include <asm-generic/errno-base.h>
#include <asm-generic/errno.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

/*
    接下来我们具体思考如何使用TCP通信
    第一步->创建套接字，这一步即为创建一个socket，socket的目的是抽象一个API,后续使用CAN，我们也会抽象为socket
    第二步->设置地址结构体信息bind
    第三步->设置套接字状态listened
    第四步->接收客户端请求获取新的文件描述符fd
    第五步->接受客户端数据
    第六步->发送客户端数据
    第七步->关闭套接字

    我们依旧采用初始化+核心业务+安全处理+辅助函数的做法
*/

//我们先写一个完整的流程，然后再把每一步骤细分成函数
/*
void Tcp_Serve()
{
    char recev_data[240];
    char send_data[240];   // 注意：此数组稍后会被初始化为要发送的内容

    int Tcp_fd = socket(AF_INET, SOCK_STREAM, 0); //创建套接字

    struct sockaddr_in addr;
    addr.sin_port = htons(9527);
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;//这是服务端代码，因此在ip地址设置我们采用INADDR_ANY，这样做的好处就是同时监听所有可用的网络接口
    socklen_t socklen = sizeof(addr);

    
        sockfd：表示要绑定的 socket 描述符。
        addr：表示要绑定的地址信息，是一个指向 struct sockaddr 类型的指针，可以是 struct sockaddr_in 或 struct sockaddr_in6 等结构体类型的指针。
        addrlen：表示地址信息的长度，通常使用 sizeof 运算符获取。
    
    if(bind(Tcp_fd,(struct sockaddr*)&addr,sizeof(addr)) == -1)//时返回 0，失败返回 -1
    {
        perror("TCP bind Fail");
        close(Tcp_fd);
        return;   // 绑定失败应退出，避免后续无效操作
    }//这一步为绑定bind

    
        sockfd 即就是 socket 套接字文件描述符
        backlog：表示操作系统允许在该 socket 上排队等待的最大连接数。
    
    if(listen(Tcp_fd,5) == -1)
    {
        perror("TCP listen fail");
        close(Tcp_fd);
        return;
    }
    
    //现在我们已经把TCP服务端相关的初始化完成了，接下来是接受客户端的数据和给客户端发送数据，这一步需要在while(1)里面实现
    while(1)
    {
        
            accept() 是服务器端常用的一个系统调用，用于接受客户端的连接请求并创建一个新的套接字来处理与该客户端的通信。
            sockfd：表示监听套接字的文件描述符。
            addr：表示传出参数，指向客户端地址的结构体指针。
            addrlen：表示传入传出参数，传入的是指向客户端地址结构体的长度，传出的是客户端地址结构体的实际长度并且指定数据类型为socklen_t
        
        int client_fd = accept(Tcp_fd, (struct sockaddr*)&addr,&socklen);
        if (client_fd == -1) {
            perror("TCP accept fail");
            continue;   // 接收连接失败，继续等待下一个
        }

        
            接受数据采用recv,ecv() 函数是用于接收数据的函数，它的作用是从已连接的套接字中接收数据，并将数据存储到指定的缓冲区中。
            sockfd 表示已连接的套接字描述符
            buf 表示用于存储接收数据的缓冲区
            len 表示缓冲区的长度
            flags 表示接收数据的可选参数，通常设置为 0。
        
        int byte_gets = recv(client_fd, recev_data, sizeof(recev_data) - 1, 0);  // 保留一个字节给'\0'
        if (byte_gets == -1)
        {
            perror("TCP receive fail");
            close(client_fd);
            continue;
        }
        else if (byte_gets == 0)
        {
            // 客户端主动关闭连接
            printf("Client closed connection.\n");
            close(client_fd);
            continue;
        }
        recev_data[byte_gets] = '\0';   // 将接收的数据安全地转为C字符串

        
            向客户端发送数据采用send，，它的作用是将指定的数据发送到已连接的套接字中
        
        // 构造要发送的数据（例如回显收到的内容）
        snprintf(send_data, sizeof(send_data), "Server received: %s", recev_data);
        if(send(client_fd, send_data, strlen(send_data), 0) == -1)   // 发送实际长度，而非固定240
        {
            perror("TCP send fail");
            close(client_fd);//失败关闭fd
            continue;
        }

        close(client_fd);//最后关闭fd
    }

    close(Tcp_fd);
}
*/

/*

    初始化要做的事情是配置基础服务然后返回套接字
    还是认为构造函数只传参数，不做初始化
TcpServe::TcpServe(unsigned port,unsigned int backlog,struct sockaddr_in addr)
{
    Tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    
    this->addr.sin_port = htons(port);
    this->addr.sin_addr.s_addr = addr.sin_addr.s_addr;
    this->addr.sin_family = addr.sin_family;

    this->backlog = backlog;
}
*/

/*
    inline->内联函数，省去函数调用的开销，执行速度更快（尤其适用于像 close_socket 这种只有一两行、会被频繁调用的“小函数
    果你把函数定义放在 .h 头文件中，而这个头文件被多个 .cpp 文件包含（#include），编译时就会产生“多重定义 (multiple definition)”链接错误。
    一旦你在函数前加上 inline，编译器就允许这个函数在多个 .cpp 文件中同时存在定义。链接器最终会只保留一份，并抛弃其他副本。
*/
void close_fd(int &fd)
{
    if(fd != -1)
    {
        close(fd);
        fd = -1;
    }
}

/*
    我们在构造函数中仅作传参，具体的初始化在init中实现
*/
TcpServe::TcpServe(unsigned int port,unsigned int backlog)
{
    memset(&addr, 0, sizeof(addr));

    this->addr.sin_port = htons(port);
    this->addr.sin_family = AF_INET;
    this->addr.sin_addr.s_addr = INADDR_ANY;
    this->backlog = backlog;

    Tcp_fd = -1;
}

TcpServe::~TcpServe()
{
    close_fd(Tcp_fd);
}

bool TcpServe::init()
{
    if(Tcp_fd != -1)
    {
        return false;
    }

    Tcp_fd = socket(AF_INET, SOCK_STREAM, 0);
    if(Tcp_fd == -1)
    {
        return false;
    }

    /*
        这里用一个新的API，setsockpt来进行套接字选项的系统调用，它允许做更加精细的选择
        sockfd：要设置的套接字描述符。
        level：选项所在的协议层，通常为 SOL_SOCKET（通用套接字层）或 IPPROTO_TCP（TCP 层）。
        optname：具体选项名，如 SO_REUSEADDR、SO_KEEPALIVE 等。
        optval：指向选项值的指针（类型取决于选项）。
        optlen：optval 的大小。
    */
    int reuse = 1;
    if(setsockopt(Tcp_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == -1)
    {
        perror("setsockopt");
        close(Tcp_fd);
        Tcp_fd = -1;
        return false;
    }

    if(bind(Tcp_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1)
    {
        perror("bind");
        close(Tcp_fd);
        Tcp_fd = -1;
        return false;
    }

    if(listen(Tcp_fd, backlog) == -1)
    {
        perror("listen");
        close(Tcp_fd);
        Tcp_fd = -1;
        return false;
    }

    return true;
}

/*
    这个函数用于返回clinetfd，这是Tcpconnection类的关键
*/
int TcpServe::client_accept()
{
    struct sockaddr_in client_addr;
    socklen_t client_addr_len = sizeof(client_addr);
    
    int client_fd = accept(Tcp_fd, (struct sockaddr*)&client_addr, &client_addr_len);
    if (client_fd == -1) {
        perror("accept failed");
        return -1;
    }
    return client_fd;
}

int TcpServe::fd() const
{
    if(Tcp_fd == -1)
    {
        perror("Tcpserve fd return fail");
        return -1;
    }

    return Tcp_fd;
}

bool TcpServe::setnoblocking()
{
    //设置fd为非阻塞我们使用
    //fcntl允许我们进行文件操作
    //这里分两步进行，第一步我们先通过第一次fcntl的F_GETFL将fd的属性用我们设置的flags提取出来
    //第二步我们在通过fcntl的F_SETFL将fd设置为O_ONOBLOCKING也就是非阻塞
    int flags = fcntl(Tcp_fd,F_GETFL,0);
    if(flags == -1)
    {
        return false;
    }

    //我们通过|（按位或）的方式开启这个开关
    if (fcntl(Tcp_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        return false;
    }

    return true;
}

TcpConnection::TcpConnection(int client_fd)
{
    this->client_fd = client_fd;
}

TcpConnection::~TcpConnection()
{
    close_fd(client_fd);
}

/*
    当前的情况下receive,send都是阻塞式的，我们在此之前设置fd为非阻塞式，那就会出现非阻塞式的一些特殊情况，我们使用错误编码来代指这些情况，我们也需要对这些非阻塞式的特殊错误进行处理
    
*/

ssize_t TcpConnection::data_receive(char buffer[],size_t length)
{
    ssize_t data_recive = recv(this->client_fd, buffer,length,0);  // 保留一个字节给'\0'
    if(data_recive == -1)
    {
        if(errno == EAGAIN || errno == EWOULDBLOCK)//这里的errno就是特殊错误代码
        //出现错误的第一种情况为资源暂时不可用，我们可以直接返回-1或者返回0再次重试，这里加一个内括号是因为运算优先级的问题
        {
            // 暂时没数据，返回 -1，上层会忽略并继续 epoll_wait
            return -1;
        }
        if (errno == EINTR)
        {
            // 被信号中断，可以重试，这里简单返回 -1 让上层重试也行
            // 或者用 goto 重试，但简单起见返回 -1
            return -1;
        }
        if(errno == ECONNRESET || errno == EPIPE || errno == ENOTCONN || errno == ESHUTDOWN)
        //这四类错误十分严重我们需要立即关闭fd
        //ECONNRESET	连接被对端重置（RST）。	对端进程崩溃、异常重启，或者对端硬生生关闭了连接（没发 FIN）。	
        //EPIPE	向已关闭的连接写入数据。	对端已经关闭了连接（FIN），你还调用 send。注意：如果你用了 MSG_NOSIGNAL，系统不会发 SIGPIPE 信号杀死进程，但 send 会返回 -1 并设置 errno 为 EPIPE。	。
        //ENOTCONN	套接字未连接。	fd 不是已连接的 socket（比如已被关闭）。	
        //ESHUTDOWN	套接字已经关闭了写端（或读端）。	本方已经主动执行了 shutdown。	
        {
            close(client_fd);
            return -1;
        }
        if(errno == EBADF || errno == EFAULT || errno == EINVAL)
        //这三类错误为程序逻辑错误，我们需要打印相关日志来提醒并且返回-1
        {
            if(errno == EBADF)//文件描述符无效
            {
                perror("fd is not available");
                return -1;
            }
            if(errno == EFAULT)//传入的缓冲区指针指向了非法的地址
            {
                perror("the buffer poniter point a lllegal adress");
                return -1;
            }
            if(errno == EINVAL)//传入的参数无效
            {
                perror("parament is not available");
                return -1;
            }
        }
        perror("unexpected error");//其余的错误不常见因此直接返回-1，如果遇到了，在细节调试
        return -1;
    }
    return data_recive;
}

ssize_t TcpConnection::data_send(const char *buffer, size_t length)
{
    size_t sent_length = 0;
    while(sent_length < length)
    {
        ssize_t data_send = send(client_fd, buffer + sent_length, length - sent_length, MSG_NOSIGNAL);
        if(data_send > 0)
        {
            sent_length += static_cast<size_t>(data_send);
            continue;
        }

        //接下来时send的非阻塞处理代码错误
        if(data_send == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                // send触发EAGAIN是因为缓冲区已满，无法写入更多数据
                if(sent_length > 0)//如果已经写入了一部分数据，那我们返回写入的字节数，如果一个数据没写就直接返回-1
                {
                    return static_cast<ssize_t>(sent_length);
                }
                return -1;
            }
            if(errno == EINTR)
            {
                continue;//系统调用被信号中断，重试
            }
            //同样的，这四类错误是严重错误，我们需要理解关闭fd并且从epoll树移除
            if(errno == EPIPE || errno == ECONNRESET || errno == ENOTCONN || errno == ESHUTDOWN)
            //EPIPE:对端已经关闭了连接（发过 FIN），你还调用 send。
            //ECONNRESET: 端进程崩溃或异常重启，强行发送了 RST 包（而不是优雅的 FIN）
            //ENOTCONN :这个 socket 根本没有处于连接状态（比如已经被关闭，或者还没 connect 成功）
            //ESHUTDOWN:本端已经执行了 shutdown() 关闭了写端，你还试图 send。
            {
                close(client_fd);
                return -1;
            }
            if(errno == EBADF || errno == EFAULT || errno == EINVAL)
            {
                if(errno == EBADF)//传入的 client_fd 无效（可能已经被关闭，或者根本不是 socket）。
                {
                    perror("fd is not acailable");
                    close(client_fd);
                    return -1;
                }
                if(errno == EFAULT)//传入的 buffer 指针指向了非法内存地址（野指针）
                {
                    perror("buffer poniter point a lllegal adress ");
                    close(client_fd);
                    return -1;
                }
                if(errno == EINVAL)//传入的参数无效
                {
                    perror("parament is not avaiable");
                    return -1;
                }
            }
            perror("unexpected error");
            return -1;
        }        
    }
    return static_cast<ssize_t>(sent_length);
}

int TcpConnection::fd() const
{
    if(client_fd == -1)
    {
        perror("client_fd return fail");
        return -1;
    }

    return client_fd;
}

bool TcpConnection::setnoblocking()
{
    //设置fd为非阻塞我们使用
    int flags = fcntl(client_fd,F_GETFL,0);
    if(flags == -1)
    {
        return false;
    }

    if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1)
    {
        return false;
    }
    
    return true;
}