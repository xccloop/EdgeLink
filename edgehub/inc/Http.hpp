#pragma once

/*
    Http —— 把 TCP 字节流换算成一条完整的 HTTP 请求，并把响应拼回字节流。

    这个类【完全不碰 socket】：
      - 它不 recv，也不 send。字节由调用方（Gateruntime）用 feed() 喂进来。
      - 它不知道 epoll、不知道 fd、不知道业务命令。
    这样它才能被单独测试：喂一串字节，看它解析出什么，不需要跑服务器。

    HTTP 与 CAN / TcpFrame 的根本差别：
      CAN 一帧 8 字节、TcpFrame 固定 16 字节 —— 长度固定，数够就是一条。
      HTTP 长度不固定：头部靠 "\r\n\r\n" 结束，身体长度靠 Content-Length 自己说。
      所以 recv 一次可能给半行、给完整请求、或者给一个半请求。
      => 必须把【未消费的字节】和【解析到哪一步】跨调用保留下来，这就是那三个成员变量。
*/

#include <cstdint>
#include <cstddef>
#include <string>

/* 一条【已解析完成】的请求。纯数据，没有函数，也没有状态。 */
struct HttpRequest
{
    std::string method;   // "GET" / "POST"，原文照存
    std::string path;     // "/api/nodes"，原文照存（不在这里拆参数）
    std::string body;     // 身体原文，长度由 Content-Length 决定
};

class Http
{
public:
    Http();

    /* 把新收到的字节追加进内部缓冲。Gateruntime 在 EPOLLIN 时调用。 */
    void feed(const uint8_t* data, size_t length);

    /* 尝试解析。够了 → 填好 request_ 并返回 true；不够 → 返回 false，等下次 feed。
       会被反复调用，所以必须能安全地重复调用（这就是 state_ 存在的理由）。 */
    bool poll();

    bool parsed() const;

    const HttpRequest& request() const;

    /* 把 status + body 拼成完整的 HTTP 响应文本（含状态行、头、空行）。
       静态：它不需要任何成员状态 —— 给一个状态码和一段 body，还一段字节。 */
    static std::string serialize(int status, const std::string& body);

private:
    /* 解析进度。必须跨调用存活：feed() 一次、poll() 一次，中间状态不能丢。 */
    enum class ParseState
    {
        WaitingHeader,   // 还没收到 "\r\n\r\n"
        WaitingBody,     // 头部好了，身体还差字节
        Complete         // 一条完整请求已就绪
    };

    /* 头里读出来的身体长度。初值 0（GET 请求没有这个头，也必须能解析成功）。 */
    size_t content_length_;

    /* 累积的未消费字节。recv 给多少收多少，不够就留着等下次。 */
    std::string buffer_;

    /* 解析进度。初始 WaitingHeader。 */
    ParseState state_;

    /* 解析结果。 */
    HttpRequest request_;
};
