/*
    Mini Edgehub项目标题
    项目主要是学习epoll的rs485,can,tcp,mqtt,安全措施，同时也涉及简单的业务逻辑
    部分通信协议需要把edgehub规范为统一的内部消息模型
    思考项目要从0逐渐建
    //嵌入式项目无非是初始化+模块使用+跨模块+安全措施

    //1.初始化
    //外部需要初始化树莓派的硬件资源，初始化网络，初始化窗口，初始化CAN总线等

    //2.模块使用
    //外部需要使用epoll管理网络，窗口，CAN总线等

    //3.进程管理
    //外部需要使用多线程运行不同任务，比如网络通信，路由
    //如果epoll用的是事件驱动模型，所以可以不使用多线程，直接使用epoll管理所有的fd即可，然后在回调函数中处理不同事件

*/

//现在我们有了 TCP，epoll，ringbuffer，frame，足够我们来写一份服务端戴代码了
//要求，使用epoll ET监听tcp，并且将收到的数据存储进ringbufer然后校验自定义帧协议

#include "common_headfile.hpp"

int main()
{
    //首先我们先创建客户端然后使用epoll
    TcpServe tcpserve(TCPSERVE_PORT,TCPSERVE_BACKLOG);
    Epoll epoll;

    /*
        这句话是是现代c语言定义一个数组的方式，它采用模板，里面的内容是指向ClientState的智能指针，长度为MAX_CLIENTS，并且初始化为nullptr，也就是括号里面什么都没写
        这样子做我就可以通过client[i]来控制我操作第几个客户端，因为每个指针指向的是一个单独的结构体，然后将这个指针写为unique_ptr这样做让普通的指针变为智能指针，
        当某一个客户端关闭连接不需要我手动关闭他会自己释放，并且智能指针禁止拷贝，这样就避免了使用者使用的时候出现将不同客户端之间的数据进行操作从而引发野指针事故
    */
    std::array<std::unique_ptr<ClientState>,MAX_CLIENTS> clients{};
    
    //我创建了一张名叫 fd_table 的查号表。凭借一个 int 数字（文件描述符），我能瞬间查到它对应的 FdType 枚举值。”
    //使用哈希表我们就可以做到区分listen_fd,client_fd也为后面接入CAN做好准备
    std::unordered_map<int, FdType> fd_table;

    struct epoll_event events[EPOLLEVENT_SIZE];

    if(tcpserve.init() == false)
    {
        return -1;
    }

    if(tcpserve.setnoblocking() == false)
    {
        return -1;
    }

    int tcp_fd = tcpserve.fd();

    if(epoll.create() == -1)
    {
        return -1;
    }

    if(epoll.add(tcp_fd,EPOLLIN|EPOLLET) == -1)
    {
        return -1;
    }

    //这句话的意思是将tcp_fd插入进哈希表，并且属于Tcpserve
    fd_table.emplace(tcp_fd, FdType::Tcpserve);

    while(1)
    {
        int nready = epoll.wait(events, EPOLLEVENT_SIZE);
        if(nready == -1)
        {
            perror("epoll error");
        }

        int client_fd = 0;
        for(int i = 0;i < nready;i++)
        {
            int fd = events[i].data.fd;

            //这里我们再哈希表中使用find来寻找响应的fd
            auto fd_iterator = fd_table.find(fd);
            if(fd_iterator == fd_table.end())
            {
                //如果等于end意味着没有找到，可能是特殊情况，我们就继续找
                continue;
            }

            //这里是fd_iterator->second,为什么是这样写呢，因为哈希表中fd_iterator找到后同时对应两个键值，第一个是我们的int fd，第二个才是FdType
            switch(fd_iterator->second)
            {
                case FdType::Tcpserve: //此时是新连接
                {
                    while(true)
                    {
                        client_fd = tcpserve.client_accept();
                        if(client_fd == -1)
                        {
                            break;
                        }

                        //现在我们知道有新的客户端来连接了，我们就需要把它放在单独的

                        //slot代表着client里面的执政是否都放满了也就是是否已经达到最大连接客户端数量了
                        int slot = -1;
                        //我们遍历整个client数组从0到MAX_CLIENTS-1，因此是++i而不是i++
                        for(int i = 0;i < MAX_CLIENTS;++i)
                        {
                            if(clients[i] == nullptr)//如果此时找到了空闲的位置，此时i代表着client的第几位是空闲的
                            {
                                slot = i;//我们让slot等于i
                                break;//不再寻找，因为一个客户端只需要占用一个指针
                            }
                        }

                        //前面我们已经让slot等于i了，如果运行到这里，slot依旧是-1，也就意味着前面的for没有生效
                        //换句话说就是client已经满了
                        if(slot == -1)
                        {
                            printf("Too many clients, closing new connection.\n");
                            close(client_fd);   // 关闭新客户端的 socket，拒绝服务
                            continue;           // 跳过本次循环，继续 accept 下一个连接（因为外面是 while）
                        }

                        //现在我们找到了空闲的位，我们要用client_fd来创建一个单独的ClientState
                        clients[slot] = std::make_unique<ClientState>(client_fd);

                        //最后别忘了挂载epoll树和设置非阻塞
                        if(clients[slot]->connection.setnoblocking() == false)
                        {
                            clients[slot].reset();
                            continue;
                        }

                        if(epoll.add(client_fd, EPOLLIN | EPOLLRDHUP | EPOLLET) == -1)
                        {
                            clients[slot].reset();
                            continue;
                        }

                        fd_table.emplace(client_fd, FdType::Tcpclient);
                        //至此我们就完成了
                    }
                    break;
                }

                case FdType::Tcpclient:
                {
                    break;
                }
            }
        }
    }
    return 0;
}
