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

//现在我们来着手实现非阻塞服务端代码

#include "Ringbuffer.hpp"
#include "Tcp.hpp"
#include "Epoll.hpp"
#include <array>
#include <cerrno>
#include <cstdio>
#include <memory>
#include <sys/epoll.h>
#include <unistd.h>

#define SERVE_PORT 8888
#define SERVE_BACKLOG 5
#define EPOLL_EVENTS_LENGTH 10
#define RINGBUFFER_LENGTH 240
#define MAX_CLIENTS 10

struct ClientState
{
    explicit ClientState(int fd)
        : connection(fd), send_ringbuffer(RINGBUFFER_LENGTH)
    {
    }

    TcpConnection connection;
    Ringbuffer send_ringbuffer;
    char pending_send[RINGBUFFER_LENGTH];
    unsigned int pending_offset{0};
    unsigned int pending_length{0};
    bool peer_read_closed{false};
};

int main()
{
    TcpServe serve(SERVE_PORT,SERVE_BACKLOG);
    Epoll epoll;
    std::array<std::unique_ptr<ClientState>, MAX_CLIENTS> clients{};

    char read_data[10];

    bool is_tcpcreate = serve.init();
    if(is_tcpcreate == false)
    {
        fprintf(stderr, "TCP serve init fail");
        return -1;
    }

    bool is_tcpblocking = serve.setnoblocking();
    if(is_tcpblocking == false)
    {
        fprintf(stderr,"TCP set blocking fail");
        return -1;
    }

    int tcp_fd = serve.fd();

    int epoll_fd = epoll.create();
    if(epoll_fd == -1)
    {
        fprintf(stderr,"Epoll fd create fail");
        return -1;
    }

    if(epoll.add(tcp_fd,EPOLLIN | EPOLLET) == -1)
    {
        fprintf(stderr,"Epoll add tcp fd fail");
        return -1;
    }


    struct epoll_event events[EPOLL_EVENTS_LENGTH];
    while(1)
    {
        int nready = epoll.wait(events, EPOLL_EVENTS_LENGTH);
        if(nready == -1)
        {
            if(errno == EINTR)
            {
                continue;
            }
            fprintf(stderr,"epoll wait create fail");
            return -1;
        }

        for(int epoll_index = 0;epoll_index < nready;epoll_index++)
        {
            int fd = events[epoll_index].data.fd;
            if(fd == tcp_fd)
            {
                while(true)
                {
                    int client_fd = serve.client_accept();
                    if(client_fd == -1)
                    {
                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        if(errno == EINTR)
                        {
                            continue;
                        }
                        perror("client accept fail");
                        break;
                    }

                    std::unique_ptr<ClientState>* empty_slot = nullptr;
                    for(auto& client : clients)
                    {
                        if(client == nullptr)
                        {
                            empty_slot = &client;
                            break;
                        }
                    }

                    if(empty_slot == nullptr)
                    {
                        close(client_fd);
                        continue;
                    }

                    std::unique_ptr<ClientState> client = std::make_unique<ClientState>(client_fd);
                    if(client->connection.setnoblocking() == false)
                    {
                        continue;
                    }

                    if(epoll.add(client_fd, EPOLLIN | EPOLLRDHUP) == -1)
                    {
                        continue;
                    }

                    *empty_slot = std::move(client);
                }
            }
            else
            {
                unsigned int event_mask = events[epoll_index].events;
                std::unique_ptr<ClientState>* client_slot = nullptr;
                for(auto& client : clients)
                {
                    if(client != nullptr && client->connection.fd() == fd)
                    {
                        client_slot = &client;
                        break;
                    }
                }

                if(client_slot == nullptr)
                {
                    continue;
                }

                ClientState& client = **client_slot;
                bool close_client = (event_mask & EPOLLERR) != 0;

                if(close_client == false && (event_mask & EPOLLIN) != 0)
                {
                    while(true)
                    {
                        int free_space = client.send_ringbuffer.free_space();
                        if(free_space <= 0)
                        {
                            break;
                        }

                        size_t receive_length = sizeof(read_data);
                        if(static_cast<unsigned int>(free_space) < receive_length)
                        {
                            receive_length = static_cast<unsigned int>(free_space);
                        }

                        ssize_t receive_length_result = client.connection.data_receive(read_data, receive_length);
                        if(receive_length_result > 0)
                        {
                            int queued_length = client.send_ringbuffer.write(read_data, static_cast<unsigned int>(receive_length_result));
                            if(queued_length != receive_length_result)
                            {
                                fprintf(stderr, "Ringbuffer write fail\n");
                                close_client = true;
                                break;
                            }
                            continue;
                        }

                        if(receive_length_result == 0)
                        {
                            client.peer_read_closed = true;
                            break;
                        }

                        if(errno == EAGAIN || errno == EWOULDBLOCK)
                        {
                            break;
                        }
                        if(errno == EINTR)
                        {
                            continue;
                        }

                        perror("client receive fail");
                        close_client = true;
                        break;
                    }
                }

                if((event_mask & (EPOLLRDHUP | EPOLLHUP)) != 0)
                {
                    client.peer_read_closed = true;
                }

                if(close_client == false)
                {
                    while(true)
                    {
                        if(client.pending_offset == client.pending_length)
                        {
                            client.pending_offset = 0;
                            client.pending_length = 0;

                            int pending_length = client.send_ringbuffer.read(client.pending_send, RINGBUFFER_LENGTH);
                            if(pending_length < 0)
                            {
                                break;
                            }
                            client.pending_length = static_cast<unsigned int>(pending_length);
                        }

                        ssize_t send_length_result = client.connection.data_send(
                            client.pending_send + client.pending_offset,
                            client.pending_length - client.pending_offset);
                        if(send_length_result > 0)
                        {
                            client.pending_offset += static_cast<unsigned int>(send_length_result);
                            continue;
                        }

                        if(send_length_result == -1 && (errno == EAGAIN || errno == EWOULDBLOCK))
                        {
                            break;
                        }
                        if(send_length_result == -1 && errno == EINTR)
                        {
                            continue;
                        }

                        perror("client send fail");
                        close_client = true;
                        break;
                    }
                }

                bool has_pending_data = client.pending_offset != client.pending_length
                    || client.send_ringbuffer.data_space() > 0;
                if(client.peer_read_closed && has_pending_data == false)
                {
                    close_client = true;
                }

                if(close_client)
                {
                    epoll.del(fd, 0);
                    client_slot->reset();
                    continue;
                }

                int client_events = EPOLLRDHUP;
                if(client.peer_read_closed == false)
                {
                    client_events |= EPOLLIN;
                }
                if(has_pending_data)
                {
                    client_events |= EPOLLOUT;
                }

                if(epoll.mod(fd, client_events) == -1)
                {
                    perror("client epoll mod fail");
                    epoll.del(fd, 0);
                    client_slot->reset();
                }
            }
        }
    }
}
