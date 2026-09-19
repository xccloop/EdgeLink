#include "rtos_queue.h"
/* Storage 初始化恢复的 sequence 只需要交给 CollectTask 一次。 */
static StaticQueue_t storage_to_collect_sequence_queue_control;
static uint8_t storage_to_collect_sequence_queue_storage[
    RTOS_STORAGE_TO_COLLECT_SEQUENCE_QUEUE_LENGTH * sizeof(uint32_t)];
static QueueHandle_t storage_to_collect_sequence_queue;

/* Storage 写入 pending 后，或恢复扫描找到 pending 后，都通过这条队列交给 TransmitTask。 */
static StaticQueue_t storage_to_transmit_queue_control;
static uint8_t storage_to_transmit_queue_storage[
    RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH * sizeof(transmit_work_item_t)];
static QueueHandle_t storage_to_transmit_queue;

/* CollectTask 只采样，不访问 Flash；新消息经此队列交给 StorageTask。 */
static StaticQueue_t collect_to_storage_queue_control;
static uint8_t collect_to_storage_queue_storage[
    RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH * sizeof(telemetry_sample_struct)];
static QueueHandle_t collect_to_storage_queue;

/* 只有收到 TCP/CAN ACK 成功后，TransmitTask 才投递此事件请求 StorageTask 二次写确认位。 */
static StaticQueue_t transmit_to_storage_confirm_queue_control;
static uint8_t transmit_to_storage_confirm_queue_storage[
    RTOS_TRANSMIT_TO_STORAGE_CONFIRM_QUEUE_LENGTH * sizeof(storage_confirm_event_t)];
static QueueHandle_t transmit_to_storage_confirm_queue;

static uint8_t rtos_queue_ready;

uint8_t rtos_queue_init(void)
{
    if(rtos_queue_ready != 0U)
    {
        return RTOS_QUEUE_SUCCESS;
    }

    storage_to_collect_sequence_queue = xQueueCreateStatic(
        RTOS_STORAGE_TO_COLLECT_SEQUENCE_QUEUE_LENGTH,
        sizeof(uint32_t),
        storage_to_collect_sequence_queue_storage,
        &storage_to_collect_sequence_queue_control);

    storage_to_transmit_queue = xQueueCreateStatic(
        RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH,
        sizeof(transmit_work_item_t),
        storage_to_transmit_queue_storage,
        &storage_to_transmit_queue_control);

    collect_to_storage_queue = xQueueCreateStatic(
        RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH,
        sizeof(telemetry_sample_struct),
        collect_to_storage_queue_storage,
        &collect_to_storage_queue_control);

    transmit_to_storage_confirm_queue = xQueueCreateStatic(
        RTOS_TRANSMIT_TO_STORAGE_CONFIRM_QUEUE_LENGTH,
        sizeof(storage_confirm_event_t),
        transmit_to_storage_confirm_queue_storage,
        &transmit_to_storage_confirm_queue_control);

    if((storage_to_collect_sequence_queue == NULL) ||
       (storage_to_transmit_queue == NULL) ||
       (collect_to_storage_queue == NULL) ||
       (transmit_to_storage_confirm_queue == NULL))
    {
        storage_to_collect_sequence_queue = NULL;
        storage_to_transmit_queue = NULL;
        collect_to_storage_queue = NULL;
        transmit_to_storage_confirm_queue = NULL;
        return RTOS_QUEUE_FAIL;
    }

    rtos_queue_ready = 1U;
    return RTOS_QUEUE_SUCCESS;
}

QueueHandle_t rtos_storage_to_collect_sequence_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return storage_to_collect_sequence_queue;
}

QueueHandle_t rtos_storage_to_transmit_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return storage_to_transmit_queue;
}

QueueHandle_t rtos_collect_to_storage_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return collect_to_storage_queue;
}

QueueHandle_t rtos_transmit_to_storage_confirm_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return transmit_to_storage_confirm_queue;
}
