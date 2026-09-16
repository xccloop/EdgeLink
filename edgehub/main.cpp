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

static volatile sig_atomic_t g_running = 1;
static void sig_handler(int) { g_running = 0; }

int main()
{
    signal(SIGINT, sig_handler);
    signal(SIGTERM, sig_handler);
    signal(SIGPIPE, SIG_IGN);
    
    Gateruntime runtime;
    
    runtime.init();
    
    runtime.run(&g_running);
}