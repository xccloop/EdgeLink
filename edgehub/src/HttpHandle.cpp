#include "Httphandle.hpp"
#include "Http.hpp"
#include <string.h>
#include <string>
#include <stdlib.h>

const HttpHandle::Route HttpHandle::routes[] = 
{
    {"/hub/status",&HttpHandle::hanlde_hubstatus},
    {"/hub/firmware/query/node/", &HttpHandle::handleFirmwareSlotQuery, true},
    {"/hub/firmware/send/node/",&HttpHandle::handleFirmwareSlotSend,true}
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
    request_path.clear();
    query_node = 0;
}

//这个函数就是负责解析业务的函数
void HttpHandle::handle(HttpRequest request)
{
    //为了防止长期持有结果，每次调用逻辑必须先清空
    clear();

    this->status = 404;
    body = "{\"error\":\"not found\"}";

    std::string method = request.method;
    this->request_body = request.body;
    std::string path = request.path;
    request_path = path;

    size_t routes_count = sizeof(routes) / sizeof(routes[0]);

    if(method != "GET" && method != "POST")
    {
        status = 405;
        body = "{\"error\":\"method not allowed\"}";
        return;
    }

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
        for(size_t index = 0;index < routes_count;++index)
        {
            if((routes[index].prefix && path.compare(0, routes[index].path.size(), routes[index].path) == 0) ||
               (!routes[index].prefix && path == routes[index].path))
            {
                const bool is_send = routes[index].handler == &HttpHandle::handleFirmwareSlotSend;
                if((is_send && method != "POST") || (!is_send && method != "GET"))
                {
                    status = 405;
                    body = "{\"error\":\"method not allowed\"}";
                    return;
                }
                (this ->* routes[index].handler) ();
                return;
            }
        }
    }

    if(method == "POST")
    {
        for(size_t index = 0;index < routes_count;++index)
        {
            if((routes[index].prefix && path.compare(0, routes[index].path.size(), routes[index].path) == 0) ||
               (!routes[index].prefix && path == routes[index].path))
            {
                const bool is_send = routes[index].handler == &HttpHandle::handleFirmwareSlotSend;
                if((is_send && method != "POST") || (!is_send && method != "GET"))
                {
                    status = 405;
                    body = "{\"error\":\"method not allowed\"}";
                    return;
                }
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

void HttpHandle::handleFirmwareSlotQuery()
{
    const std::string prefix = "/hub/firmware/query/node/";
    std::string node_text = request_path.substr(prefix.size());
    unsigned int node = 0;
    status = 400;
    body = "{\"error\":\"node must be an integer from 1 to 127\"}";
    if(node_text.empty() || node_text.size() > 3U)
    {
        return;
    }
    for(char digit : node_text)
    {
        if(digit < '0' || digit > '9')
        {
            return;
        }
        node = node * 10U + static_cast<unsigned int>(digit - '0');
    }
    if(node == 0U || node > 127U)
    {
        return;
    }
    // 只提交查询意图，运行时负责异步 CAN 收发；这里不等待回复。
    query_node = static_cast<uint8_t>(node);
    status = 200;
    body.clear();
}

void HttpHandle::handleFirmwareSlotSend()
{
    const std::string prefix = "/hub/firmware/send/node/";
    std::string node_text = request_path.substr(prefix.size());
    unsigned int node = 0;
    status = 400;
    body = "{\"error\":\"node must be an integer from 1 to 127\"}";
        if(node_text.empty() || node_text.size() > 3U)
    {
        return;
    }
    for(char digit : node_text)
    {
        if(digit < '0' || digit > '9')
        {
            return;
        }
        node = node * 10U + static_cast<unsigned int>(digit - '0');
    }
    if(node == 0U || node > 127U)
    {
        return;
    }
    // 只提交查询意图，运行时负责异步 CAN 收发；这里不等待回复。
    firmware_node = static_cast<uint8_t>(node);
    firmware_path = request_body;
    status = 200;
    body.clear();
}