#include "Http.hpp"

#include <cctype>
#include <cstdlib>

/* 单条请求（含头+身体）的字节上限。防止对端只发半截就一直占内存。 */
static constexpr size_t HTTP_MAX_REQUEST_BYTES = 1024U * 1024U;

/* 头部结束标记。HTTP 的行尾是 \r\n，所以是四个字节，不是 "\n\n"。 */
static constexpr char HTTP_HEADER_TERMINATOR[] = "\r\n\r\n";

/* 去掉字符串两端的空格和制表符。 */
static std::string trim(const std::string& text)
{
    size_t begin = 0;
    size_t end = text.size();
    while(begin < end && (text[begin] == ' ' || text[begin] == '\t'))
    {
        ++begin;
    }
    while(end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
    {
        --end;
    }
    return text.substr(begin, end - begin);
}

/* 头部名字比较：HTTP 里字段名不区分大小写。 */
static bool header_name_equals(const std::string& name, const char* expected)
{
    size_t index = 0;
    for(; expected[index] != '\0'; ++index)
    {
        //如果大于是false
        if(index >= name.size())
        {
            return false;
        }
        //忽略大小
        if(std::tolower(static_cast<unsigned char>(name[index])) !=
           std::tolower(static_cast<unsigned char>(expected[index])))
        {
            return false;
        }
    }
    return index == name.size();
}

/* 状态码 → 原因短语。 */
static const char* status_reason(int status)
{
    switch(status)
    {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 409: return "Conflict";
        case 413: return "Payload Too Large";
        case 500: return "Internal Server Error";
        case 502: return "Bad Gateway";
        case 503: return "Service Unavailable";
        case 504: return "Gateway Timeout";
        default:  return "ERROR";
    }
}

Http::Http()
    : content_length_(0U)
    , buffer_()
    , state_(ParseState::WaitingHeader)
    , request_()
{
}

//这个函数用于接受http请求存储到buffer
void Http::feed(const uint8_t* data, size_t length)
{
    if(data == nullptr || length == 0U)
    {
        return;
    }
    //append() 就是往字符串末尾追加数据。
    //强制转化讲data的uint8转为const char*
    buffer_.append(reinterpret_cast<const char*>(data), length);
}

//返回当前http的状态
bool Http::parsed() const
{
    return state_ == ParseState::Complete;
}

//返回结果
const HttpRequest& Http::request() const
{
    return request_;
}

//这个函数就是http运行的主函数了
bool Http::poll()
{
    //先判断是否完成
    if(state_ == ParseState::Complete)
    {
        return true;
    }

    /* 对端只发半截还一直不发完 —— 不能让它无限占内存。 */
    /*
        正常请求可能：
        GET /status HTTP/1.1
        Host: localhost
        才几十字节。
        但如果客户端一直发：
        AAAAAAAAAAAAAAAAAAAAAAAAAAAA....
        又永远不发：
        \r\n\r\n
        你的程序就会不断执行：
        buffer_.append(...)
        最终：
        10 KB
        100 KB
        1 MB
        100 MB
        ...
        这就可能把 EdgeHub 内存耗光。
    */
    if(buffer_.size() > HTTP_MAX_REQUEST_BYTES)
    {
        return false;
    }

    /* ---------- 步骤 1：等头部收齐 ---------- */
    if(state_ == ParseState::WaitingHeader)
    {
        //这里的terminator_at标志buffer中找到的请求结束位
        //如果找到了，termainator_at为请求结束位的起始下标
        //如果没有找到为std::string::npos
        size_t terminator_at = buffer_.find(HTTP_HEADER_TERMINATOR);

        //std::string::npos表示为一个很大的无符号整数，溢出时候会为-1
        //大多数情况表示查找失败，我们之前的terminator如果没有找到请求结束位就会为这个
        if(terminator_at == std::string::npos)
        {
            return false;   // 头还没收全，等下次 feed
        }

        std::string head = buffer_.substr(0U, terminator_at);

        /* 步骤 1a：第一行是请求行，三段的顺序是 method / path / version。 */
        size_t line_end = head.find("\r\n");
        //正常情况找到结束行标志所以request——line可以被正常提取，如果没有找到，说明只发送了一个请求那么head就是request
        std::string request_line = (line_end == std::string::npos) ?
            head : head.substr(0U, line_end);

        //first_space找的是第一个空格，比如request_line = POST /data HTTP/1.1\r\n
        size_t first_space = request_line.find(' ');
        if(first_space == std::string::npos)
        {
            return false;   // 请求行不成形
        }
        size_t second_space = request_line.find(' ', first_space + 1U);
        if(second_space == std::string::npos)
        {
            return false;
        }
        request_.method = request_line.substr(0U, first_space);
        request_.path = request_line.substr(first_space + 1U,
                                            second_space - first_space - 1U);

        /* 步骤 1b：其余行是请求头。只认 Content-Length，别的全部忽略。 */
        content_length_ = 0U;
        if(line_end != std::string::npos)
        {
            size_t cursor = line_end + 2U;   // 跳过 "\r\n"
            while(cursor < head.size())
            {
                //从 cursor 指向的位置开始，在 head 里找下一处 \r\n，也就是找“当前这一行的结尾”。
                size_t next = head.find("\r\n", cursor);
                //substr: 从 cursor 开始，截取到 next 之前的这一段字符串。
                //此时 line就代表着一段请求行
                std::string line = (next == std::string::npos) ?
                    head.substr(cursor) : head.substr(cursor, next - cursor);
                //判断next是不是空的，如果是代表着下一行已经没有了，所以直接让cursor等于size
                cursor = (next == std::string::npos) ? head.size() : next + 2U;

                if(line.empty())
                {
                    continue;
                }
                //这里冒号是因为我们要提取content_length用来判断body长度，
                //Header 格式一般都是：
                // 名字: 值
                //content_length : x
                size_t colon = line.find(':');
                if(colon == std::string::npos)
                {
                    continue;
                }
                //然后我们再提取这一行的name格式，原本的名字:值变为只有名字
                //随后判断这个名字是不是content-length如果是的话，提取名字后面的值，于是我们就得到的content_length
                //strtoul:这一句就是把冒号后面的值转成整数。
                //先line.substr(colon + 1U)得到”5“转为C字符串，然后std::strtoul(..., nullptr, 10)，按十进制解析得到数字5
                std::string name = trim(line.substr(0U, colon));
                if(header_name_equals(name, "Content-Length"))
                {
                    content_length_ = static_cast<size_t>(
                        std::strtoul(line.substr(colon + 1U).c_str(), nullptr, 10));
                }
            }
        }

        /* 头部已消费掉（连结束标记一起），剩下的就是身体（的一部分）。 */
        //从 buffer_ 的第 0 个字符开始，删除 terminator_at + 4 个字符。此时buffer里面存储的就是body
        buffer_.erase(0U, terminator_at + 4U);
        state_ = ParseState::WaitingBody;
    }

    /* ---------- 步骤 2：等身体收齐 ---------- */
    if(state_ == ParseState::WaitingBody)
    {
        if(buffer_.size() < content_length_)
        {
            return false;   // 身体还没收全
        }

        /* 只取 content_length_ 个字节：多出来的是下一个请求（粘包），不能一起吞。 */
        request_.body = buffer_.substr(0U, content_length_);
        buffer_.erase(0U, content_length_);
        content_length_ = 0U;
        state_ = ParseState::Complete;
    }
    //至此我们就已经成功将reqeust解析了
    return state_ == ParseState::Complete;
}

//这个函数用于返回你要返回给客户端的数据”组装成一个合法的 HTTP 响应。她与poll正好相反
//本质就是拼接字符串
std::string Http::serialize(int status, const std::string& body)
{
    std::string out;
    out += "HTTP/1.1 ";
    out += std::to_string(status);
    out += ' ';
    out += status_reason(status);
    out += "\r\n";
    out += "Content-Type: application/json\r\n";
    out += "Connection: close\r\n";
    if(status == 405)
    {
        out += "Allow: GET\r\n";
    }
    out += "Content-Length: ";
    out += std::to_string(body.size());
    out += "\r\n";
    out += "\r\n";
    out += body;
    return out;
}
