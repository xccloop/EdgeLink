#include "collect_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Model/message.h"
#include <stdio.h>

TaskHandle_t collect_task_handle;

/*
    这个任务是采集任务，要先获取从stoage_init得到的sequence作为本次的起点，然后还需要将采集到的数据给storage，transmit存储
*/
static void collect_task(void *argument)
{
    uint32_t next_sequence;
    uint8_t collect_permission;
    telemetry_sample_struct message;
    QueueHandle_t sequence_queue;
    QueueHandle_t permission_queue;
    QueueHandle_t collect_queue;
    QueueHandle_t hmi_queue;

    (void)argument;

    sequence_queue =
        rtos_log_to_collect_sequence_queue_get();

    permission_queue =
        rtos_log_to_collect_permission_queue_get();

    collect_queue =
        rtos_collect_to_log_queue_get();
    
    hmi_queue = 
        rtos_collect_to_hmi_queue_get();

    if((sequence_queue == NULL) ||
       (permission_queue == NULL) ||
       (collect_queue == NULL) ||
       (hmi_queue == NULL))
    {
        task_block_forever();
    }

    /*
        LogTask 未初始化成功、未完成恢复前，
        CollectTask 永远停在这里。
    */
    if(xQueueReceive(sequence_queue,
                     &next_sequence,
                     portMAX_DELAY) != pdPASS)
    {
        task_block_forever();
    }

    /*
        Message 从这里开始接管 sequence 自增。
        Log 只负责恢复它的起点。
    */
    if(message_sequence_init(next_sequence) != MESSAGE_SUCCESS)
    {
        task_block_forever();
    }

    while(1)
    {
        /*
            只有 LogTask 确认下一槽可写时才会发许可。
            这样 CollectTask 生成的每个 sequence 都有一个预留的落盘机会。
        */
        if(xQueueReceive(permission_queue,
                         &collect_permission,
                         portMAX_DELAY) != pdPASS)
        {
            task_block_forever();
        }

        if(message_collect(&message) == MESSAGE_SUCCESS)
        {
            /* 调试：显示本次采集到的内容。 */
            printf("[collect] seq=%lu temp=%d scale=%d uptime=%lu\r\n",
                   (unsigned long)message.sequence,
                   (int)message.temperature,
                   (int)message.temperature_scale,
                   (unsigned long)message.sample_uptime_ms);

            /*
                队列中复制完整 Message。
                CollectTask 此后不关心 Flash 地址、CRC、pending 状态。
            */
            xQueueSend(collect_queue,
                       &message,
                       portMAX_DELAY);
            //* 显示是"有更好、没有也行"：队列满就丢，绝不能让 HMI 拖住采集。 */
            xQueueSend(hmi_queue, 
                       &message, 
                       0);
        }
        else
        {
            /* 本次没有采到有效数据，归还许可，下一周期再试。 */
            (void)xQueueSend(permission_queue,
                             &collect_permission,
                             portMAX_DELAY);
        }

        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static StaticTask_t collect_task_tcb;
static StackType_t collect_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */

void collect_task_create(void)
{
    collect_task_handle = xTaskCreateStatic(collect_task,
                                            "collect",
                                            512,
                                            NULL,
                                            3,
                                            collect_task_stack,
                                            &collect_task_tcb);
    configASSERT(collect_task_handle != NULL);
}
