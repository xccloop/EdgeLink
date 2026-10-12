#include "Http.hpp"
#include <cctype>

bool Http::assemble(int type)
{
    this->request.clear();
    this->response.clear();

    switch (type)
    {
    case STAUS:
        // HTTP/1.0 不要求 Host 头，组装层无需知道服务器地址。
        this->request = "GET /hub/status HTTP/1.0\r\n";
        break;

    default:
        return false;
    }

    this->request += "Connection: close\r\n";
    this->request += "\r\n";

    return true;
}

std::string Http::getRequest() const
{
    return this->request;
}

bool Http::parseResponse(const std::string &message)
{
    this->response.clear();

    // 与 Hub 一样限制单条报文大小，避免异常长度造成过大的内存占用。
    const size_t maxMessageSize = 1024 * 1024;
    if (message.size() > maxMessageSize)
    {
        return false;
    }

    size_t headerEnd = message.find("\r\n\r\n");
    size_t lineEnd = message.find("\r\n");
    if (headerEnd == std::string::npos || lineEnd < 13 || lineEnd > headerEnd)
    {
        return false;
    }

    std::string version = message.substr(0, 9);
    if (version != "HTTP/1.0 " && version != "HTTP/1.1 ")
    {
        return false;
    }
    if (message[9] < '1' || message[9] > '5' ||
        message[10] < '0' || message[10] > '9' ||
        message[11] < '0' || message[11] > '9' || message[12] != ' ')
    {
        return false;
    }

    // Hub 的 GET 响应使用 Content-Length，直接找到这一项即可。
    std::string headers = message.substr(0, headerEnd);
    for (size_t i = 0; i < headers.size(); i++)
    {
        headers[i] = std::tolower((unsigned char)headers[i]);
    }

    std::string lengthField = "\r\ncontent-length:";
    size_t lengthPosition = headers.find(lengthField);
    if (lengthPosition == std::string::npos ||
        headers.find(lengthField, lengthPosition + 1) != std::string::npos ||
        headers.find("\r\ntransfer-encoding:") != std::string::npos)
    {
        return false;
    }

    size_t lengthBegin = lengthPosition + lengthField.size();
    size_t lengthEnd = message.find("\r\n", lengthBegin);
    std::string lengthText = message.substr(lengthBegin, lengthEnd - lengthBegin);
    size_t firstDigit = lengthText.find_first_not_of(" \t");
    size_t lastDigit = lengthText.find_last_not_of(" \t");
    if (firstDigit == std::string::npos)
    {
        return false;
    }

    size_t contentLength = 0;
    for (size_t i = firstDigit; i <= lastDigit; i++)
    {
        if (lengthText[i] < '0' || lengthText[i] > '9')
        {
            return false;
        }
        contentLength = contentLength * 10 + (lengthText[i] - '0');
        if (contentLength > maxMessageSize)
        {
            return false;
        }
    }

    size_t bodyBegin = headerEnd + 4;
    if (message.size() - bodyBegin != contentLength)
    {
        return false;
    }

    this->response = message;
    return true;
}

std::string Http::getResponse() const
{
    return this->response;
}

int Http::getStatusCode() const
{
    if (this->response.empty())
    {
        return 0;
    }
    return (this->response[9] - '0') * 100 +
           (this->response[10] - '0') * 10 + (this->response[11] - '0');
}

std::string Http::getResponseBody() const
{
    if (this->response.empty())
    {
        return "";
    }
    size_t bodyBegin = this->response.find("\r\n\r\n") + 4;
    return this->response.substr(bodyBegin);
}
