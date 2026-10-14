#include "log_task.h"
#include "FreeRtos/Tasks/Hmi/hmi_task.h"
#include "FreeRtos/Tasks/Transmit/transmit_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Output/Log/log.h"
#include <stdint.h>
#include <stdio.h>

typedef enum
{
    LOG_REPLAY_IDLE,
    LOG_REPLAY_SCAN,
    LOG_REPLAY_FIND,
    LOG_REPLAY_SUBMIT,
    LOG_REPLAY_WAIT
} log_replay_state_t;

typedef struct
{
    log_replay_state_t state;
    transmit_work_item_t work;
    uint8_t fail_count;
} log_replay_context_t;

static void log_replay_step(log_replay_context_t *replay,
                             QueueHandle_t transmit_queue,
                             uint8_t normal_pending,
                             uint8_t normal_in_flight)
{
    uint8_t result;

    /*
        每轮只推进一个状态，不在这里循环扫描，也不等待确认队列。
        历史最多一条在途，正常发送优先；已有正常结果全部处理后才投递历史，
        避免同一条旧记录的正常结果被误认为当前补发结果。
    */
    switch(replay->state)
    {
        case LOG_REPLAY_SCAN:
            result = log_pending_scan_step();
            if(result == LOG_FAIL)
            {
                task_block_forever();
            }
            if(result == LOG_SUCCESS)
            {
                replay->state = LOG_REPLAY_FIND;
            }
            break;

        case LOG_REPLAY_FIND:
            result = log_pending_replay_next(&replay->work.message,
                                             &replay->work.flash_address);
            if(result == LOG_FAIL)
            {
                task_block_forever();
            }
            if(result == LOG_SUCCESS)
            {
                replay->fail_count = 0U;
                replay->state = LOG_REPLAY_SUBMIT;
            }
            else if(result == LOG_NO_PENDING)
            {
                replay->state = LOG_REPLAY_IDLE;
            }
            break;

        case LOG_REPLAY_SUBMIT:
            if((normal_pending == 0U) && (normal_in_flight == 0U) &&
               (xQueueSend(transmit_queue, &replay->work, 0U) == pdPASS))
            {
                replay->state = LOG_REPLAY_WAIT;
            }
            break;

        default:
            /* IDLE 没有恢复工作；WAIT 由主循环收到对应结果后推进。 */
            break;
    }
}

static StaticTask_t log_task_tcb;
static StackType_t log_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
TaskHandle_t log_task_handle;

static void log_task(void *argument)
{
    log_init_result_t log_result;
    telemetry_sample_struct collected_message;
    transmit_work_item_t normal_work;
    log_replay_context_t replay = {0};
    log_confirm_event_t confirm_event;
    uint8_t normal_pending = 0U;
    uint8_t normal_in_flight = 0U;
    uint8_t permission_outstanding = 0U;
    uint8_t permission_check_needed = 1U;
    uint8_t replay_requested = 0U;
    uint8_t retry_on_delivery = 0U;
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
    if((sequence_queue == NULL) || (permission_queue == NULL) ||
       (collect_queue == NULL) || (transmit_queue == NULL) || (confirm_queue == NULL))
    {
        task_block_forever();
    }

    /* HMI 首帧完成后恢复序号。上电全区扫描保持原逻辑，历史发送不再挡住采集。 */
    (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    if(log_init(&log_result) != LOG_SUCCESS)
    {
        task_block_forever();
    }
    if(log_result.has_pending != 0U)
    {
        replay.state = LOG_REPLAY_FIND;
    }
    (void)xQueueSend(sequence_queue, &log_result.next_sequence, portMAX_DELAY);
    xTaskNotifyGive(hmi_task_handle);

    while(1)
    {
        /*
            成功结果先确认 Flash；仅地址和 sequence 都匹配，才推进历史工作。
            normal_in_flight 只统计普通工作，每条工作由 TransmitTask 回一个结果。
        */
        while(xQueueReceive(confirm_queue, &confirm_event, 0U) == pdPASS)
        {
            uint8_t is_replay =
                (replay.state == LOG_REPLAY_WAIT) &&
                (confirm_event.sequence == replay.work.message.sequence) &&
                (confirm_event.flash_address == replay.work.flash_address);

            if(confirm_event.success != 0U)
            {
                uint8_t result = log_confirm(confirm_event.flash_address,
                                             confirm_event.sequence);
                if(result == LOG_FAIL)
                {
                    task_block_forever();
                }
                /* LOG_STALE 是槽位已复用的旧结果，不得确认新记录。 */
                if(result == LOG_SUCCESS)
                {
                    permission_check_needed = 1U;
                }
            }

            if(is_replay != 0U)
            {
                if(confirm_event.success != 0U)
                {
                    replay.state = LOG_REPLAY_FIND;
                    replay.fail_count = 0U;
                }
                else if(++replay.fail_count < 3U)
                {
                    replay.state = LOG_REPLAY_SUBMIT;
                }
                else
                {
                    /* 不自动无限重试同一条，正常采集继续，Flash pending 保留。 */
                    replay.state = LOG_REPLAY_IDLE;
                }
            }
            else
            {
                if(normal_in_flight != 0U)
                {
                    normal_in_flight--;
                }
                if(confirm_event.success == 0U)
                {
                    retry_on_delivery = 1U;
                }
                else if(retry_on_delivery != 0U)
                {
                    /* 新记录失败后留在 Flash，后续通信成功再安排恢复，离线时不空转建图。 */
                    replay_requested = 1U;
                    retry_on_delivery = 0U;
                }
            }
        }

        if(link_restored != 0U)
        {
            link_restored = 0U;
            replay_requested = 1U;
        }
        if((replay_requested != 0U) && (replay.state == LOG_REPLAY_IDLE))
        {
            /* 捕获已落盘序号上限，之后的新采样不延长这一次历史恢复。 */
            if(log_pending_scan_begin() != LOG_SUCCESS)
            {
                task_block_forever();
            }
            replay.state = LOG_REPLAY_SCAN;
            replay.fail_count = 0U;
            replay_requested = 0U;
        }

        /* 待投递普通记录只有一个，未成功入队时保留，不用 portMAX_DELAY 堵住恢复。 */
        if((normal_pending == 0U) &&
           (xQueueReceive(collect_queue, &collected_message, 0U) == pdPASS))
        {
            permission_outstanding = 0U;
            if(log_write_pending(&collected_message, &normal_work.flash_address)
               != LOG_SUCCESS)
            {
                task_block_forever();
            }
            normal_work.message = collected_message;
            normal_pending = 1U;
            permission_check_needed = 1U;
            printf("[log] seq=%lu temp=%d scale=%d addr=0x%06lX\r\n",
                   (unsigned long)collected_message.sequence,
                   (int)collected_message.temperature,
                   (int)collected_message.temperature_scale,
                   (unsigned long)normal_work.flash_address);
        }
        if((normal_pending != 0U) &&
           (xQueueSend(transmit_queue, &normal_work, 0U) == pdPASS))
        {
            normal_pending = 0U;
            normal_in_flight++;
        }

        /*
            恢复状态不阻止发许可。队列满的待投递记录先保留，形成有界背压；
            目标扇区含 pending 时等确认后再检查，不每毫秒反复读取整个扇区。
        */
        if((permission_outstanding == 0U) && (normal_pending == 0U) &&
           (permission_check_needed != 0U))
        {
            uint8_t result = log_write_pending_ready();
            if(result == LOG_FAIL)
            {
                task_block_forever();
            }
            if(result == LOG_SUCCESS)
            {
                uint8_t permission = 1U;
                if(xQueueSend(permission_queue, &permission, 0U) != pdPASS)
                {
                    task_block_forever();
                }
                permission_outstanding = 1U;
            }
            permission_check_needed = 0U;
        }

        log_replay_step(&replay, transmit_queue, normal_pending, normal_in_flight);
        /* 恢复时每轮让出 CPU，空闲时短睡眠，既不忙等也不让扫描每页等 100 ms。 */
        vTaskDelay((replay.state != LOG_REPLAY_IDLE || normal_pending != 0U)
                       ? 1U : pdMS_TO_TICKS(10U));
    }
}

void log_task_create(void)
{
    log_task_handle = xTaskCreateStatic(log_task,
                                      "log",
                                      512,
                                      NULL,
                                      4,
                                      log_task_stack,
                                      &log_task_tcb);
    configASSERT(log_task_handle != NULL);
}
