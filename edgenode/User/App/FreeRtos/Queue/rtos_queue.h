#ifndef RTOS_QUEUE_H_
#define RTOS_QUEUE_H_

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "Model/message.h"

/* Storage 初始化完成后只发送一次 next_sequence，CollectTask 在此之前保持阻塞。 */
#define RTOS_STORAGE_TO_COLLECT_SEQUENCE_QUEUE_LENGTH  1U

/* CollectTask 把新采样的业务 Message 交给唯一允许写 Flash 的 StorageTask。 */
#define RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH            10U

/* Storage 找到的 pending 已完成 Flash 解码，再交给 TransmitTask 编码为 TCP/CAN。 */
#define RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH           10U

/* TransmitTask 收到 Hub 成功 ACK 后，将确认事件交回 StorageTask 二次写状态。 */
#define RTOS_TRANSMIT_TO_STORAGE_CONFIRM_QUEUE_LENGTH   10U

#define RTOS_QUEUE_SUCCESS 1U
#define RTOS_QUEUE_FAIL    0U

typedef struct
{
    /* TCP/CAN 只使用 message；地址保留给 ACK 成功后回传 Storage 精确确认槽位。 */
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
QueueHandle_t rtos_storage_to_collect_sequence_queue_get(void);
QueueHandle_t rtos_collect_to_storage_queue_get(void);
QueueHandle_t rtos_storage_to_transmit_queue_get(void);
QueueHandle_t rtos_transmit_to_storage_confirm_queue_get(void);

#endif
