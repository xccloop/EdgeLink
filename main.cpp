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

#include "Ringbuffer.hpp"
#include "Tcp.hpp"
#include <cstdio>
#include <sys/types.h>

#define NETWORK_PORT 9527
#define BACKLOG 5
#define RINGBUFFER_SIZE 240

int main()
{
    TcpServe serve(NETWORK_PORT,BACKLOG);
    Ringbuffer buffer(RINGBUFFER_SIZE);
    char receive_buffer[RINGBUFFER_SIZE];
    char send_buffer[RINGBUFFER_SIZE];

    if(serve.init() == false)
    {
        perror("Tcp init fail");
        return 1;
    }

    int clinet_fd = serve.client_accept();
    if(clinet_fd == -1)
    {
        perror("Cilent init fail");
        return 1;
    }

    TcpConnection connection(clinet_fd);

    ssize_t receive_buffer_length = connection.data_receive(receive_buffer, RINGBUFFER_SIZE);
    if(receive_buffer_length < 0)
    {
        perror("receive fail");
        return 1;
    }
    if(receive_buffer_length == 0)
    {
        return 0;
    }

    //这里的static_cast<unsigned int>叫做显示转化类型，因为我的buffer.write()的length要求unsigned int类型，但是之前定义的receive_buffer_length为ssize_t类型
    //转化后相当于我确认这里是正数所以转化成无符号int
    int actually_receive_data_length = buffer.write(receive_buffer, static_cast<unsigned int>(receive_buffer_length));
    if(actually_receive_data_length <= 0)
    {
        fprintf(stderr, "Ringbuffer write fail\n");//stderr叫做标准错误输出
        return 1;
    }

    int actually_read_data_length = buffer.read(send_buffer, static_cast<unsigned int>(actually_receive_data_length));
    if(actually_read_data_length <= 0)
    {
        fprintf(stderr, "Ringbuffer read fail\n");
        return 1;
    }

    ssize_t send_data_length = connection.data_send(send_buffer, static_cast<size_t>(actually_read_data_length));
    if(send_data_length != actually_read_data_length)
    {
        if(send_data_length < 0)
        {
            perror("send fail");
        }
        return 1;
    }

    return 0;
}

