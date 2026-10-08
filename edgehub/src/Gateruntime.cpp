#include "Gateruntime.hpp"
#include "TcpFrame.hpp"
#include "Can.hpp"
#include <sys/epoll.h>

/*
    GateRuntime 负责拥有并初始化 EdgeHub 的运行时资源：TcpServe、Can、Epoll、
    Storage、客户端状态数组和 fd 类型表；它在事件循环中根据就绪 fd 分发 TCP
    监听、TCP 客户端和 CAN 事件。

    它的职责是运行时调度，不是把 Frame 解析和 SQLite 业务规则重新实现一遍：
    TcpFrame 仍负责字节流到完整 Frame 的解析，Message 仍负责数据模型转换，
    Storage 仍负责 SQLite 的打开、建表和持久化。

    原 main() 的运行过程迁入这里：
    1. 初始化 CAN、设置非阻塞，取得 can_fd 并登记为 FdType::Can。
    2. 初始化 TCP 监听 socket、设置非阻塞，取得 tcp_fd 并登记为 FdType::Tcpserve。
    3. 创建 epoll，并把 can_fd 和 tcp_fd 注册为边沿触发读事件。
    4. 打开 SQLite 并创建或校验数据表。
    5. 在 while(g_running != 0) 中调用 epoll.wait(events, ...)。

    epoll.wait() 的返回值 nready 表示本次就绪事件的数量；具体的 fd 和事件标志
    保存在 events[i] 中。通过 fd_table 查到 FdType 后，再分发到对应处理流程：

    - Tcpserve：持续 accept 到 EAGAIN；为每个新连接分配独立 ClientState，设置
      非阻塞，并注册到 epoll 和 fd_table。
    - Tcpclient：持续 recv 到 EAGAIN；把真实收到的字节写入该客户端独立的
      RingBuffer，解析完整 TCP Frame，转换为 Message 后交给 Storage。还要处理
      EINTR、EPOLLRDHUP、EPOLLHUP、EPOLLERR 和统一的客户端清理。
    - Can：持续 can.receive() 到 EAGAIN；校验 CAN ID 和 DLC，转换为 Message 后
      交给 Storage。

    客户端清理必须由 GateRuntime 统一完成：epoll.del(fd)、fd_table.erase(fd) 与
    clients[slot].reset() 必须保持一致，避免失效登记、重复事件或 fd 泄漏。
*/

Gateruntime::Gateruntime()
    : _tcpserve(TCPSERVE_PORT, TCPSERVE_BACKLOG)
    ,_httpserve(HTTPSERVE_PORT, HTTPSERVE_BACKLOG)
{
}

/*
    初始化运行时拥有的 CAN、TCP 监听 socket、epoll 和 SQLite。
    成功后 fd_table 只登记监听 fd 和 CAN fd；客户端 fd 在 accept 后再登记。
    任一步失败就返回 false，调用者不应进入 run()。
*/
bool Gateruntime::init()
{
    if(_can.init() == false)
    {
        printf("CAN initialization failed.\n");
        return false;
    }

    if(_can.setnoblocking() == false)
    {
        printf("Failed to set CAN socket non-blocking.\n");
        return false;
    }

    int can_fd = _can.fd();

    fd_table.emplace(can_fd, FdType::Can);

    if(_tcpserve.init() == false)
    {
        printf("TCP server initialization failed.\n");
        return false;
    }

    if(_tcpserve.setnoblocking() == false)
    {
        printf("Failed to set TCP listen socket non-blocking.\n");
        return false;
    }

    int tcp_fd = _tcpserve.fd();

    if(_httpserve.init() == false)
    {
        printf("Failed to set HTTP listen socket non-blocking.\n");
        return false;
    }

    if(_httpserve.setnoblocking() == false)
    {
        printf("Failed to set HTTP listen socket non-blocking.\n");
        return false;
    }

    int http_fd = _httpserve.fd();

    if(_epoll.create() == -1)
    {
        printf("epoll creation failed.\n");
        return false;
    }

    if(_epoll.add(tcp_fd,EPOLLIN|EPOLLET) == -1)
    {
        printf("Failed to add TCP listen socket to epoll.\n");
        return false;
    }

    if(_epoll.add(can_fd,EPOLLIN|EPOLLET) == -1)
    {
        printf("Failed to add CAN socket to epoll.\n");
        return false;
    }

    if(_epoll.add(http_fd,EPOLLIN|EPOLLET) == -1)
    {
        printf("Failed to add HTTP socket to epoll");
        return false;
    }

    //这句话的意思是将tcp_fd插入进哈希表，并且属于Tcpserve
    fd_table.emplace(tcp_fd, FdType::Tcpserve);
    fd_table.emplace(http_fd,FdType::Httpserve);

    if(_storage.open("/home/qxc/Desktop/EdgeLink/edgehub/data/edgehub.db") == false)
    {
        printf("Failed to open SQLite database: /home/qxc/Desktop/Mini_Edgehub/data/edgehub.db\n");
        return false;
    }
    if(_storage.createTable() == false)
    {
        printf("Failed to create or validate SQLite table.\n");
        return false;
    }

    return true;
}

/*
    事件循环入口：持续等待 epoll 的就绪事件，并逐个交给 dispatchEvent()。
    EINTR 只是信号暂时打断等待，继续等待；其他 epoll 错误才结束运行。
*/
bool Gateruntime::run(volatile sig_atomic_t *g_running)
{
    struct epoll_event events[EPOLLEVENT_SIZE];
    while(*g_running != 0)
    {
        int timeout_ms = -1;
        for(const auto& client : httpclients)
        {
            if(client && client->waiting_slot_reply)
            {
                timeout_ms = 100;
                break;
            }
        }
        int nready = _epoll.wait(events, EPOLLEVENT_SIZE, timeout_ms);
        if(nready == -1)
        {
            //修复：EINTR只是epoll_wait被信号临时中断，重新等待就可以；其他错误才说明事件循环无法继续
            if(errno == EINTR)
            {
                continue;
            }
            perror("epoll error");
            return false;
        }

        for(int event_index = 0;event_index < nready;event_index++)
        {
            int fd = events[event_index].data.fd;
            dispatchEvent(fd, events[event_index].events);
        }
        serviceFirmwareSlotQueries();
    }

    return true;
}

/*
    根据 fd_table 中记录的 FdType 分发本次事件。
    输入是就绪 fd 与 epoll 事件掩码；找不到 fd 表示它已被清理，直接忽略。
    将来新增 UART、定时器或 eventfd 时，只需新增 FdType 和这里的分支。
*/
bool Gateruntime::dispatchEvent(int fd, uint32_t event_mask)
{
    auto fd_iterator = fd_table.find(fd);
    if(fd_iterator == fd_table.end())
    {
        return false;
    }

    switch(fd_iterator->second)
    {
        case FdType::Tcpserve:
            return handleTcpServe();
        case FdType::Tcpclient:
            return handleTcpclient(fd, event_mask);
        case FdType::Can:
            return handleCan(event_mask);
        case FdType::Httpserve:
            return handleHttpServe();
        case FdType::Httpclient:
            return handleHttpClient(fd,event_mask);
    }

    return false;
}

/*
    处理 TCP 监听 socket 的 EPOLLIN：在 ET 模式下持续 accept，直到 EAGAIN。
    每个连接占用一个 ClientState 槽位；没有槽位时明确拒绝新连接。
    本函数只处理连接建立，不读取客户端业务数据。
*/
bool Gateruntime::handleTcpServe()
{
    int client_fd;
    while(true)
    {
        client_fd = _tcpserve.client_accept();
        if(client_fd == -1)
        {
            /*
                我们来解释一下这一段是什么意思
                 client_accept() 返回 -1，说明本次 accept 没有成功拿到新的客户端连接。
                但是返回 -1 并不一定代表真正发生了错误，
                我们需要通过 errno 判断具体是什么情况。
                EAGAIN / EWOULDBLOCK：
                    当前 listen socket 使用的是非阻塞模式，
                    这两个错误表示当前已经没有新的客户端连接可以 accept 了。
                    因为我们使用的是 epoll ET 模式，所以收到一次 EPOLLIN 通知后，
                    必须不断调用 accept()，把当前已经排队的连接全部取出来，
                    一直取到 accept() 返回 EAGAIN / EWOULDBLOCK，
                    才说明本次连接队列已经被我们处理干净。
                    因此这里使用 break，退出 accept 循环。
                EINTR：
                    accept() 在执行过程中被信号中断了，
                    这不代表 socket 本身发生错误，
                    因此直接 continue，再重新调用一次 accept()。
                其他错误：
                    说明 accept() 出现了真正的异常，
                    当前无法继续正常接收客户端，因此退出本次 accept 循环。
                    实际工程中这里最好额外记录 errno 和错误日志。
            */
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            if(errno == EINTR)
            {
                continue;
            }
            break;
        }
        int slot = findFreeClientSlot();
        if(slot == -1)
        {
            //这里就是当有新的客户端连接但是我们没有空余的clients了
            rejectClient(client_fd);
            continue;
        }
        if(registerTcpclient(client_fd, slot) == false)
        {
            continue;
        }
    }
    return true;
}

/*
    查找 clients 数组中第一个空闲槽位。
    返回 [0, MAX_CLIENTS) 的下标；返回 -1 表示并发客户端数已到上限。
*/
int Gateruntime::findFreeClientSlot() const
{
    for(int slot = 0;slot < MAX_CLIENTS;slot++)
    {
        if(clients[slot] == nullptr)
        {
            return slot;
        }
    }

    return -1;
}

/*
    根据客户端 socket fd 查找它实际拥有的 ClientState 槽位。
    fd_table 与 clients 理应同步；返回 -1 表示两者登记已经不一致。
*/
int Gateruntime::findClientSlot(int fd) const
{
    for(int slot = 0;slot < MAX_CLIENTS;slot++)
    {
        if(clients[slot] != nullptr && clients[slot]->connection.fd() == fd)
        {
            return slot;
        }
    }

    return -1;
}

/*
    将一个已 accept 的 fd 注册为可服务客户端：创建 ClientState、设为非阻塞、
    加入 epoll，并在 fd_table 中标注 FdType::Tcpclient。
    中途失败时 reset ClientState，由 TcpConnection 析构关闭该 fd，不留下半注册连接。
*/
bool Gateruntime::registerTcpclient(int client_fd, int slot)
{
    clients[slot] = std::make_unique<ClientState>(client_fd);
    if(clients[slot]->connection.setnoblocking() == false)
    {
        clients[slot].reset();
        return false;
    }
    if(_epoll.add(client_fd, EPOLLIN | EPOLLRDHUP | EPOLLET) == -1)
    {
        clients[slot].reset();
        return false;
    }

    fd_table.emplace(client_fd, FdType::Tcpclient);
    return true;
}

/*
    当前客户端容量已满时的拒绝策略：记录原因并立即关闭刚 accept 的 fd。
    它没有进入 clients、fd_table 或 epoll，因此只需直接 close。
*/
void Gateruntime::rejectClient(int client_fd)
{
    //对于额外的客户端连接暂时没有想好要做什么直接关闭
    printf("Too many clients, closing new connection.\n");
    close(client_fd);
}

/*
    处理单个客户端的事件：先定位 ClientState，再读完 EPOLLIN 数据，最后处理
    ERR/RDHUP/HUP 的关闭。关闭路径统一交给 closeTcpclient()，不影响其他客户端。
*/
bool Gateruntime::handleTcpclient(int fd,uint32_t event_mask)
{
    int slot = findClientSlot(fd);
    //必须遍历完clients以后再判断是否找到，否则第一个位置不匹配就会提前结束整个服务端
    if(slot == -1)
    {
        closeUntrackedTcpclient(fd);
        return false;
    }
    //检查本次 epoll 事件中是否包含 EPOLLERR，如果包含，就把 close_client 设为 true。
    //EPOLLERR:EPOLL 错误
    bool close_client = (event_mask & EPOLLERR) != 0;
    //判断本次epoll事件不包含EPOLLERR并且包含EPOLLIN（可读）
    if(close_client == false && (event_mask & EPOLLIN) != 0)
    {
        drainTcpclient(slot, close_client);
    }
    //修复：同一个事件可能同时带EPOLLIN和关闭标志，上面先读完最后一批数据，再根据RDHUP/HUP进入清理流程
    if((event_mask & (EPOLLRDHUP | EPOLLHUP)) != 0)
    {
        close_client = true;
    }

    if(close_client)
    {
        closeTcpclient(fd, slot);
    }

    return true;
}

/*
    从一个非阻塞客户端持续 recv，直到 EAGAIN，满足 EPOLLET 的“必须读空”要求。
    收到的真实字节先写入该客户端的 Ringbuffer，再调用 parseTcpFrames()。
    close_client 以引用返回：连接关闭、缓冲区写入失败或不可恢复 recv 错误时设为 true。
*/
void Gateruntime::drainTcpclient(int slot, bool &close_client)
{
    uint8_t data_tmp[READ_BUFFER_LENGTH]{};
    while(true)
    {
        int ringbuffer_space_length = clients[slot]->receive_ringbuffer.free_space();
        if(ringbuffer_space_length <= 0)
        {
            close_client = true;
            break;
        }

        size_t receive_length = READ_BUFFER_LENGTH;
        if(static_cast<size_t>(ringbuffer_space_length) < receive_length)
        {
            receive_length = static_cast<size_t>(ringbuffer_space_length);
        }

        ssize_t receive_result = clients[slot]->connection.data_receive(data_tmp, receive_length);
        if(receive_result > 0)
        {
            int write_result = clients[slot]->receive_ringbuffer.write(
                data_tmp, static_cast<unsigned int>(receive_result));
            if(write_result != receive_result)
            {
                close_client = true;
                break;
            }

            parseTcpFrames(slot, close_client);
            if(close_client)
            {
                break;
            }
            continue;
        }

        if(receive_result == 0)
        {
            close_client = true;
            break;
        }
        if(errno == EINTR)
        {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            break;
        }

        close_client = true;
        break;
    }
}

/*
    从指定客户端的 Ringbuffer 连续取出完整 TCP 帧，转换为 Message 并写入 SQLite。
    FRAME_PARSE_PENDING 表示半包，保留数据等待下次 recv；解析错误则请求关闭该客户端。
    只有 SQLite 已成功写入或确认该键已存在，才向这个 TCP 连接回复 ACK。
*/
void Gateruntime::parseTcpFrames(int slot, bool &close_client)
{
    while(true)
    {
        TcpFrame frame{};
        Message message{};
        int parse_result = tcp_frame_parser(&clients[slot]->receive_ringbuffer, &frame);
        if(parse_result == FRAME_PARSE_SUCCESS)
        {
            Message_handle(&message, &frame);
            printf("TCP telemetry received: node=%u sequence=%u "
                "temperature_raw=%ld scale=%d\n",
                (unsigned int)message.nodeId,
                (unsigned int)message.sequence,
                (long)message.temperature,
                (int)message.temperatureScale);

            if(_storage.isOpen() == true)
            {
                StorageInsertResult insert_result = _storage.insertMessage(message);
                if(insert_result == StorageInsertResult::Error)
                {
                    std::string storage_error = _storage.getLastError();
                    fprintf(stderr, "%s\n", storage_error.c_str());
                }
                else if(sendTcpAck(slot, message) == false)
                {
                    /* 非阻塞 socket 只发出部分 ACK 时关闭连接，避免后续 ACK 拼接成损坏字节流。 */
                    close_client = true;
                    break;
                }
            }
            continue;
        }

        if(parse_result == FRAME_PARSE_PENDING)
        {
            break;
        }

        close_client = true;
        break;
    }
}

bool Gateruntime::sendTcpAck(int slot, const Message &message)
{
    uint8_t frame[FRAME_LENGTH]{};
    if(tcp_ack_encode(frame, message.nodeId, message.sequence, 0U) == false)
    {
        return false;
    }

    ssize_t sent_length = clients[slot]->connection.data_send(
        reinterpret_cast<const char *>(frame), FRAME_LENGTH);
    return sent_length == static_cast<ssize_t>(FRAME_LENGTH);
}

bool Gateruntime::sendCanAck(const Message &message)
{
    uint8_t data[CAN_ACK_LENGTH]{};
    data[0] = static_cast<uint8_t>(message.sequence >> 24);
    data[1] = static_cast<uint8_t>(message.sequence >> 16);
    data[2] = static_cast<uint8_t>(message.sequence >> 8);
    data[3] = static_cast<uint8_t>(message.sequence);
    data[4] = 0U;
    return _can.send(CAN_ACK_BASE_ID + message.nodeId, data, CAN_ACK_LENGTH);
}

/*
    关闭一个已正常登记的客户端，按 epoll、fd_table、ClientState 的顺序撤销。
    ClientState reset 后，TcpConnection 析构函数负责真正关闭 socket fd。
*/
void Gateruntime::closeTcpclient(int fd, int slot)
{
    //按 epoll 登记、类型登记、ClientState 所有权的顺序统一清理。
    _epoll.del(fd, 0);
    fd_table.erase(fd);
    clients[slot].reset();
}

/*
    处理“fd_table 仍有 fd、但 clients 找不到 ClientState”的异常登记。
    为防止失效 fd 反复触发事件，删除 epoll 和 fd_table 记录后直接关闭该 fd。
*/
void Gateruntime::closeUntrackedTcpclient(int fd)
{
    //fd_table 存在但 ClientState 不存在，说明登记失效；不让它继续触发事件。
    _epoll.del(fd, 0);
    fd_table.erase(fd);
    close(fd);
}

bool Gateruntime::handleCan(unsigned int event_mask)
{
    if(event_mask & EPOLLIN) //这里判断如果数据是EPOLLIN，也就是可读数据的话
    {
        while(true)//既然这次已经收到通知了，我就一直 read，直到把当前 CAN 接收队列彻底读空，
        //因为我们是Epoll ET所以他只会告诉你来数据了，而我们要读取得把它读完
        {
            struct can_frame can_frame;
            int receive_result = _can.receive(can_frame);
            Message can_message{};
            //现在我们就收到数据了，要区分数据到底是来了多少

            //如果收到的数据和can_frame长度相等，也就意味着收到了完整的一帧，我们对这一帧进行解析然后存储
            if(receive_result == sizeof(can_frame))
            {
                uint32_t can_id = can_frame.can_id & CAN_SFF_MASK;
                uint16_t temperature_raw;

                // OTA 帧先分流，不进入遥测入库和遥测 ACK 流程。
                if(handleCanOtaFrame(can_frame))
                {
                    continue;
                }

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
                    (static_cast<uint32_t>(can_frame.data[0]) << 24) |
                    (static_cast<uint32_t>(can_frame.data[1]) << 16) |
                    (static_cast<uint32_t>(can_frame.data[2]) << 8) |
                    static_cast<uint32_t>(can_frame.data[3]);
                temperature_raw =
                    (static_cast<uint16_t>(can_frame.data[4]) << 8) |
                    static_cast<uint16_t>(can_frame.data[5]);
                can_message.temperature = temperature_raw <= 0x7FFFU ?
                    static_cast<int16_t>(temperature_raw) :
                    static_cast<int16_t>(static_cast<int32_t>(temperature_raw) - 0x10000L);
                can_message.temperatureScale =
                    static_cast<int8_t>(can_frame.data[6]);
                can_message.receivedAtUs = frame_received_at_us();

                printf("CAN rx: id=0x%03X node=%u seq=%u temp_raw=0x%04X temp=%d scale=%d t=%llu\r\n",
                    can_id,
                    can_message.nodeId,
                    can_message.sequence,
                    temperature_raw,
                    can_message.temperature,
                    can_message.temperatureScale,
                    (unsigned long long)can_message.receivedAtUs);

                StorageInsertResult insert_result = _storage.insertMessage(can_message);
                if(insert_result == StorageInsertResult::Error)
                {
                    fprintf(stderr, "%s\n", _storage.getLastError().c_str());
                }
                else if(sendCanAck(can_message) == false)
                {
                    fprintf(stderr, "CAN ACK send failed: node=%u sequence=%u\n",
                        static_cast<unsigned int>(can_message.nodeId),
                        static_cast<unsigned int>(can_message.sequence));
                }

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
    return true;
}


bool Gateruntime::handleHttpServe()
{
    int httpclient_fd;
    while(true)
    {
        httpclient_fd = _httpserve.client_accept();
        if(httpclient_fd == -1)
        {
            if(errno == EAGAIN || errno == EWOULDBLOCK)
            {
                break;
            }
            if(errno == EINTR)
            {
                continue;
            }
            break;
        }
        //现在我们要给我们的httpclient安排一个空位
        int slot = findFreeHttpClientSlot();
        if(slot == -1)
        {
            //拒绝新连接
            rejectClient(httpclient_fd);
            continue;
        }
        if(registerHttpClient(httpclient_fd, slot) == false)
        {
            continue;
        }
    }

    return true;
}

int Gateruntime::findFreeHttpClientSlot() const
{
    for(int slot = 0;slot < MAX_CLIENTS;slot++)
    {
        if(httpclients[slot] == nullptr)
        {
            return slot;
        }
    }
    return -1;
}

bool Gateruntime::registerHttpClient(int client_fd, int slot)
{
    httpclients[slot] = std::make_unique<HttpClientState>(client_fd);
    if(httpclients[slot]->connection.setnoblocking() == false)
    {
        httpclients[slot].reset();
        return false;
    }
    if(_epoll.add(client_fd, EPOLLIN | EPOLLRDHUP | EPOLLET) == -1)
    {
        httpclients[slot].reset();
        return false;
    }

    fd_table.emplace(client_fd, FdType::Httpclient);
    return true;
}


bool Gateruntime::handleHttpClient(int fd,uint32_t event_mask)
{
    int slot = findHttpClientSlot(fd);
    if(slot == -1)
    {
        _epoll.del(fd, 0);
        fd_table.erase(fd);
        close(fd);
        return false;
    }

    bool close_client = (event_mask & (EPOLLERR | EPOLLHUP)) != 0;
    HttpClientState& client = *httpclients[slot];
    if(close_client == false && client.response.empty() && !client.waiting_slot_reply)
    {
        // 对端停止发送也要先读取最后的数据，完整请求仍然可以回复。
        if((event_mask & (EPOLLIN | EPOLLRDHUP)) != 0)
        {
            drainHttpclient(slot, close_client);
        }
        if(close_client == false && client.http.parsed())
        {
            HttpHandle handler;
            handler.handle(client.http.request());
            if(handler.firmwareQueryNode() != 0U)
            {
                startFirmwareSlotQuery(slot, handler.firmwareQueryNode());
            }
            else
            {
                client.response = Http::serialize(handler.statusCode(), handler.responseBody());
            }
        }
        else if((event_mask & EPOLLRDHUP) != 0)
        {
            close_client = true;
        }
    }

    if(close_client == false && client.response.empty() == false)
    {
        sendHttpResponse(slot, close_client);
    }
    if(close_client)
    {
        _epoll.del(fd, 0);
        fd_table.erase(fd);
        httpclients[slot].reset();
    }
    return true;
}

int Gateruntime::findHttpClientSlot(int fd) const
{
    for(int slot = 0;slot < MAX_CLIENTS;slot++)
    {
        if(httpclients[slot] != nullptr && httpclients[slot]->connection.fd() == fd)
        {
            return slot;
        }
    }
    return -1;
}

void Gateruntime::drainHttpclient(int slot, bool &close_client)
{
    // 第一版每条连接只处理一个请求，完整请求保留给后续业务处理。
    if(httpclients[slot]->http.parsed())
    {
        return;
    }

    uint8_t data_tmp[READ_BUFFER_LENGTH]{};
    while(true)
    {
        ssize_t receive_result = httpclients[slot]->connection.data_receive(
            data_tmp, sizeof(data_tmp));
        if(receive_result > 0)
        {
            static constexpr size_t HTTP_MAX_REQUEST_BYTES = 1024U * 1024U;
            size_t received_length = static_cast<size_t>(receive_result);
            if(received_length > HTTP_MAX_REQUEST_BYTES - httpclients[slot]->received_bytes)
            {
                close_client = true;
                return;
            }
            httpclients[slot]->received_bytes += received_length;
            httpclients[slot]->http.feed(data_tmp, received_length);
            if(httpclients[slot]->http.poll())
            {
                return;
            }
            continue;
        }

        if(receive_result == 0)
        {
            // 请求尚未收完整，对端已结束发送，无法继续解析。
            close_client = true;
            return;
        }
        if(errno == EINTR)
        {
            continue;
        }
        if(errno == EAGAIN || errno == EWOULDBLOCK)
        {
            // 当前读空，Http 中的半包保留到下一次可读事件。
            return;
        }

        close_client = true;
        return;
    }
}

void Gateruntime::sendHttpResponse(int slot, bool &close_client)
{
    HttpClientState& client = *httpclients[slot];
    while(client.sent_bytes < client.response.size())
    {
        ssize_t sent = client.connection.data_send(
            client.response.data() + client.sent_bytes,
            client.response.size() - client.sent_bytes);
        if(sent > 0)
        {
            client.sent_bytes += static_cast<size_t>(sent);
            continue;
        }
        if(sent == -1 && errno == EINTR)
        {
            continue;
        }
        if(sent == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
        {
            // 只在有剩余响应时监听可写事件，稍后从 sent_bytes 继续。
            if(_epoll.mod(client.connection.fd(), EPOLLOUT | EPOLLET) == -1)
            {
                close_client = true;
            }
            return;
        }
        close_client = true;
        return;
    }

    // 第一版一条连接处理一个请求，响应全部发送后关闭。
    close_client = true;
}


bool Gateruntime::handleCanOtaFrame(const can_frame& frame)
{
    if((frame.can_id & (CAN_EFF_FLAG | CAN_RTR_FLAG | CAN_ERR_FLAG)) != 0U)
    {
        return false;
    }

    uint32_t id = frame.can_id & CAN_SFF_MASK;
    uint32_t base;
    const char* kind;
    if(id >= CAN_OTA_DATA_BASE_ID && id < CAN_OTA_DATA_BASE_ID + CAN_ID_SEGMENT_SIZE)
    {
        base = CAN_OTA_DATA_BASE_ID;
        kind = "data";
    }
    else if(id >= CAN_OTA_CTRL_BASE_ID && id < CAN_OTA_CTRL_BASE_ID + CAN_ID_SEGMENT_SIZE)
    {
        base = CAN_OTA_CTRL_BASE_ID;
        kind = "control";
    }
    else if(id >= CAN_OTA_REPLY_BASE_ID && id < CAN_OTA_REPLY_BASE_ID + CAN_ID_SEGMENT_SIZE)
    {
        base = CAN_OTA_REPLY_BASE_ID;
        kind = "reply";
    }
    else
    {
        return false;
    }

    uint32_t node = id - base;
    // 节点 0 保留；控制载荷尚未定义，只校验经典 CAN 的长度范围。
    if(node == 0U || frame.can_dlc > CAN_MAX_DLEN ||
       (base == CAN_OTA_DATA_BASE_ID && frame.can_dlc != CAN_OTA_DATA_LENGTH) ||
       (base == CAN_OTA_REPLY_BASE_ID && frame.can_dlc != CAN_OTA_REPLY_LENGTH &&
        frame.can_dlc != 8U))
    {
        fprintf(stderr, "CAN OTA invalid: id=0x%03X dlc=%u\n", id,
            static_cast<unsigned int>(frame.can_dlc));
        return true;
    }

    if(base == CAN_OTA_REPLY_BASE_ID)
    {
        if(frame.data[0] == CAN_OTA_REPLY_KIND_SLOT && frame.can_dlc == 8U)
        {
            finishFirmwareSlotQuery(static_cast<uint8_t>(node), frame);
            return true;
        }
        if(frame.can_dlc != CAN_OTA_REPLY_LENGTH || frame.data[0] == CAN_OTA_REPLY_KIND_SLOT)
        {
            fprintf(stderr, "CAN OTA invalid reply length: node=%u kind=%u dlc=%u\n",
                node, static_cast<unsigned int>(frame.data[0]),
                static_cast<unsigned int>(frame.can_dlc));
            return true;
        }
        uint16_t contiguous = (static_cast<uint16_t>(frame.data[1]) << 8) |
            static_cast<uint16_t>(frame.data[2]);
        if(frame.data[0] == CAN_OTA_REPLY_KIND_PROGRESS)
        {
            printf("CAN OTA progress: node=%u contiguous=%u\n", node,
                static_cast<unsigned int>(contiguous));
        }
        else if(frame.data[0] == CAN_OTA_REPLY_KIND_RESULT && frame.data[3] <= 1U)
        {
            printf("CAN OTA result: node=%u success=%u\n", node,
                static_cast<unsigned int>(frame.data[3]));
        }
        else
        {
            fprintf(stderr, "CAN OTA invalid reply: node=%u kind=%u success=%u\n",
                node, static_cast<unsigned int>(frame.data[0]),
                static_cast<unsigned int>(frame.data[3]));
        }
        return true;
    }

    // 数据/控制帧正常方向为 Hub -> Node，收到时仅记录，不执行升级动作。
    printf("CAN OTA %s observed: node=%u dlc=%u data=", kind, node,
        static_cast<unsigned int>(frame.can_dlc));
    for(uint8_t index = 0; index < frame.can_dlc; ++index)
    {
        printf("%02X", static_cast<unsigned int>(frame.data[index]));
    }
    printf("\n");
    return true;
}


void Gateruntime::startFirmwareSlotQuery(int slot, uint8_t node)
{
    HttpClientState& client = *httpclients[slot];
    bool in_use;
    //这里的next_query是curl的查询编号，不是node编号
    do
    {
        ++next_query_id;
        if(next_query_id == 0U)
        {
            ++next_query_id;
        }
        in_use = false;
        for(const auto& pending : httpclients)
        {
            if(pending && pending->waiting_slot_reply && pending->query_id == next_query_id)
            {
                in_use = true;
                break;
            }
        }
    } while(in_use);

    client.query_node = node;
    client.query_id = next_query_id;
    uint8_t data[4] = {1U, static_cast<uint8_t>(client.query_id >> 8),
        static_cast<uint8_t>(client.query_id), 0U};
    if(!_can.send(CAN_OTA_CTRL_BASE_ID + node, data, sizeof(data)))
    {
        client.response = Http::serialize(503, "{\"error\":\"CAN query send failed\"}\n");
        return;
    }
    client.waiting_slot_reply = true;
    client.query_deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
}

void Gateruntime::finishFirmwareSlotQuery(uint8_t node, const can_frame& frame)
{
    uint16_t request_id = (static_cast<uint16_t>(frame.data[1]) << 8) | frame.data[2];
    uint32_t target = (static_cast<uint32_t>(frame.data[4]) << 24) |
        (static_cast<uint32_t>(frame.data[5]) << 16) |
        (static_cast<uint32_t>(frame.data[6]) << 8) | frame.data[7];
    for(int slot = 0; slot < MAX_CLIENTS; ++slot)
    {
        if(!httpclients[slot] || !httpclients[slot]->waiting_slot_reply ||
           httpclients[slot]->query_node != node || httpclients[slot]->query_id != request_id)
        {
            continue;
        }
        HttpClientState& client = *httpclients[slot];
        client.waiting_slot_reply = false;
        int status = 502;
        std::string body = "{\"error\":\"invalid node query reply\"}\n";
        if(std::chrono::steady_clock::now() >= client.query_deadline)
        {
            status = 504;
            body = "{\"error\":\"node query timed out\"}\n";
        }
        else if(frame.data[3] == 0U &&
                (target == OTA_SLOT_A_BASE_ADDRESS || target == OTA_SLOT_B_BASE_ADDRESS))
        {
            status = 200;
            body = (target == OTA_SLOT_A_BASE_ADDRESS) ?
                "{\"slot\":\"A\"}\n" : "{\"slot\":\"B\"}\n";
        }
        else if(frame.data[3] == 1U && target == 0U)
        {
            body = "{\"error\":\"node metadata invalid\"}\n";
        }
        else if(frame.data[3] == 2U && target == 0U)
        {
            status = 409;
            body = "{\"error\":\"node has an unconfirmed slot\"}\n";
        }
        client.response = Http::serialize(status, body);
        // 本批事件处理完再发送，避免在 CAN 回调中关闭另一个仍有事件的 fd。
        return;
    }
}

void Gateruntime::serviceFirmwareSlotQueries()
{
    const auto now = std::chrono::steady_clock::now();
    for(int slot = 0; slot < MAX_CLIENTS; ++slot)
    {
        if(httpclients[slot] && httpclients[slot]->waiting_slot_reply &&
           now >= httpclients[slot]->query_deadline)
        {
            HttpClientState& client = *httpclients[slot];
            client.waiting_slot_reply = false;
            client.response = Http::serialize(504, "{\"error\":\"node query timed out\"}\n");
        }
        if(httpclients[slot] && !httpclients[slot]->waiting_slot_reply &&
           httpclients[slot]->query_id != 0U && !httpclients[slot]->response.empty())
        {
            // 此函数只在当前 epoll 事件批次结束后执行。
            httpclients[slot]->query_id = 0U;
            handleHttpClient(httpclients[slot]->connection.fd(), 0U);
        }
    }
}
