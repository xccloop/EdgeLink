#include "message.h"
#include "Acquisition/collection.h"

/*
    这个文件是将采集到的温度进行模型管理作为中转层他最后会给TCPframe，CANframe，Storage
*/

static uint32_t message_next_sequence;
static uint8_t message_sequence_ready;

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

    if(message_sequence_ready == 0U)
    {
        return MESSAGE_FAIL;
    }

    /* 预留 0xFFFFFFFF 作为“没有可再分配的 sequence”，禁止静默回绕为 0。 */
    if(message_next_sequence == 0xFFFFFFFFUL)
    {
        return MESSAGE_FAIL;
    }

    message->sequence = message_next_sequence;
    message_next_sequence++;

    return MESSAGE_SUCCESS;
}


/*
    这个函数用于将storage_init计算出来的squence变为这次的squence起点
*/
uint8_t message_sequence_init(uint32_t next_sequence)
{
    message_next_sequence = next_sequence;
    message_sequence_ready = 1U;
    return MESSAGE_SUCCESS;
}
