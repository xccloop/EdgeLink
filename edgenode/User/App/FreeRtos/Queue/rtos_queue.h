#ifndef RTOS_QUEUE_H_
#define RTOS_QUEUE_H_

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "Model/message.h"

/* 每条采集消息先进入 StorageTask。 */
#define RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH  10U

/* Storage 成功写入 pending 后，把消息和所在槽位一起交给 TransmitTask。 */
#define RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH 10U

/* TCP/CAN 收到 ACK 后，把确认请求交还给 StorageTask。 */
#define RTOS_CONFIRM_TO_STORAGE_QUEUE_LENGTH  10U

#define RTOS_QUEUE_SUCCESS 1U
#define RTOS_QUEUE_FAIL    0U

typedef struct
{
    telemetry_sample_struct message;
    uint32_t flash_address;
} transmit_work_item_t;

typedef struct
{
    uint32_t sequence;
    uint32_t flash_address;
} storage_confirm_event_t;

/* 在调度器启动前调用一次；全部使用静态内存，不使用 FreeRTOS 堆。 */
uint8_t rtos_queue_init(void);

/* 任务在创建后取得各自需要的队列句柄。 */
QueueHandle_t rtos_collect_to_storage_queue_get(void);
QueueHandle_t rtos_storage_to_transmit_queue_get(void);
QueueHandle_t rtos_confirm_to_storage_queue_get(void);

#endif
