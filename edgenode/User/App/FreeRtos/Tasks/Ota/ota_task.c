#include "ota_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Service/Ota/ota.h"
#include "Protocol/Can/can_frame.h"
#include "INTERRUPT/USART/usart.h"
#include <stdint.h>

/*
    OtaTask：固件接收的驱动层。

    它不做任何协议判断 —— 判断全在 ota.c 里。这里只负责"搬"：
    把 CAN OTA 帧或 TCP 查询取出来递给 ota.c，并传入实际空闲时间。

    为什么不塞进现有任务：transmit_task 里发 TCP 要等 1 秒 ACK，log_task 会被
    gd25_clear 的 45ms 卡住。绑进去的话，8 深的 ota_frame_queue 会瞬间被灌爆
    （1 秒能来几千帧）。所以要独立任务，而且优先级要能单独调。

    优先级 3 和 collect/transmit 同级，是设计文档里的起点值，真机再调：
    太低排空 CAN 队列不及时，太高会饿死采集。
*/
static StaticTask_t ota_task_tcb;
static StackType_t ota_task_stack[512];
static TaskHandle_t ota_task_handle;

/* 服务层只返回结果，本任务负责按原链路排队。 */
static void ota_result_send(QueueHandle_t ota_reply_queue, const ota_result_t *result,
                            uint8_t source, uint32_t generation)
{
    if((result->reply != 0U) || (result->finished != 0U))
    {
        ota_transmit_request_t request = {0};
        request.source = source;
        request.tcp_generation = generation;

        request.reply.kind = (result->finished != 0U) ? CAN_OTA_REPLY_KIND_RESULT
                                                    : CAN_OTA_REPLY_KIND_PROGRESS;
        if(result->reply_kind != 0U)
        {
            request.reply.kind = result->reply_kind;
        }
        request.reply.contiguous = result->contiguous;
        request.reply.success = result->success;
        request.reply.request_id = result->request_id;
        request.reply.query_status = result->query_status;
        request.reply.target_slot = result->target_slot;

        if((xQueueSend(ota_reply_queue, &request, 0U) != pdPASS) &&
           (source == RTOS_OTA_SOURCE_TCP))
        {
            rtos_tcp_ota_query_complete(generation);
        }
    }
    else if(source == RTOS_OTA_SOURCE_TCP)
    {
        rtos_tcp_ota_query_complete(generation);
    }
}

static void ota_task(void *argument)
{
    QueueHandle_t ota_queue;
    QueueHandle_t ota_reply_queue;
    ota_receive_frame_t input = {0};
    ota_result_t result;
    TickType_t boot_tick;
    TickType_t idle_tick;
    TickType_t now;
    uint8_t confirmed;

    (void)argument;

    ota_queue = rtos_ota_frame_queue_get();
    ota_reply_queue = rtos_ota_reply_queue_get();
    if((ota_queue == NULL) || (ota_reply_queue == NULL))
    {
        task_block_forever();
    }

    boot_tick = xTaskGetTickCount();
    idle_tick = boot_tick;
    confirmed = 0U;

    while(1)
    {
        /*
            跑满 OTA_CONFIRM_DELAY_MS 之后，向 bootloader 销账一次。

            为什么由这里做：这是 OTA 模块自己的事 —— "我这份固件被确认了"。
            为什么等这么久才做：太早销账等于把安全网拆了（见 ota_confirm_boot 的注释）。

            代价：真的写那一次会冻 CPU 约 30ms（内部 Flash 擦页）。
            但它只在"刚升级完的第一次启动"上真写（其余时候读一下就返回），
            而那一刻没有 OTA 传输在进行，所以不会丢帧。
        */
        if((confirmed == 0U) &&
           ((xTaskGetTickCount() - boot_tick) >= pdMS_TO_TICKS(OTA_CONFIRM_DELAY_MS)))
        {
            (void)ota_confirm_boot();
            confirmed = 1U;
        }

        /* 同一输入队列接收两条链路；TCP 查询不重置 CAN 接收空闲计时。 */
        if(xQueueReceive(ota_queue, &input, pdMS_TO_TICKS(OTA_IDLE_REPLY_MS)) == pdPASS)
        {
            if(input.source == RTOS_OTA_SOURCE_TCP)
            {
                if(input.tcp_generation == esp12s_tcp_generation_get())
                {
                    result = ota_tcp_on_frame(input.frame.data, input.frame.data_length);
                    ota_result_send(ota_reply_queue, &result, input.source, input.tcp_generation);
                }
                else
                {
                    rtos_tcp_ota_query_complete(input.tcp_generation);
                }
            }
            else
            {
                result = ota_can_on_frame(input.frame.standard_id,
                                          input.frame.data, input.frame.data_length);
                idle_tick = xTaskGetTickCount();
                ota_result_send(ota_reply_queue, &result, RTOS_OTA_SOURCE_CAN, 0U);
            }
        }
        now = xTaskGetTickCount();
        if((now - idle_tick) >= pdMS_TO_TICKS(OTA_IDLE_REPLY_MS))
        {
            result = ota_on_idle((uint32_t)(now - idle_tick) * portTICK_PERIOD_MS);
            idle_tick = now;
            ota_result_send(ota_reply_queue, &result, RTOS_OTA_SOURCE_CAN, 0U);
        }

        /* 接收成功后继续运行当前固件，HMI 提示用户手动重启再安装。 */
    }
}

void ota_task_create(void)
{
    ota_task_handle = xTaskCreateStatic(ota_task,
                                        "ota",
                                        512,
                                        NULL,
                                        3,
                                        ota_task_stack,
                                        &ota_task_tcb);
    configASSERT(ota_task_handle != NULL);
}
