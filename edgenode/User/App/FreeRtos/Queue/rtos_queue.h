#ifndef RTOS_QUEUE_H_
#define RTOS_QUEUE_H_

#include <stdint.h>
#include "FreeRTOS.h"
#include "queue.h"
#include "Model/message.h"
#include "Protocol/Tcp/tcp_frame.h"

/* Storage 初始化完成后只发送一次 next_sequence，CollectTask 在此之前保持阻塞。 */
#define RTOS_STORAGE_TO_COLLECT_SEQUENCE_QUEUE_LENGTH  1U

/* CollectTask 把新采样的业务 Message 交给唯一允许写 Flash 的 StorageTask。 */
#define RTOS_COLLECT_TO_STORAGE_QUEUE_LENGTH            10U

/* Storage 找到的 pending 已完成 Flash 解码，再交给 TransmitTask 编码为 TCP/CAN。 */
#define RTOS_STORAGE_TO_TRANSMIT_QUEUE_LENGTH           10U

/* TransmitTask 收到 Hub 成功 ACK 后，将确认事件交回 StorageTask 二次写状态。 */
#define RTOS_TRANSMIT_TO_STORAGE_CONFIRM_QUEUE_LENGTH   10U

/* USART1中断从 +IPD 取出的完整 TCP ACK 原始帧，由 TransmitTask 解码。 */
#define RTOS_TCP_ACK_FRAME_QUEUE_LENGTH                 4U

/* CAN接收中断交给 TransmitTask 的原始标准帧。 */
#define RTOS_CAN_RECEIVE_FRAME_QUEUE_LENGTH             4U

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

typedef struct
{
    uint8_t data[TCP_FRAME_LENGTH];
} tcp_ack_frame_t;

typedef struct
{
    uint16_t standard_id;
    uint8_t data_length;
    uint8_t data[8];
} can_receive_frame_t;

/* 在调度器启动前调用一次；全部使用静态内存，不使用 FreeRTOS 堆。 */
uint8_t rtos_queue_init(void);

/* 任务在创建后取得各自需要的队列句柄。 */
QueueHandle_t rtos_storage_to_collect_sequence_queue_get(void);
QueueHandle_t rtos_collect_to_storage_queue_get(void);
QueueHandle_t rtos_storage_to_transmit_queue_get(void);
QueueHandle_t rtos_transmit_to_storage_confirm_queue_get(void);
QueueHandle_t rtos_tcp_ack_frame_queue_get(void);
QueueHandle_t rtos_can_receive_frame_queue_get(void);

/* 仅供优先级满足 FreeRTOS 规则的 USART1/CAN 接收中断调用。 */
uint8_t rtos_tcp_ack_frame_send_from_isr(const tcp_ack_frame_t *frame,
                                         BaseType_t *higher_priority_task_woken);
uint8_t rtos_can_receive_frame_send_from_isr(const can_receive_frame_t *frame,
                                             BaseType_t *higher_priority_task_woken);

#endif
