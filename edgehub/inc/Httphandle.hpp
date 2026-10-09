#pragma once

#include "Http.hpp"

//这个类负责将一个http的request解析然后处理业务逻辑返回status和body
class HttpHandle
{
public:
    HttpHandle();

    void handle(HttpRequest request);

    void clear();

    uint8_t firmwareQueryNode() const { return query_node; }
    uint8_t firmwareSendNode() const {return firmware_node;}
    std::string firmwareSendNodePath() const {return firmware_path;}
    int statusCode() const { return status; }
    const std::string& responseBody() const { return body; }

private:

    int status;

    std::string body;
    std::string request_path;
    uint8_t query_node{0};

    std::string request_body;
    uint8_t firmware_node = 0;
    std::string firmware_path;

    // 给“成员函数指针类型”起名为 Handler。
    //Handler 代表一个指向 HttpHandle 类成员函数的指针，这个成员函数无参数、返回 void。
    using Handler = void (HttpHandle::*)();

    // 定义表中“一行”的结构。
    struct Route
    {
        std::string path;
        Handler handler;
        bool prefix{false};
    };

    // 声明整张表，具体内容写在 cpp 中。
    static const Route routes[];

    void hanlde_hubstatus();
    void handleFirmwareSlotQuery();
    void handleFirmwareSlotSend();
};