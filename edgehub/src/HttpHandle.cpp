#include "Httphandle.hpp"
#include "Http.hpp"
#include <string.h>
#include <string>
#include <stdlib.h>

const HttpHandle::Route HttpHandle::routes[] = 
{
    {"/hub/status",&HttpHandle::hanlde_hubstatus}
};

//初始构造清空
HttpHandle::HttpHandle()
    :status(0),
    body{}
{
};

void HttpHandle::clear()
{
    this->status = 0;
    body.clear();
}

//这个函数就是负责解析业务的函数
void HttpHandle::handle(HttpRequest request)
{
    //为了防止长期持有结果，每次调用逻辑必须先清空
    clear();

    this->status = 404;

    std::string method = request.method;
    std::string path = request.path;

    size_t routes_count = sizeof(routes) / sizeof(routes[0]);

    //这个时候获取参数信息
    if(method == "GET")
    {
        /*
            遍历路由表，根据 path 找到对应业务处理函数。
            routes[index].handler 保存的是 HttpHandle 的成员函数指针。
            (this->*routes[index].handler)()
            表示使用当前 HttpHandle 对象调用该成员函数。
            例如：
            handler = &HttpHandle::handle_nodes
            那么调用效果等价于：
            this->handle_nodes();
            这种好处就是我们不需要再去写一大段if-else了，现在当我们增加一个新的命令只需要在表中增加并且增加对应的业务就好了
        */
        for(int index = 0;index < routes_count;++index)
        {
            if(strcmp(routes[index].path.c_str(), path.c_str()) == 0)
            {
                (this ->* routes[index].handler) ();
                return;
            }
        }
    }
}

void HttpHandle::hanlde_hubstatus()
{
    this->status = 200;
    //这种写法是json写法
    this->body =  "{\"running\":true}";
}