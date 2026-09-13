#include "message.h"
#include "Acquisition/collection.h"

/*
    这个文件是将采集到的温度进行模型管理作为中转层他最后会给TCPframe，CANframe，Storage
*/

uint8_t message_collect(telemetry_sample_struct *message)
{
    if(message == 0)
    {
        return MESSAGE_FAIL;
    }

    /* Collection负责读取BMP280并记录读取完成时刻；Message只把结果放进统一模型。 */
    if(collection_temperature_get(&message->temperature,
                                  &message->temperature_scale,
                                  &message->sample_uptime_ms) == COLLECTION_FAIL)
    {
        return MESSAGE_FAIL;
    }

    return MESSAGE_SUCCESS;
}

