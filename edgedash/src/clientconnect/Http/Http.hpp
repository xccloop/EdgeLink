#pragma once

#include <string>

// 查询 Hub 运行状态，对应 GET /hub/status。
#define STAUS 1

class Http
{
public:
    // 按类别组装请求并保存；返回值只表示组装是否成功。
    bool assemble(int type);

    std::string getRequest() const;

    // 解析调用方收齐的 Hub GET 响应，只读取状态码和 Content-Length 正文。
    // 不检查其他业务头字段；长度不符或不支持的传输格式返回 false。
    bool parseResponse(const std::string &message);
    std::string getResponse() const;
    int getStatusCode() const;
    std::string getResponseBody() const;

private:
    std::string request;
    std::string response;
};
