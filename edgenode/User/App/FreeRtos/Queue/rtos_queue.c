#include "rtos_queue.h"
#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>
/* Log 初始化恢复的 sequence 只需要交给 CollectTask 一次。 */
static StaticQueue_t log_to_collect_sequence_queue_control;
static uint8_t log_to_collect_sequence_queue_storage[
    RTOS_LOG_TO_COLLECT_SEQUENCE_QUEUE_LENGTH * sizeof(uint32_t)];
static QueueHandle_t log_to_collect_sequence_queue;

/* CollectTask 每产生一条 Message 前必须先取得一个许可。 */
static StaticQueue_t log_to_collect_permission_queue_control;
static uint8_t log_to_collect_permission_queue_storage[
    RTOS_LOG_TO_COLLECT_PERMISSION_QUEUE_LENGTH * sizeof(uint8_t)];
static QueueHandle_t log_to_collect_permission_queue;

/* Log 写入 pending 后，或恢复扫描找到 pending 后，都通过这条队列交给 TransmitTask。 */
static StaticQueue_t log_to_transmit_queue_control;
static uint8_t log_to_transmit_queue_storage[
    RTOS_LOG_TO_TRANSMIT_QUEUE_LENGTH * sizeof(transmit_work_item_t)];
static QueueHandle_t log_to_transmit_queue;

/* CollectTask 只采样，不访问 Flash；新消息经此队列交给 LogTask。 */
static StaticQueue_t collect_to_log_queue_control;
static uint8_t collect_to_log_queue_storage[
    RTOS_COLLECT_TO_LOG_QUEUE_LENGTH * sizeof(telemetry_sample_struct)];
static QueueHandle_t collect_to_log_queue;

/* CollectTask将采样内容发给hmi */
static StaticQueue_t collect_to_hmi_queue_control;
static uint8_t collect_to_hmi_queue_storage[
    RTOS_COLLECT_TO_HMI_QUEUE_LENGTH * sizeof(telemetry_sample_struct)];
static QueueHandle_t collect_to_hmi_queue;

/* TransmitTask 对每个 work 投递一次 TCP/CAN 最终结果，LogTask 决定是否确认。 */
static StaticQueue_t transmit_to_log_confirm_queue_control;
static uint8_t transmit_to_log_confirm_queue_storage[
    RTOS_TRANSMIT_TO_LOG_CONFIRM_QUEUE_LENGTH * sizeof(log_confirm_event_t)];
static QueueHandle_t transmit_to_log_confirm_queue;

static StaticQueue_t tcp_ack_frame_queue_control;
static uint8_t tcp_ack_frame_queue_storage[
    RTOS_TCP_ACK_FRAME_QUEUE_LENGTH * sizeof(tcp_ack_frame_t)];
static QueueHandle_t tcp_ack_frame_queue;

static StaticQueue_t can_receive_frame_queue_control;
static uint8_t can_receive_frame_queue_storage[
    RTOS_CAN_RECEIVE_FRAME_QUEUE_LENGTH * sizeof(can_receive_frame_t)];
static QueueHandle_t can_receive_frame_queue;

/* TransmitTask 每发完一条就交一行显示用日志给 HMI，HMI 收下后塞进自己的滚动窗口。 */
static StaticQueue_t transmit_to_hmi_log_queue_control;
static uint8_t transmit_to_hmi_log_queue_storage[
    RTOS_TRANSMIT_TO_HMI_LOG_QUEUE_LENGTH * HMI_LOG_LINE_SIZE];
static QueueHandle_t transmit_to_hmi_log_queue;

static StaticQueue_t ota_frame_queue_control;
static uint8_t ota_frame_queue_storage[
    RTOS_OTA_FRAME_QUEUE_LENGTH * sizeof(ota_receive_frame_t)];
static QueueHandle_t ota_frame_queue;

/* OtaTask 把"该回复什么"交给 TransmitTask，由唯一发送者真正发帧。 */
static StaticQueue_t ota_reply_queue_control;
static uint8_t ota_reply_queue_storage[
    RTOS_OTA_REPLY_QUEUE_LENGTH * sizeof(ota_transmit_request_t)];
static QueueHandle_t ota_reply_queue;

static uint8_t rtos_queue_ready;
static volatile uint8_t tcp_query_pending;
static volatile uint32_t tcp_query_generation;

uint8_t rtos_queue_init(void)
{
    if(rtos_queue_ready != 0U)
    {
        return RTOS_QUEUE_SUCCESS;
    }

    log_to_collect_sequence_queue = xQueueCreateStatic(
        RTOS_LOG_TO_COLLECT_SEQUENCE_QUEUE_LENGTH,
        sizeof(uint32_t),
        log_to_collect_sequence_queue_storage,
        &log_to_collect_sequence_queue_control);

    log_to_collect_permission_queue = xQueueCreateStatic(
        RTOS_LOG_TO_COLLECT_PERMISSION_QUEUE_LENGTH,
        sizeof(uint8_t),
        log_to_collect_permission_queue_storage,
        &log_to_collect_permission_queue_control);

    log_to_transmit_queue = xQueueCreateStatic(
        RTOS_LOG_TO_TRANSMIT_QUEUE_LENGTH,
        sizeof(transmit_work_item_t),
        log_to_transmit_queue_storage,
        &log_to_transmit_queue_control);

    collect_to_log_queue = xQueueCreateStatic(
        RTOS_COLLECT_TO_LOG_QUEUE_LENGTH,
        sizeof(telemetry_sample_struct),
        collect_to_log_queue_storage,
        &collect_to_log_queue_control);
    
    collect_to_hmi_queue = xQueueCreateStatic(
        RTOS_COLLECT_TO_HMI_QUEUE_LENGTH,
        sizeof(telemetry_sample_struct),
        collect_to_hmi_queue_storage,
        &collect_to_hmi_queue_control);

    transmit_to_log_confirm_queue = xQueueCreateStatic(
        RTOS_TRANSMIT_TO_LOG_CONFIRM_QUEUE_LENGTH,
        sizeof(log_confirm_event_t),
        transmit_to_log_confirm_queue_storage,
        &transmit_to_log_confirm_queue_control);

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

    transmit_to_hmi_log_queue = xQueueCreateStatic(
        RTOS_TRANSMIT_TO_HMI_LOG_QUEUE_LENGTH,
        HMI_LOG_LINE_SIZE,
        transmit_to_hmi_log_queue_storage,
        &transmit_to_hmi_log_queue_control);
    
    ota_frame_queue = xQueueCreateStatic(
        RTOS_OTA_FRAME_QUEUE_LENGTH,
        sizeof(ota_receive_frame_t),
        ota_frame_queue_storage,
        &ota_frame_queue_control);

    ota_reply_queue = xQueueCreateStatic(
        RTOS_OTA_REPLY_QUEUE_LENGTH,
        sizeof(ota_transmit_request_t),
        ota_reply_queue_storage,
        &ota_reply_queue_control);

    if((log_to_collect_sequence_queue == NULL) ||
       (log_to_collect_permission_queue == NULL) ||
       (log_to_transmit_queue == NULL) ||
       (collect_to_log_queue == NULL) ||
       (transmit_to_log_confirm_queue == NULL) ||
       (tcp_ack_frame_queue == NULL) ||
       (can_receive_frame_queue == NULL) ||
       (collect_to_hmi_queue == NULL) ||
       (transmit_to_hmi_log_queue == NULL) ||
       (ota_frame_queue == NULL) ||
       (ota_reply_queue == NULL))
    {
        log_to_collect_sequence_queue = NULL;
        log_to_collect_permission_queue = NULL;
        log_to_transmit_queue = NULL;
        collect_to_log_queue = NULL;
        collect_to_hmi_queue = NULL;
        transmit_to_log_confirm_queue = NULL;
        tcp_ack_frame_queue = NULL;
        can_receive_frame_queue = NULL;
        transmit_to_hmi_log_queue = NULL;
        ota_frame_queue = NULL;
        ota_reply_queue = NULL;
        return RTOS_QUEUE_FAIL;
    }

    rtos_queue_ready = 1U;
    return RTOS_QUEUE_SUCCESS;
}

QueueHandle_t rtos_log_to_collect_sequence_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return log_to_collect_sequence_queue;
}

QueueHandle_t rtos_log_to_collect_permission_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return log_to_collect_permission_queue;
}

QueueHandle_t rtos_log_to_transmit_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return log_to_transmit_queue;
}

QueueHandle_t rtos_collect_to_log_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return collect_to_log_queue;
}

QueueHandle_t rtos_collect_to_hmi_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return collect_to_hmi_queue;
}

QueueHandle_t rtos_transmit_to_log_confirm_queue_get(void)
{
    if(rtos_queue_ready == 0U)
    {
        return NULL;
    }

    return transmit_to_log_confirm_queue;
}

QueueHandle_t rtos_tcp_ack_frame_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? tcp_ack_frame_queue : NULL;
}

QueueHandle_t rtos_can_receive_frame_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? can_receive_frame_queue : NULL;
}

QueueHandle_t rtos_transmit_to_hmi_log_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? transmit_to_hmi_log_queue : NULL;
}

QueueHandle_t rtos_ota_frame_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? ota_frame_queue : NULL;
}

QueueHandle_t rtos_ota_reply_queue_get(void)
{
    return (rtos_queue_ready != 0U) ? ota_reply_queue : NULL;
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

uint8_t rtos_ota_frame_send_from_isr(const can_receive_frame_t *frame,
                                     BaseType_t *higher_priority_task_woken)
{
    ota_receive_frame_t input = {0};
    if((frame == NULL) || (higher_priority_task_woken == NULL) ||
       (rtos_queue_ready == 0U) || (ota_frame_queue == NULL))
    {
        return RTOS_QUEUE_FAIL;
    }

    input.source = RTOS_OTA_SOURCE_CAN;
    input.frame = *frame;
    return (xQueueSendFromISR(ota_frame_queue,
                              &input,
                              higher_priority_task_woken) == pdPASS) ?
           RTOS_QUEUE_SUCCESS : RTOS_QUEUE_FAIL;
}
uint8_t rtos_tcp_ota_query_send_from_isr(const uint8_t data[TCP_OTA_QUERY_LENGTH],
                                        uint32_t generation,
                                        BaseType_t *higher_priority_task_woken)
{
    ota_receive_frame_t input = {0};
    if((data == NULL) || (higher_priority_task_woken == NULL) ||
       (rtos_queue_ready == 0U) || (ota_frame_queue == NULL) || (tcp_query_pending != 0U))
    {
        return RTOS_QUEUE_FAIL;
    }
    input.source = RTOS_OTA_SOURCE_TCP;
    input.tcp_generation = generation;
    input.frame.data_length = TCP_OTA_QUERY_LENGTH;
    input.frame.data[0] = data[0];
    input.frame.data[1] = data[1];
    if(xQueueSendFromISR(ota_frame_queue, &input, higher_priority_task_woken) != pdPASS)
    {
        return RTOS_QUEUE_FAIL;
    }
    tcp_query_generation = generation;
    tcp_query_pending = 1U;
    return RTOS_QUEUE_SUCCESS;
}

void rtos_tcp_ota_query_complete(uint32_t generation)
{
    taskENTER_CRITICAL();
    if(tcp_query_generation == generation)
    {
        tcp_query_pending = 0U;
    }
    taskEXIT_CRITICAL();
}
