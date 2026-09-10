#include <sys/epoll.h>

/*
    传统的epoll提供了三个常见的API
    epoll_create(): 创建一个 epoll 实例，返回一个文件描述符，用于后续的操作。
    epoll_ctl(): 用于向 epoll 实例中添加、修改或删除要监控的文件描述符及其事件。
    epoll_wait(): 阻塞等待，直到有一个或多个文件描述符上有事件发生，然后返回就绪的文件描述符列表。
    select,poll仅仅只用一个接口就可以实现，主要为效率问题，二者采用轮询而epoll是fd发生变化，资源占用和效率会快很多
*/

class Epoll {
public:
    Epoll();
    ~Epoll();

    Epoll(const Epoll&) = delete;
    Epoll& operator=(const Epoll&) = delete;
    //这两句话意味着：禁止拷贝构造函数和拷贝赋值运算符，确保这个类的对象“只能被移动，不能被复制

    int create();

    // 带事件参数（可传入 EPOLLIN | EPOLLET 等）
    int add(int fd, int epoll_event);
    int del(int fd, int epoll_event);
    int mod(int fd, int epoll_event);

    // 无参数重载：默认 LT（EPOLLIN）
    int add(int fd);
    int mod(int fd);

    int wait(struct epoll_event events[],int length,int timeout_ms = -1);

private:
    int epoll_fd;
};