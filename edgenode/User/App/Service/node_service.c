#include "node_service.h"
#include "Config/config.h"
#include "Model/message.h"
#include "Output/Storage/storage.h"
#include "Output/Tcp/tcp.h"
#include "board_time.h"
#include "Service/Transmit/transmit.h"
#include <stdio.h>

static uint32_t node_service_last_sample_ms;
static uint8_t node_service_has_sampled;
static uint16_t sequence;


/*
    这个函数是一次采集周期的APP调度入口。
    先由Model从Acquisition取得一条完整遥测数据，再交给每个已启用的输出分支。
    目前唯一已接入的分支是Storage；TCP和CAN会在各自发送链路完成后从这里接入，
    所以main()不需要了解传感器、Flash或协议的具体细节。
*/
uint8_t node_service_run_once()
{
    telemetry_sample_struct message;
    uint32_t now_ms;

    now_ms = board_systick_ms;
    if((node_service_has_sampled != 0U) &&
       ((uint32_t)(now_ms - node_service_last_sample_ms) < NODE_SAMPLE_PERIOD_MS))
    {
        return 1U;
    }

    /* 先记下本轮时刻，写Flash失败时也不会在紧循环中无限重试。 */
    node_service_last_sample_ms = now_ms;
    node_service_has_sampled = 1U;

    if(message_collect(&message) == MESSAGE_FAIL)
    {
        printf("collection fail");
        return 0U;
    }

    /*
    if(storage_temperature(&message) == STORAGE_FAIL)
    {
        printf("storage fail");
        return 0U;
    }
    */

    if(tcp_frame_transmit(&message,sequence) == TCP_SEND_FAIL)
    {
        printf("tcp send fail");
        return 0u;
    }
    sequence++;
    return 1U;
}
