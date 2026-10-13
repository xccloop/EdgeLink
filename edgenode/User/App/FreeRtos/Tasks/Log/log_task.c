#include "log_task.h"
#include "FreeRtos/Tasks/Hmi/hmi_task.h"
#include "FreeRtos/Tasks/Transmit/transmit_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Output/Log/log.h"
#include <stdint.h>
#include <stdio.h>

/* 扫描期间也处理正常发送留下的结果，避免确认队列塞满挡住 TransmitTask。 */
static void log_confirm_queue_drain(QueueHandle_t confirm_queue)
{
    log_confirm_event_t event;

    while(xQueueReceive(confirm_queue, &event, 0U) == pdPASS)
    {
        if((event.success != 0U) &&
           (log_confirm(event.flash_address, event.sequence) != LOG_SUCCESS))
        {
            task_block_forever();
        }
    }
}

static void log_pending_index_build(QueueHandle_t confirm_queue)
{
    uint8_t result;

    /* Log 层只读一页；任务层负责页间休眠，完整扫描成功之前不投递补发。 */
    if(log_pending_scan_begin() != LOG_SUCCESS)
    {
        task_block_forever();
    }
    do
    {
        log_confirm_queue_drain(confirm_queue);
        result = log_pending_scan_step();
        if(result == LOG_FAIL)
        {
            task_block_forever();
        }
        vTaskDelay(1U);
    } while(result == LOG_MORE);
}

static uint8_t log_replay_next(transmit_work_item_t *work,
                                QueueHandle_t confirm_queue)
{
    uint8_t result;

    /* 页内继续使用独立缓存，换页时休眠；没有 pending 的页只查 RAM 位图。 */
    do
    {
        log_confirm_queue_drain(confirm_queue);
        result = log_pending_replay_next(&work->message, &work->flash_address);
        if(result == LOG_MORE)
        {
            vTaskDelay(1U);
        }
    } while(result == LOG_MORE);
    if(result == LOG_FAIL)
    {
        task_block_forever();
    }
    return result;
}

/*
    把一条待补发的记录交给 TransmitTask，再根据回来的确认事件决定下一步：
      - 确认成功 → log_confirm 写 confirmed，然后找下一条 pending；
      - 连续 3 次失败 → 放弃这一条直接返回（留给下次补发或下次上电），
        不让一条补不动的数据把补发流程永久卡死；
      - 全部 pending 确认完 → 返回。
    上电使用初始化扫描建立的位图；重连必须先完成分步建图再调用。
*/
static void log_replay_pending(QueueHandle_t transmit_queue,
                                   QueueHandle_t confirm_queue)
{
    transmit_work_item_t transmit_work;
    uint8_t fail_count = 0U;

    if(log_replay_next(&transmit_work, confirm_queue) == LOG_NO_PENDING)
    {
        return;
    }
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

        find_result = log_replay_next(&transmit_work, confirm_queue);

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

static void log_collect_pause(QueueHandle_t permission_queue,
                               QueueHandle_t collect_queue,
                               QueueHandle_t confirm_queue,
                               uint8_t *permission_outstanding)
{
    uint8_t permission;
    telemetry_sample_struct sample;
    uint32_t address;

    /*
        尚未取走的许可直接撤回；已取走则等本次采样落盘，或失败归还许可。
        不再发新许可，确保建图期间没有新记录落到已经扫描过的页里。
    */
    while(*permission_outstanding != 0U)
    {
        if(xQueueReceive(permission_queue, &permission, 0U) == pdPASS)
        {
            *permission_outstanding = 0U;
            break;
        }
        if(xQueueReceive(collect_queue, &sample, 0U) == pdPASS)
        {
            if(log_write_pending(&sample, &address) != LOG_SUCCESS)
            {
                task_block_forever();
            }
            *permission_outstanding = 0U;
            break;
        }
        log_confirm_queue_drain(confirm_queue);
        vTaskDelay(1U);
    }
}

static StaticTask_t log_task_tcb;
static StackType_t log_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
TaskHandle_t log_task_handle;

/*
    上电先等待 HMI 启动页，再执行存储恢复并放行正常页面。
*/
static void log_task(void *argument)
{
    log_init_result_t log_result;
    telemetry_sample_struct collected_message;
    transmit_work_item_t transmit_work;
    log_confirm_event_t confirm_event;
    uint8_t permission_outstanding = 0U;

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

    /* HMI 完成首帧后再扫描 Flash，任务创建顺序不承担启动同步。 */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    /* 1. LogTask 首先初始化 Flash。 */
    if(log_init(&log_result) != LOG_SUCCESS)
    {
        task_block_forever();
    }

    /*
        2. 上电恢复：初始化扫描同时建立页位图，按物理页顺序补发完再放行采集。
           不再为寻找下一条 pending 执行无休眠的逐槽全区搜索。
    */
    if(log_result.has_pending != 0U)
    {
        log_replay_pending(transmit_queue, confirm_queue);
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
        permission_outstanding = 1U;
    }

    /* 存储前置流程完成，允许 HMI 进入 HOME、LINKS 等正常页面。 */
    xTaskNotifyGive(hmi_task_handle);

    while(1)
    {
        /*
            TransmitTask 投递每次发送的成功或失败事件。
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
            先暂停采集，按页完整建立位图，然后集中补发。
            扫描和补发页间均会休眠，期间的新重连通知留到下一轮处理，不嵌套调用。
        */
        if(link_restored != 0U)
        {
            link_restored = 0U;

            log_collect_pause(permission_queue, collect_queue, confirm_queue,
                              &permission_outstanding);
            log_pending_index_build(confirm_queue);
            log_replay_pending(transmit_queue, confirm_queue);

            /* 补发失败也可恢复采集，但仍不能擦除含 pending 的目标扇区。 */
            if(log_write_pending_ready() == LOG_SUCCESS)
            {
                uint8_t collect_permission = 1U;
                (void)xQueueSend(permission_queue, &collect_permission, portMAX_DELAY);
                permission_outstanding = 1U;
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
            permission_outstanding = 0U;
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
                    permission_outstanding = 1U;
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
