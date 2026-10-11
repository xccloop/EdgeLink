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

#include "Gateruntime.hpp"

#include <csignal>   // signal / SIGINT / sig_atomic_t
#include <cstdio>    // printf / stdout / setvbuf / _IONBF

static volatile sig_atomic_t g_running = 1;
static void sig_handler(int) { g_running = 0; }

int main()
{
    /* stdout 接到文件或管道时，默认是"全缓冲"（攒够 4KB 才写出去）。
       程序一旦进入事件循环就永不退出，日志会一直卡在缓冲区里看不见。
       板子上没有 stdbuf 命令，所以只能在这里自己关掉缓冲。
       _IONBF = 不缓冲，每条 printf 立刻写出去。
       （想减少系统调用可以换成 _IOLBF 行缓冲，但那样每句都得带 \n。） */
    setvbuf(stdout, NULL, _IONBF, 0);

    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);

    Gateruntime runtime;

    printf("EdgeHub starting...\n");

    if(runtime.init() == false)
    {
        return 1;
    }
    
    if(runtime.run(&g_running) == false)
    {
        return 1;
    }

    return 0;
}