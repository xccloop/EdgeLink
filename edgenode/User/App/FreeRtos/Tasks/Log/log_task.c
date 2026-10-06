#include "log_task.h"
#include "FreeRtos/Tasks/Transmit/transmit_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Output/Log/log.h"
#include <stdint.h>
#include <stdio.h>

/*
    把一条待补发的记录交给 TransmitTask，再根据回来的确认事件决定下一步：
      - 确认成功 → log_confirm 写 confirmed，然后找下一条 pending；
      - 连续 3 次失败 → 放弃这一条直接返回（留给下次补发或下次上电），
        不让一条补不动的数据把补发流程永久卡死；
      - 全部 pending 确认完 → 返回。
    此函数只由 log_init() 扫描到的 pending 调用，用于上电恢复。
*/
static void log_replay_pending(transmit_work_item_t transmit_work,
                                   QueueHandle_t transmit_queue,
                                   QueueHandle_t confirm_queue)
{
    uint8_t fail_count = 0U;

    (void)xQueueSend(transmit_queue, &transmit_work, portMAX_DELAY);

    while(1)
    {
        log_confirm_event_t confirm_event;
        uint8_t find_result;

        if(xQueueReceive(confirm_queue, &confirm_event, portMAX_DELAY) != pdPASS)
        {
            task_block_forever();
        }

        /*
            运行期补发时，TransmitTask 可能刚处理完上一条正常发送，
            它的事件会先于我们要的那条到达。这种“别人的”事件就地消化：
            成功就照常写 confirmed，失败则忽略，然后继续等自己这条。
        */
        if((confirm_event.sequence != transmit_work.message.sequence) ||
           (confirm_event.flash_address != transmit_work.flash_address))
        {
            if(confirm_event.success != 0U)
            {
                if(log_confirm(confirm_event.flash_address,
                                   confirm_event.sequence) != LOG_SUCCESS)
                {
                    task_block_forever();
                }
            }
            continue;
        }

        if(confirm_event.success == 0U)
        {
            fail_count++;

            if(fail_count < 3U)
            {
                (void)xQueueSend(transmit_queue, &transmit_work, portMAX_DELAY);
                continue;
            }

            return;
        }

        if(log_confirm(confirm_event.flash_address,
                           confirm_event.sequence) != LOG_SUCCESS)
        {
            task_block_forever();
        }

        fail_count = 0U;

        find_result = log_find_next_pending(
            confirm_event.flash_address,
            &transmit_work.message,
            &transmit_work.flash_address);

        if(find_result == LOG_SUCCESS)
        {
            (void)xQueueSend(transmit_queue, &transmit_work, portMAX_DELAY);
            continue;
        }

        if(find_result == LOG_NO_PENDING)
        {
            return;
        }

        task_block_forever();
    }
}

static StaticTask_t log_task_tcb;
static StackType_t log_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
TaskHandle_t log_task_handle;

/*
    这个任务是上电后第一个运行的第一个任务
*/
static void log_task(void *argument)
{
    log_init_result_t log_result;
    telemetry_sample_struct collected_message;
    transmit_work_item_t transmit_work;
    log_confirm_event_t confirm_event;

    QueueHandle_t sequence_queue;
    QueueHandle_t permission_queue;
    QueueHandle_t collect_queue;
    QueueHandle_t transmit_queue;
    QueueHandle_t confirm_queue;

    (void)argument;

    sequence_queue = rtos_log_to_collect_sequence_queue_get();
    permission_queue = rtos_log_to_collect_permission_queue_get();
    collect_queue = rtos_collect_to_log_queue_get();
    transmit_queue = rtos_log_to_transmit_queue_get();
    confirm_queue = rtos_transmit_to_log_confirm_queue_get();

    if((sequence_queue == NULL) ||
       (permission_queue == NULL) ||
       (collect_queue == NULL) ||
       (transmit_queue == NULL) ||
       (confirm_queue == NULL))
    {
        task_block_forever();
    }

    /* 1. LogTask 首先初始化 Flash。 */
    if(log_init(&log_result) != LOG_SUCCESS)
    {
        task_block_forever();
    }

    /*
        2. 上电恢复：把扫描到的所有 pending 按顺序补发完，再放行采集。
           起点是 log_init() 找到的最旧 pending。
    */
    if(log_result.has_pending != 0U)
    {
        transmit_work.message = log_result.oldest_pending_message;
        transmit_work.flash_address =
            log_result.oldest_pending_address;

        log_replay_pending(transmit_work,
                               transmit_queue,
                               confirm_queue);
    }

    if(log_write_pending_ready() != LOG_SUCCESS)
    {
        task_block_forever();
    }

    (void)xQueueSend(sequence_queue,
                     &log_result.next_sequence,
                     portMAX_DELAY);

    {
        uint8_t collect_permission = 1U;

        (void)xQueueSend(permission_queue,
                         &collect_permission,
                         portMAX_DELAY);
    }

    while(1)
    {
        /*
            TransmitTask 只在收到 Hub 的成功 ACK 后才投递确认事件。
            LogTask 是唯一写 Flash 的任务，所以二次写入必须在这里完成。
            此处先取尽已经到达的事件，避免采集频繁时 ACK 长时间滞留。
        */
        while(xQueueReceive(confirm_queue, &confirm_event, 0U) == pdPASS)
        {
            if(confirm_event.success != 0U)
            {
                if(log_confirm(confirm_event.flash_address,
                                   confirm_event.sequence) != LOG_SUCCESS)
                {
                    task_block_forever();
                }

            }
        }

        /*
            运行期链路恢复：TransmitTask 在 TCP 重连成功后置 link_restored。
            这里把当前所有 pending 补发一遍（起点取地址最旧的一条），
            补完再回到正常采集。补发期间 CollectTask 停在许可等待上，不会被饿着。
        */
        if(link_restored != 0U)
        {
            link_restored = 0U;

            if(log_find_oldest_pending(&transmit_work.message,
                                           &transmit_work.flash_address)
               == LOG_SUCCESS)
            {
                log_replay_pending(transmit_work,
                                       transmit_queue,
                                       confirm_queue);
            }
        }

        /*
            4. 等待 CollectTask 的新采样。
            最多等待 100 ms；这样没有新采样时也会回来检查确认队列。
            FreeRTOS 当前没有启用 Queue Set，不能同时永久阻塞在两条队列上。
        */
        if(xQueueReceive(collect_queue,
                         &collected_message,
                         pdMS_TO_TICKS(100U)) == pdPASS)
        {
            if(log_write_pending(&collected_message,
                                     &transmit_work.flash_address)
               == LOG_SUCCESS)
            {
                /* 调试：显示本次写入 Flash 的内容与地址。 */
                printf("[log] seq=%lu temp=%d scale=%d addr=0x%06lX\r\n",
                       (unsigned long)collected_message.sequence,
                       (int)collected_message.temperature,
                       (int)collected_message.temperature_scale,
                       (unsigned long)transmit_work.flash_address);

                transmit_work.message = collected_message;

                /*
                    先成功写 Flash，再交给 TransmitTask。
                    至此断电，重启扫描仍能找到 pending。
                */
                xQueueSend(transmit_queue,
                           &transmit_work,
                           portMAX_DELAY);

                if(log_write_pending_ready() != LOG_SUCCESS)
                {
                    continue;
                }

                {
                    uint8_t collect_permission = 1U;

                    (void)xQueueSend(permission_queue,
                                     &collect_permission,
                                     portMAX_DELAY);
                }
            }
            else
            {
                /*
                    已取出的 Message 不能在未落盘时继续跳过；停止本任务，
                    防止 CollectTask 继续分配 sequence 后造成静默数据丢失。
                    上电后由 log_init() 重新扫描实际 Flash 状态。
                */
                task_block_forever();
            }
        }
    }
}

void log_task_create(void)
{
    log_task_handle = xTaskCreateStatic(log_task,
                                            "log",
                                            512,
                                            NULL,
                                            4,//storage任务我希望他可以上电后就启动，因此设置为最高优先级
                                            log_task_stack,
                                            &log_task_tcb);
    configASSERT(log_task_handle != NULL);
}
