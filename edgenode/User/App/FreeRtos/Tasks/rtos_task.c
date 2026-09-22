//现在我们知道创建一个task要配置对应的TCB，栈，我们来创建两个任务，LED交替闪烁
#include "rtos_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "LED/led.h"
#include "Model/message.h"
#include "Output/Storage/storage.h"
#include "Service/Transmit/transmit.h"
#include "Config/config.h"
#include "Protocol/Tcp/tcp_frame.h"
#include "Protocol/Can/can_frame.h"
#include <stdint.h>
#include <stdio.h>
#include "queue.h"
#include "FreeRtos/Queue/rtos_queue.h"

/* 初始化失败时本任务不再访问外设或队列，但继续阻塞让其他任务可以运行。 */
static void task_block_forever(void)
{
    while(1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/*
    TransmitTask 在 TCP 重连成功后置 1；StorageTask 看到后把积压的 pending 补发一遍。
    跨任务单字节标志：即使读写竞争，最坏也只是重复补发一次，而确认 / Hub 去重是幂等的。
*/
static volatile uint8_t link_restored;

/* 上次尝试 TCP 重连的 tick，用来给重连加冷却，避免链路长时间不可用时反复卡住发送任务。 */
static TickType_t last_reconnect_tick;

/* 两条记录之间至少间隔这么久才再试一次重连。 */
#define TCP_RECONNECT_COOLDOWN_MS 5000U

static StaticTask_t led1_task_tcb;
static StackType_t led1_task_stack[256];
static TaskHandle_t led1_task_handle;

static void led1_task(void *argument)
{
    TickType_t last_wake_time;

    (void)argument;
    last_wake_time = xTaskGetTickCount();

    while (1)
    {
        led1_toggle();

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(200U));
    }
}

static void led1_task_create(void)
{
    led1_task_handle = xTaskCreateStatic(led1_task,
                                         "led1",
                                         256,
                                         NULL,
                                         2,
                                         led1_task_stack,
                                         &led1_task_tcb);
    configASSERT(led1_task_handle != NULL);
}

static StaticTask_t led2_task_tcb;
static StackType_t led2_task_stack[256];
static TaskHandle_t led2_task_handle;

static void led2_task(void *argument)
{
    TickType_t last_wake_time;//这里是记录tick值而不是ms

    (void)argument;

    vTaskDelay(pdMS_TO_TICKS(100U));
    
    last_wake_time = xTaskGetTickCount();

    while (1)
    {
        led2_toggle();

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(200U));
    }
}

static void led2_task_create(void)
{
    led2_task_handle = xTaskCreateStatic(led2_task,
                                         "led2",
                                         256,
                                         NULL,
                                         2,
                                         led2_task_stack,
                                         &led2_task_tcb);
    configASSERT(led2_task_handle != NULL);
}

void led_tasks_create(void)
{
    led1_task_create();
    led2_task_create();
}

/*
    把一条待补发的记录交给 TransmitTask，再根据回来的确认事件决定下一步：
      - 确认成功 → storage_confirm 写 confirmed，然后找下一条 pending；
      - 连续 3 次失败 → 放弃这一条直接返回（留给下次补发或下次上电），
        不让一条补不动的数据把补发流程永久卡死；
      - 全部 pending 确认完 → 返回。
    起点既可能来自 storage_init() 的扫描结果（上电恢复），
    也可能来自 storage_find_oldest_pending()（运行期链路恢复）。
*/
static void storage_replay_pending(transmit_work_item_t transmit_work,
                                   QueueHandle_t transmit_queue,
                                   QueueHandle_t confirm_queue)
{
    uint8_t fail_count = 0U;

    (void)xQueueSend(transmit_queue, &transmit_work, portMAX_DELAY);

    while(1)
    {
        storage_confirm_event_t confirm_event;
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
                if(storage_confirm(confirm_event.flash_address,
                                   confirm_event.sequence) != STORAGE_SUCCESS)
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

        if(storage_confirm(confirm_event.flash_address,
                           confirm_event.sequence) != STORAGE_SUCCESS)
        {
            task_block_forever();
        }

        fail_count = 0U;

        find_result = storage_find_next_pending(
            confirm_event.flash_address,
            &transmit_work.message,
            &transmit_work.flash_address);

        if(find_result == STORAGE_SUCCESS)
        {
            (void)xQueueSend(transmit_queue, &transmit_work, portMAX_DELAY);
            continue;
        }

        if(find_result == STORAGE_NO_PENDING)
        {
            return;
        }

        task_block_forever();
    }
}

static StaticTask_t storage_task_tcb;
static StackType_t storage_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
static TaskHandle_t storage_task_handle;
static TaskHandle_t collect_task_handle;

/*
    这个任务是上电后第一个运行的第一个任务
*/
static void storage_task(void *argument)
{
    storage_init_result_t storage_result;
    telemetry_sample_struct collected_message;
    transmit_work_item_t transmit_work;
    storage_confirm_event_t confirm_event;

    QueueHandle_t sequence_queue;
    QueueHandle_t permission_queue;
    QueueHandle_t collect_queue;
    QueueHandle_t transmit_queue;
    QueueHandle_t confirm_queue;

    (void)argument;

    sequence_queue = rtos_storage_to_collect_sequence_queue_get();
    permission_queue = rtos_storage_to_collect_permission_queue_get();
    collect_queue = rtos_collect_to_storage_queue_get();
    transmit_queue = rtos_storage_to_transmit_queue_get();
    confirm_queue = rtos_transmit_to_storage_confirm_queue_get();

    if((sequence_queue == NULL) ||
       (permission_queue == NULL) ||
       (collect_queue == NULL) ||
       (transmit_queue == NULL) ||
       (confirm_queue == NULL))
    {
        task_block_forever();
    }

    /* 1. StorageTask 首先初始化 Flash。 */
    if(storage_init(&storage_result) != STORAGE_SUCCESS)
    {
        task_block_forever();
    }

    /*
        2. 上电恢复：把扫描到的所有 pending 按顺序补发完，再放行采集。
           起点是 storage_init() 找到的最旧 pending。
    */
    if(storage_result.has_pending != 0U)
    {
        transmit_work.message = storage_result.oldest_pending_message;
        transmit_work.flash_address =
            storage_result.oldest_pending_address;

        storage_replay_pending(transmit_work,
                               transmit_queue,
                               confirm_queue);
    }

    if(storage_write_pending_ready() != STORAGE_SUCCESS)
    {
        task_block_forever();
    }

    (void)xQueueSend(sequence_queue,
                     &storage_result.next_sequence,
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
            StorageTask 是唯一写 Flash 的任务，所以二次写入必须在这里完成。
            此处先取尽已经到达的事件，避免采集频繁时 ACK 长时间滞留。
        */
        while(xQueueReceive(confirm_queue, &confirm_event, 0U) == pdPASS)
        {
            if(confirm_event.success != 0U)
            {
                if(storage_confirm(confirm_event.flash_address,
                                   confirm_event.sequence) != STORAGE_SUCCESS)
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

            if(storage_find_oldest_pending(&transmit_work.message,
                                           &transmit_work.flash_address)
               == STORAGE_SUCCESS)
            {
                storage_replay_pending(transmit_work,
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
            if(storage_write_pending(&collected_message,
                                     &transmit_work.flash_address)
               == STORAGE_SUCCESS)
            {
                /* 调试：显示本次写入 Flash 的内容与地址。 */
                printf("[storage] seq=%lu temp=%d scale=%d addr=0x%06lX\r\n",
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

                if(storage_write_pending_ready() != STORAGE_SUCCESS)
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
                    上电后由 storage_init() 重新扫描实际 Flash 状态。
                */
                task_block_forever();
            }
        }
    }
}

void storage_task_create(void)
{
    storage_task_handle = xTaskCreateStatic(storage_task,
                                            "storage",
                                            512,
                                            NULL,
                                            4,//storage任务我希望他可以上电后就启动，因此设置为最高优先级
                                            storage_task_stack,
                                            &storage_task_tcb);
    configASSERT(storage_task_handle != NULL);
}

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

    (void)argument;

    sequence_queue =
        rtos_storage_to_collect_sequence_queue_get();

    permission_queue =
        rtos_storage_to_collect_permission_queue_get();

    collect_queue =
        rtos_collect_to_storage_queue_get();

    if((sequence_queue == NULL) ||
       (permission_queue == NULL) ||
       (collect_queue == NULL))
    {
        task_block_forever();
    }

    /*
        StorageTask 未初始化成功、未完成恢复前，
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
        Storage 只负责恢复它的起点。
    */
    if(message_sequence_init(next_sequence) != MESSAGE_SUCCESS)
    {
        task_block_forever();
    }

    while(1)
    {
        /*
            只有 StorageTask 确认下一槽可写时才会发许可。
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
static void transmit_storage_result_send(
    QueueHandle_t confirm_queue,
    const transmit_work_item_t *transmit_work,
    uint8_t success)
{
    storage_confirm_event_t confirm_event;

    confirm_event.sequence = transmit_work->message.sequence;
    confirm_event.flash_address = transmit_work->flash_address;
    confirm_event.success = success;

    (void)xQueueSend(confirm_queue,
                     &confirm_event,
                     portMAX_DELAY);
}

static void transmit_task(void *argument)
{
    transmit_work_item_t transmit_work;
    uint8_t transmit_success;
    TickType_t now_tick;
    QueueHandle_t transmit_queue;
    QueueHandle_t confirm_queue;
    QueueHandle_t tcp_ack_queue;
    QueueHandle_t can_receive_queue;

    (void)argument;

    transmit_queue =
        rtos_storage_to_transmit_queue_get();
    confirm_queue = rtos_transmit_to_storage_confirm_queue_get();
    tcp_ack_queue = rtos_tcp_ack_frame_queue_get();
    can_receive_queue = rtos_can_receive_frame_queue_get();

    if((transmit_queue == NULL) || (confirm_queue == NULL) ||
       (tcp_ack_queue == NULL) || (can_receive_queue == NULL))
    {
        task_block_forever();
    }

    /* TCP 建连可能等待 WiFi 回应，放在 TransmitTask 内不能阻塞 Storage 恢复。 */
    (void)tcp_init(config_tcp_get());

    while(1)
    {
        if(xQueueReceive(transmit_queue,
                         &transmit_work,
                         portMAX_DELAY) == pdPASS)
        {
            /*
                transmit_work.message：
                    TCP/CAN 编码所需业务数据。

                transmit_work.flash_address：
                    当前不参与发送；
                    将来收到 ACK 后，用于通知 StorageTask
                    确认正确的 Flash 槽位。
            */

            transmit_success = 0U;

            transmit_tcp_ack_queue_clear(tcp_ack_queue);
            if(tcp_frame_transmit(&transmit_work.message) == TCP_TRANSMIT_SUCCESS)
            {
                if(transmit_tcp_ack_wait(tcp_ack_queue,
                                         transmit_work.message.sequence) != 0U)
                {
                    transmit_success = 1U;
                }
            }

            if(transmit_success == 0U)
            {
                /*
                    TCP 这条路失败：多半是链路已经断了。
                    低频尝试重连；一旦重连成功，置 link_restored 通知
                    StorageTask 把积压的 pending 补发一遍。
                    tick 冷却避免网关一直不在时，每条记录都卡在 CIPSTART 上。
                */
                now_tick = xTaskGetTickCount();

                if((now_tick - last_reconnect_tick) >=
                   pdMS_TO_TICKS(TCP_RECONNECT_COOLDOWN_MS))
                {
                    last_reconnect_tick = now_tick;

                    if(tcp_try_reconnect() == TCP_SUCCESS)
                    {
                        link_restored = 1U;
                    }
                }

                transmit_can_receive_queue_clear(can_receive_queue);
                if(can_frame_transmit(board_id,
                                      &transmit_work.message) == CAN_TRANSMIT_SUCCESS)
                {
                    if(transmit_can_ack_wait(can_receive_queue,
                                             transmit_work.message.sequence) != 0U)
                    {
                        transmit_success = 1U;
                    }
                }
            }

            /* 调试：显示本次发送的内容与结果（OK/FAIL）。 */
            printf("[transmit] seq=%lu temp=%d scale=%d result=%s\r\n",
                   (unsigned long)transmit_work.message.sequence,
                   (int)transmit_work.message.temperature,
                   (int)transmit_work.message.temperature_scale,
                   (transmit_success != 0U) ? "OK" : "FAIL");

            transmit_storage_result_send(confirm_queue,
                                         &transmit_work,
                                         transmit_success);
        }
    }
}

static StaticTask_t transmit_task_tcb;
static StackType_t transmit_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
static TaskHandle_t transmit_task_handle;

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

static StaticTask_t stack_monitor_task_tcb;
/* printf 的调用链较深，监控任务单独使用 512 个字的栈。 */
static StackType_t stack_monitor_task_stack[512];
static TaskHandle_t stack_monitor_task_handle;

/*
    uxTaskGetStackHighWaterMark() 返回任务运行以来“最少还剩多少栈”，单位是字，
    不是字节，也不是这一个瞬间的空闲栈。数值越小，说明历史上越接近栈溢出。
*/
static void stack_monitor_task(void *argument)
{
    TickType_t last_wake_time;

    (void)argument;
    last_wake_time = xTaskGetTickCount();

    while(1)
    {
        printf("stack free words: storage=%lu collect=%lu transmit=%lu led1=%lu led2=%lu monitor=%lu\r\n",
               (unsigned long)uxTaskGetStackHighWaterMark(storage_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(collect_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(transmit_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(led1_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(led2_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(NULL));

        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(30000U));
    }
}

void stack_monitor_task_create(void)
{
    stack_monitor_task_handle = xTaskCreateStatic(stack_monitor_task,
                                                   "stack",
                                                   512,
                                                   NULL,
                                                   1,
                                                   stack_monitor_task_stack,
                                                   &stack_monitor_task_tcb);
    configASSERT(stack_monitor_task_handle != NULL);
}
