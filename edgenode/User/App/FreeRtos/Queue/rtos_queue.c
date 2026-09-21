#include "rtos_queue.h"
/* Storage 初始化恢复的 sequence 只需要交给 CollectTask 一次。 */
static StaticQueue_t storage_to_collect_sequence_queue_control;
static uint8_t storage_to_collect_sequence_queue_storage[
    RTOS_STORAGE_TO_COLLECT_SEQUENCE_QUEUE_LENGTH * sizeof(uint32_t)];
static QueueHandle_t storage_to_collect_sequence_queue;

/* CollectTask 每产生一条 Message 前必须先取得一个许可。 */
static StaticQueue_t storage_to_collect_permission_queue_control;
static uint8_t storage_to_collect_permission_queue_storage[
    RTOS_STORAGE_TO_COLLECT_PERMISSION_QUEUE_LENGTH * sizeof(uint8_t)];
static QueueHandle_t storage_to_collect_permission_queue;

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

/* TransmitTask 对每个 work 投递一次 TCP/CAN 最终结果，StorageTask 决定是否确认。 */
static StaticQueue_t transmit_to_storage_confirm_queue_control;
static uint8_t transmit_to_storage_confirm_queue_storage[
    RTOS_TRANSMIT_TO_STORAGE_CONFIRM_QUEUE_LENGTH * sizeof(storage_confirm_event_t)];
static QueueHandle_t transmit_to_storage_confirm_queue;

static StaticQueue_t tcp_ack_frame_queue_control;
static uint8_t tcp_ack_frame_queue_storage[
    RTOS_TCP_ACK_FRAME_QUEUE_LENGTH * sizeof(tcp_ack_frame_t)];
static QueueHandle_t tcp_ack_frame_queue;

static StaticQueue_t can_receive_frame_queue_control;
static uint8_t can_receive_frame_queue_storage[
    RTOS_CAN_RECEIVE_FRAME_QUEUE_LENGTH * sizeof(can_receive_frame_t)];
static QueueHandle_t can_receive_frame_queue;

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

    storage_to_collect_permission_queue = xQueueCreateStatic(
        RTOS_STORAGE_TO_COLLECT_PERMISSION_QUEUE_LENGTH,
        sizeof(uint8_t),
        storage_to_collect_permission_queue_storage,
        &storage_to_collect_permission_queue_control);

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

    tcp_ack_frame_queue = xQueueCreateStatic(
        RTOS_TCP_ACK_FRAME_QUEUE_LENGTH,
        sizeof(tcp_ack_frame_t),
        tcp_ack_frame_queue_storage,
        &tcp_ack_frame_queue_control);

    can_receive_frame_queue = xQueueCreateStatic(
        RTOS_CAN_RECEIVE_FRAME_QUEUE_LENGTH,
        sizeof(can_receive_frame_t),
        can_receive_frame_queue_storage,
        &can_receive_frame_queue_control);

    if((storage_to_collect_sequence_queue == NULL) ||
       (storage_to_collect_permission_queue == NULL) ||
       (storage_to_transmit_queue == NULL) ||
       (collect_to_storage_queue == NULL) ||
       (transmit_to_storage_confirm_queue == NULL) ||
       (tcp_ack_frame_queue == NULL) ||
       (can_receive_frame_queue == NULL))
    {
        storage_to_collect_sequence_queue = NULL;
        storage_to_collect_permission_queue = NULL;
        storage_to_transmit_queue = NULL;
        collect_to_storage_queue = NULL;
        transmit_to_storage_confirm_queue = NULL;
        tcp_ack_frame_queue = NULL;
        can_receive_frame_queue = NULL;
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

QueueHandle_t rtos_storage_to_collect_permission_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return storage_to_collect_permission_queue;
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

QueueHandle_t rtos_tcp_ack_frame_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? tcp_ack_frame_queue : NULL;
}

QueueHandle_t rtos_can_receive_frame_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? can_receive_frame_queue : NULL;
}

uint8_t rtos_tcp_ack_frame_send_from_isr(const tcp_ack_frame_t *frame,
                                         BaseType_t *higher_priority_task_woken)
{
    if((frame == NULL) || (higher_priority_task_woken == NULL) ||
       (rtos_queue_ready == 0U) || (tcp_ack_frame_queue == NULL))
    {
        return RTOS_QUEUE_FAIL;
    }

    return (xQueueSendFromISR(tcp_ack_frame_queue,
                              frame,
                              higher_priority_task_woken) == pdPASS) ?
           RTOS_QUEUE_SUCCESS : RTOS_QUEUE_FAIL;
}

uint8_t rtos_can_receive_frame_send_from_isr(const can_receive_frame_t *frame,
                                             BaseType_t *higher_priority_task_woken)
{
    if((frame == NULL) || (higher_priority_task_woken == NULL) ||
       (rtos_queue_ready == 0U) || (can_receive_frame_queue == NULL))
    {
        return RTOS_QUEUE_FAIL;
    }

    return (xQueueSendFromISR(can_receive_frame_queue,
                              frame,
                              higher_priority_task_woken) == pdPASS) ?
           RTOS_QUEUE_SUCCESS : RTOS_QUEUE_FAIL;
}
