#include "transmit_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Service/Transmit/transmit.h"
#include "Service/Ota/ota.h"
#include "Output/Can/can_output.h"
#include "Output/Tcp/tcp.h"
#include "Config/config.h"
#include "Protocol/Tcp/tcp_frame.h"
#include "Protocol/Tcp/tcp_receive.h"
#include "Protocol/Can/can_frame.h"
#include "INTERRUPT/USART/usart.h"
#include "OTA/flash_layout.h"
#include <stdint.h>
#include <stdio.h>

/*
    TransmitTask 在 TCP 重连成功或 CAN 从失败恢复后置 1；LogTask 看到后补发 pending。
    跨任务单字节标志：即使读写竞争，最坏也只是重复补发一次，
    而 log_confirm 和 Hub 的查重都是幂等的，重复不会出错。
*/
volatile uint8_t link_restored;

#define TRANSMIT_ACK_WAIT_MS 1000U

static void transmit_tcp_ack_queue_clear(QueueHandle_t tcp_ack_queue)
{
    tcp_ack_frame_t frame;

    while(xQueueReceive(tcp_ack_queue, &frame, 0U) == pdPASS)
    {
    }
}

static void transmit_can_receive_queue_clear(QueueHandle_t can_receive_queue)
{
    can_receive_frame_t frame;

    while(xQueueReceive(can_receive_queue, &frame, 0U) == pdPASS)
    {
    }
}

static uint8_t transmit_tcp_ack_wait(QueueHandle_t tcp_ack_queue,
                                     uint32_t expected_sequence)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_tick = pdMS_TO_TICKS(TRANSMIT_ACK_WAIT_MS);
    tcp_ack_frame_t frame;
    uint32_t ack_sequence;
    uint8_t ack_status;

    while((xTaskGetTickCount() - start_tick) < timeout_tick)
    {
        TickType_t elapsed_tick = xTaskGetTickCount() - start_tick;

        /* 两次读取 tick 之间可能正好达到超时，不能让减法下溢。 */
        if(elapsed_tick >= timeout_tick)
        {
            break;
        }

        if(xQueueReceive(tcp_ack_queue,
                         &frame,
                         timeout_tick - elapsed_tick) != pdPASS)
        {
            break;
        }

        if((tcp_ack_decode(frame.data, board_id, &ack_sequence, &ack_status) != 0U) &&
           (ack_sequence == expected_sequence) &&
           (ack_status == TCP_ACK_STATUS_SUCCESS))
        {
            return 1U;
        }
    }

    return 0U;
}

static uint8_t transmit_can_ack_wait(QueueHandle_t can_receive_queue,
                                     uint32_t expected_sequence)
{
    TickType_t start_tick = xTaskGetTickCount();
    TickType_t timeout_tick = pdMS_TO_TICKS(TRANSMIT_ACK_WAIT_MS);
    can_receive_frame_t frame;
    uint32_t ack_sequence;
    uint8_t ack_status;

    while((xTaskGetTickCount() - start_tick) < timeout_tick)
    {
        TickType_t elapsed_tick = xTaskGetTickCount() - start_tick;

        /* 两次读取 tick 之间可能正好达到超时，不能让减法下溢。 */
        if(elapsed_tick >= timeout_tick)
        {
            break;
        }

        if(xQueueReceive(can_receive_queue,
                         &frame,
                         timeout_tick - elapsed_tick) != pdPASS)
        {
            break;
        }

        if((can_ack_decode(frame.standard_id,
                           frame.data,
                           frame.data_length,
                           board_id,
                           &ack_sequence,
                           &ack_status) != CAN_FRAME_FAIL) &&
           (ack_sequence == expected_sequence) &&
           (ack_status == 0U))
        {
            return 1U;
        }
    }

    return 0U;
}
/*
    这个函数用于将
*/
static void transmit_log_result_send(
    QueueHandle_t confirm_queue,
    const transmit_work_item_t *transmit_work,
    uint8_t success)
{
    log_confirm_event_t confirm_event;

    confirm_event.sequence = transmit_work->message.sequence;
    confirm_event.flash_address = transmit_work->flash_address;
    confirm_event.success = success;

    (void)xQueueSend(confirm_queue,
                     &confirm_event,
                     portMAX_DELAY);
}

volatile hmi_link_state_t can_state;

static void transmit_task(void *argument)
{
    transmit_work_item_t transmit_work;
    uint8_t transmit_success;
    QueueHandle_t transmit_queue;
    QueueHandle_t confirm_queue;
    QueueHandle_t tcp_ack_queue;
    QueueHandle_t can_receive_queue;
    QueueHandle_t log_queue;
    QueueHandle_t ota_reply_queue;
    ota_transmit_request_t ota_reply;
    char log_line[HMI_LOG_LINE_SIZE];
    const char *link;

    (void)argument;

    transmit_queue =
        rtos_log_to_transmit_queue_get();
    confirm_queue = rtos_transmit_to_log_confirm_queue_get();
    tcp_ack_queue = rtos_tcp_ack_frame_queue_get();
    can_receive_queue = rtos_can_receive_frame_queue_get();
    log_queue = rtos_transmit_to_hmi_log_queue_get();
    ota_reply_queue = rtos_ota_reply_queue_get();

    if((transmit_queue == NULL) || (confirm_queue == NULL) ||
       (tcp_ack_queue == NULL) || (can_receive_queue == NULL) ||
       (log_queue == NULL) || (ota_reply_queue == NULL))
    {
        task_block_forever();
    }

    /* TCP 建连可能等待 WiFi 回应，放在 TransmitTask 内不能阻塞 Log 恢复。 */
    tcp_receive_init();
    (void)tcp_init(config_tcp_get());

    while(1)
    {
        if(tcp_recovery_service() == TCP_SUCCESS)
        {
            link_restored = 1U;
        }

        /*
            OTA 回复优先：Hub 发完一个窗口就停下来等它，它晚一步，整个传输就晚一步。
            非阻塞取，取到就发、立刻回环，不占用等待遥测的时间。
        */
        if(xQueueReceive(ota_reply_queue, &ota_reply, 0U) == pdPASS)
        {
            if(ota_reply.source == RTOS_OTA_SOURCE_CAN)
            {
                (void)can_ota_reply_send(board_id, &ota_reply.reply);
                continue;
            }
            if(ota_reply.source == RTOS_OTA_SOURCE_TCP)
            {
                if((ota_reply.tcp_generation == esp12s_tcp_generation_get()) &&
                   (tcp_connected_get() == TCP_SUCCESS))
                {
                    uint8_t data[TCP_OTA_QUERY_LENGTH] = {
                        TCP_OTA_QUERY_TYPE, TCP_OTA_SLOT_UNAVAILABLE};
                    if(ota_reply.reply.target_slot == OTA_SLOT_A_BASE_ADDRESS)
                    {
                        data[1] = TCP_OTA_SLOT_A;
                    }
                    else if(ota_reply.reply.target_slot == OTA_SLOT_B_BASE_ADDRESS)
                    {
                        data[1] = TCP_OTA_SLOT_B;
                    }
                    (void)tcp_data_send(data, sizeof(data));
                }
                rtos_tcp_ota_query_complete(ota_reply.tcp_generation);
                /* TCP 查询回复后仍给遥测一次发送机会。 */
            }
        }

        /*
            超时不能用 portMAX_DELAY：那样本任务会一直睡在遥测队列上，
            OTA 回复就永远没人取（遥测 1 秒才一条，回复延迟能到 1 秒）。
            用一个短超时定期醒来看一眼上面那条队列 —— 代价只是多几次空转。
        */
        if(xQueueReceive(transmit_queue,
                         &transmit_work,
                         pdMS_TO_TICKS(OTA_REPLY_POLL_MS)) == pdPASS)
        {
            /*
                transmit_work.message：
                    TCP/CAN 编码所需业务数据。

                transmit_work.flash_address：
                    当前不参与发送；
                    将来收到 ACK 后，用于通知 LogTask
                    确认正确的 Flash 槽位。
            */

            transmit_success = 0U;
            /*
                link 只在"帧真的发出去了"的时候才记名。
                两条链路都没发出去时保持 NULL，日志行会写成 FAIL，不冒充任何链路。
            */
            link = NULL;

            transmit_tcp_ack_queue_clear(tcp_ack_queue);
            if(tcp_connected_get() == TCP_SUCCESS)
            {
                if(tcp_frame_transmit(&transmit_work.message) == TCP_TRANSMIT_SUCCESS)
                {
                    link = "TCP";
                    transmit_success = transmit_tcp_ack_wait(tcp_ack_queue,
                                                             transmit_work.message.sequence);
                }
                tcp_recovery_note_ack(transmit_success);
            }

            if(transmit_success == 0U)
            {
                transmit_can_receive_queue_clear(can_receive_queue);
                if(can_frame_transmit(board_id,
                                      &transmit_work.message) == CAN_TRANSMIT_SUCCESS)
                {
                    link = "CAN";
                    if(transmit_can_ack_wait(can_receive_queue,
                                             transmit_work.message.sequence) != 0U)
                    {
                        if(can_state == HMI_LINK_OFFLINE)
                        {
                            link_restored = 1U;
                        }
                        can_state = HMI_LINK_ONLINE;
                        transmit_success = 1U;
                    }
                }
            }

            if(transmit_success == 0U)
            {
                can_state = HMI_LINK_OFFLINE;
            }

            /*
                拼一行给 HMI 滚动日志用：数据、倍率、谁发的、成没成。
                谁都没发出去时整行只写到 FAIL，不假装用了某条链路。
            */
            if(link == NULL)
            {
                (void)snprintf(log_line, sizeof(log_line), "%d %d FAIL",
                               (int)transmit_work.message.temperature,
                               (int)transmit_work.message.temperature_scale);
            }
            else
            {
                (void)snprintf(log_line, sizeof(log_line), "%d %d %s %s",
                               (int)transmit_work.message.temperature,
                               (int)transmit_work.message.temperature_scale,
                               link,
                               (transmit_success != 0U) ? "OK" : "FAIL");
            }

            /* 队列长度固定为 1，覆盖写：HMI 没来取就换掉旧的，不积压、不阻塞发送。 */
            (void)xQueueOverwrite(log_queue, log_line);

            /* 调试：显示本次发送的内容与结果（OK/FAIL）。 */
            printf("[transmit] seq=%lu temp=%d scale=%d result=%s\r\n",
                   (unsigned long)transmit_work.message.sequence,
                   (int)transmit_work.message.temperature,
                   (int)transmit_work.message.temperature_scale,
                   (transmit_success != 0U) ? "OK" : "FAIL");

            transmit_log_result_send(confirm_queue,
                                         &transmit_work,
                                         transmit_success);
        }
    }
}

static StaticTask_t transmit_task_tcb;
static StackType_t transmit_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
TaskHandle_t transmit_task_handle;

void transmit_task_create(void)
{
    transmit_task_handle = xTaskCreateStatic(transmit_task,
                                             "transmit",
                                             512,
                                             NULL,
                                             3,
                                             transmit_task_stack,
                                             &transmit_task_tcb);
    configASSERT(transmit_task_handle != NULL);
}
