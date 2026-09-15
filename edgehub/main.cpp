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

#include "TcpFrame.hpp"
#include "Message.hpp"
#include "Tcp.hpp"
#include "common_headfile.hpp"
#include <cstdio>

static volatile sig_atomic_t g_running = 1;
static void sig_handler(int) { g_running = 0; }

/* CAN标准ID 0x200 + nodeId表示来源节点；payload固定为sequence(2) + temperature(4) + scale(1)。 */
static constexpr uint32_t CAN_TELEMETRY_BASE_ID = 0x200U;
static constexpr uint32_t CAN_TELEMETRY_NODE_MAX = 127U;
static constexpr uint8_t CAN_TELEMETRY_LENGTH = 7U;

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    //首先我们先创建客户端然后使用epoll
    TcpServe tcpserve(TCPSERVE_PORT,TCPSERVE_BACKLOG);
    Can can;
    Epoll epoll;
    Storage storage;
    /*
        这句话是是现代c语言定义一个数组的方式，它采用模板，里面的内容是指向ClientState的智能指针，长度为MAX_CLIENTS，并且初始化为nullptr，也就是括号里面什么都没写
        这样子做我就可以通过client[i]来控制我操作第几个客户端，因为每个指针指向的是一个单独的结构体，然后将这个指针写为unique_ptr这样做让普通的指针变为智能指针，
        当某一个客户端关闭连接不需要我手动关闭他会自己释放，并且智能指针禁止拷贝，这样就避免了使用者使用的时候出现将不同客户端之间的数据进行操作从而引发野指针事故
    */
    std::array<std::unique_ptr<ClientState>,MAX_CLIENTS> clients{};
    
    //我创建了一张名叫 fd_table 的查号表。凭借一个 int 数字（文件描述符），我能瞬间查到它对应的 FdType 枚举值。”
    //使用哈希表我们就可以做到区分listen_fd,client_fd也为后面接入CAN做好准备
    std::unordered_map<int, FdType> fd_table;

    if(can.init() == false)
    {
        printf("CAN initialization failed.\n");
        return -1;
    }

    if(can.setnoblocking() == false)
    {
        printf("Failed to set CAN socket non-blocking.\n");
        return -1;
    }

    int can_fd = can.fd();

    fd_table.emplace(can_fd, FdType::Can);

    struct epoll_event events[EPOLLEVENT_SIZE];

    if(tcpserve.init() == false)
    {
        printf("TCP server initialization failed.\n");
        return -1;
    }

    if(tcpserve.setnoblocking() == false)
    {
        printf("Failed to set TCP listen socket non-blocking.\n");
        return -1;
    }

    int tcp_fd = tcpserve.fd();

    if(epoll.create() == -1)
    {
        printf("epoll creation failed.\n");
        return -1;
    }

    if(epoll.add(tcp_fd,EPOLLIN|EPOLLET) == -1)
    {
        printf("Failed to add TCP listen socket to epoll.\n");
        return -1;
    }

    if(epoll.add(can_fd,EPOLLIN|EPOLLET) == -1)
    {
        printf("Failed to add CAN socket to epoll.\n");
        return -1;
    }

    //这句话的意思是将tcp_fd插入进哈希表，并且属于Tcpserve
    fd_table.emplace(tcp_fd, FdType::Tcpserve);

    if(storage.open("/home/qxc/Desktop/EdgeLink/edgehub/data/edgehub.db") == false)
    {
        printf("Failed to open SQLite database: /home/qxc/Desktop/Mini_Edgehub/data/edgehub.db\n");
        return -1;
    }
    if(storage.createTable() == false)
    {
        printf("Failed to create or validate SQLite table.\n");
        return -1;
    }

    while(g_running != 0)
    {
        int nready = epoll.wait(events, EPOLLEVENT_SIZE);
        if(nready == -1)
        {
            //修复：EINTR只是epoll_wait被信号临时中断，重新等待就可以；其他错误才说明事件循环无法继续
            if(errno == EINTR)
            {
                continue;
            }
            perror("epoll error");
            return -1;
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
                            //修复：ET模式必须一直accept到EAGAIN，这个错误表示当前的新连接已经全部取完，不是服务端故障
                            if(errno == EAGAIN || errno == EWOULDBLOCK)
                            {
                                break;
                            }
                            //修复：EINTR只表示accept被信号打断，继续循环就能重新接收这个连接
                            if(errno == EINTR)
                            {
                                continue;
                            }
                            //修复：其他accept错误已经由client_accept打印，结束本轮accept但不能让一个监听错误直接结束整个服务端
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
                    //现在说明有了客户端发送数据给我们了，我们要做的是接受数据，别忘了我们之前可是为了单独的客户端创建了结构体指针的
                    int slot = -1;//注意slot要在for之外，不然每次循环都要都会赋值-1
                    //修复：这里使用client_index避免遮蔽外层epoll事件下标i，后面events[i]才能一直代表当前事件
                    for(int client_index = 0;client_index < MAX_CLIENTS;client_index++)
                    {
                        //这里比较的是fd而不是client_fd，因为我们要比较的是clients数组里面存储的client_fd和响应的fd是否相同然后才返回下标
                        if(clients[client_index] != nullptr && clients[client_index]->connection.fd() == fd)
                        {
                            slot = client_index;
                            break;
                        }
                    }

                    //修复：必须遍历完clients以后再判断是否找到，否则第一个位置不匹配就会提前结束整个服务端
                    if(slot == -1)
                    {
                        //修复：哈希表存在但ClientState不存在说明登记已经失效，删除epoll和哈希表记录并关闭fd可以阻止错误事件反复触发和资源泄漏
                        epoll.del(fd, 0);
                        fd_table.erase(fd);
                        close(fd);
                        break;
                    }

                    //修复：同一个客户端事件可能同时包含可读、关闭或错误标志，所以先保存事件掩码再分别处理，不能因为没有EPOLLIN就退出服务端
                    unsigned int event_mask = events[i].events;
                    bool close_client = (event_mask & EPOLLERR) != 0;

                    if(close_client == false && (event_mask & EPOLLIN) != 0)
                    {
                        uint8_t data_tmp[READ_BUFFER_LENGTH]{};

                        //修复：客户端使用了EPOLLET，一次通知必须循环读取到EAGAIN，否则内核中剩余的数据可能不会再次触发边沿通知
                        while(true)
                        {
                            int ringbuffer_space_length = clients[slot]->receive_ringbuffer.free_space();

                            //修复：Ringbuffer没有剩余空间时不能继续从TCP取数据，否则已经离开内核的数据会因为无处保存而永久丢失
                            if(ringbuffer_space_length <= 0)
                            {
                                close_client = true;
                                break;
                            }

                            size_t receive_length = READ_BUFFER_LENGTH;

                            //修复：本次recv长度不能超过Ringbuffer剩余空间，这样收到的每一个字节都保证有位置保存
                            if(static_cast<size_t>(ringbuffer_space_length) < receive_length)
                            {
                                receive_length = static_cast<size_t>(ringbuffer_space_length);
                            }

                            //修复：保存recv真实返回值，后面只能写入实际收到的字节，不能固定把整个临时数组都写进Ringbuffer
                            ssize_t receive_result = clients[slot]->connection.data_receive(data_tmp, receive_length);
                            if(receive_result > 0)
                            {
                                int write_result = clients[slot]->receive_ringbuffer.write(
                                    data_tmp,
                                    static_cast<unsigned int>(receive_result));
                                //修复：Ringbuffer必须完整接收本次recv的全部字节，部分写入会让TCP字节流永久缺失并破坏后续帧边界
                                if(write_result != receive_result)
                                {
                                    close_client = true;
                                    break;
                                }

                                //现在环形缓冲区里面就有了原始字节流，我们调用tcp_frame_parser筛选固定Telemetry帧。
                                //修复：一次recv可能粘着多帧，所以成功解析一帧后继续调用，直到Ringbuffer只剩半帧
                                while(true)
                                {
                                    //注意要在这里创建临时的帧协议而不是全局
                                    //修复：TcpFrame只在SUCCESS时读取，使用{}初始化可以避免半包或错误状态下残留旧数据
                                    TcpFrame frame{};
                                    Message message{};
                                    int parse_result = tcp_frame_parser(&clients[slot]->receive_ringbuffer,&frame);
                                    if(parse_result == FRAME_PARSE_SUCCESS)
                                    {
                                        //此TCP端口只承载Telemetry；接入Storage时在这里调用Message_handle转换。
                                        //SUCCESS时解析器已经消费一整帧，继续循环可处理粘着的下一帧。
                                        Message_handle(&message, &frame);
                                        if(storage.isOpen() == true)
                                        {
                                            if(storage.insertMessage(message) == false)
                                            {
                                                std::string storage_error = storage.getLastError();
                                                fprintf(stderr, "%s\n", storage_error.c_str());
                                            }
                                        }
                                        else
                                        {
                                            continue;
                                        }
                                        continue;
                                    }

                                    //修复：PENDING表示TCP半包，数据继续保留在当前客户端Ringbuffer中，等待下一次数据到来即可
                                    if(parse_result == FRAME_PARSE_PENDING)
                                    {
                                        break;
                                    }

                                    //修复：ERROR表示解析器参数或缓冲区状态无法继续，只标记关闭当前客户端，不能返回-1结束整个服务端
                                    close_client = true;
                                    break;
                                }

                                if(close_client)
                                {
                                    break;
                                }
                                //修复：这批数据已经保存并解析完成，继续recv才能满足EPOLLET必须读到EAGAIN的要求
                                continue;
                            }

                            //修复：recv返回0表示对端正常关闭连接，需要进入统一的客户端清理流程
                            if(receive_result == 0)
                            {
                                close_client = true;
                                break;
                            }

                            //修复：EINTR只表示recv被信号打断，重新调用recv不会丢失连接数据
                            if(errno == EINTR)
                            {
                                continue;
                            }

                            //修复：EAGAIN表示非阻塞socket已经读空，本次ET事件处理完成，返回epoll_wait等待下一批数据
                            if(errno == EAGAIN || errno == EWOULDBLOCK)
                            {
                                break;
                            }

                            //修复：其他recv错误说明当前客户端连接不能继续，只关闭这个客户端避免影响其他连接
                            close_client = true;
                            break;
                        }
                    }

                    //修复：同一个事件可能同时带EPOLLIN和关闭标志，上面先读完最后一批数据，再根据RDHUP/HUP进入清理流程
                    if((event_mask & (EPOLLRDHUP | EPOLLHUP)) != 0)
                    {
                        close_client = true;
                    }

                    if(close_client)
                    {
                        //修复：按epoll登记、类型登记、ClientState所有权的顺序清理，reset最终通过TcpConnection析构关闭客户端fd
                        epoll.del(fd, 0);
                        fd_table.erase(fd);
                        clients[slot].reset();
                    }

                    break;
                }
                case FdType::Can: //can_fd 可读，读取接收队列中的 CAN frame
                {
                    unsigned int event_mask = events[i].events; //这里用于知道本次数据具体是什么
                    if(event_mask & EPOLLIN) //这里判断如果数据是EPOLLIN，也就是可读数据的话
                    {
                        while(true)//既然这次已经收到通知了，我就一直 read，直到把当前 CAN 接收队列彻底读空，
                        //因为我们是Epoll ET所以他只会告诉你来数据了，而我们要读取得把它读完
                        {
                            struct can_frame can_frame;
                            int receive_result = can.receive(can_frame);
                            Message can_message{};
                            //现在我们就收到数据了，要区分数据到底是来了多少

                            //如果收到的数据和can_frame长度相等，也就意味着收到了完整的一帧，我们对这一帧进行解析然后存储
                            if(receive_result == sizeof(can_frame))
                            {
                                uint32_t can_id = can_frame.can_id & CAN_SFF_MASK;
                                uint32_t temperature_raw;

                                /* 不使用固定ID过滤：每个节点的ID不同，先接收后按遥测ID范围和DLC确认格式。 */
                                if(((can_frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0U) ||
                                   (can_frame.can_dlc != CAN_TELEMETRY_LENGTH) ||
                                   (can_id <= CAN_TELEMETRY_BASE_ID) ||
                                   (can_id > (CAN_TELEMETRY_BASE_ID + CAN_TELEMETRY_NODE_MAX)))
                                {
                                    continue;
                                }

                                can_message.nodeId = static_cast<uint8_t>(can_id - CAN_TELEMETRY_BASE_ID);
                                can_message.sequence =
                                    (static_cast<uint16_t>(can_frame.data[0]) << 8) |
                                    static_cast<uint16_t>(can_frame.data[1]);
                                temperature_raw =
                                    (static_cast<uint32_t>(can_frame.data[2]) << 24) |
                                    (static_cast<uint32_t>(can_frame.data[3]) << 16) |
                                    (static_cast<uint32_t>(can_frame.data[4]) << 8)  |
                                    static_cast<uint32_t>(can_frame.data[5]);
                                can_message.temperature = temperature_raw <= 0x7FFFFFFFU ?
                                    static_cast<int32_t>(temperature_raw) :
                                    static_cast<int32_t>(static_cast<int64_t>(temperature_raw) - 0x100000000LL);
                                can_message.temperatureScale =
                                    static_cast<int8_t>(can_frame.data[6]);
                                can_message.receivedAtUs = frame_received_at_us();

                                storage.insertMessage(can_message);

                                continue;
                            }
                            if (receive_result < 0 && errno == EINTR)//这种情况代表着读取被打断，我们继续尝试读取
                            {
                                continue;
                            }
                            if (receive_result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))//这种情况代表资源（缓冲区）暂时不可用，并非连接出错”
                            {
                                break;
                            }
                            if (receive_result < 0)//其余情况直接break
                            {
                                break;
                            }
                        }
                    }
                }
                break;
            }
        }
    }


    return 0;
}
