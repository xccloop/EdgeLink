#include "Message.hpp"
#include "Frame.hpp"

//这个文件负责把已经校验过的Frame转换为数据库和后续业务使用的内部Message。
int Message_handle(Message *message,const Frame *frame)
{
    if(message == nullptr || frame == nullptr)
    {
        return -1;
    }

    message->nodeId = frame->header.sourceNode;
    message->sequence = frame->header.sequence;
    message->temperature = frame->temperature;
    message->temperatureScale = frame->temperatureScale;
    message->pressure = frame->pressure;
    message->pressureScale = frame->pressureScale;
    message->receivedAtUs = frame->receivedAtUs;
    return 0;
}
