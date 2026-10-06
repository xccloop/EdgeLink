#include "ota_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Service/Ota/ota.h"
#include "Protocol/Can/can_frame.h"
#include "gd32f10x.h"
#include <stdint.h>

/*
    OtaTask：固件接收的驱动层。

    它不做任何协议判断 —— 判断全在 ota.c 里。这里只负责"搬"：
    把 CAN 中断分流过来的固件帧取出来递给 ota.c，没帧的时候问它一声。

    为什么不塞进现有任务：transmit_task 里发 TCP 要等 1 秒 ACK，log_task 会被
    gd25_clear 的 45ms 卡住。绑进去的话，8 深的 ota_frame_queue 会瞬间被灌爆
    （1 秒能来几千帧）。所以要独立任务，而且优先级要能单独调。

    优先级 3 和 collect/transmit 同级，是设计文档里的起点值，真机再调：
    太低排空 CAN 队列不及时，太高会饿死采集。
*/
static StaticTask_t ota_task_tcb;
static StackType_t ota_task_stack[512];
static TaskHandle_t ota_task_handle;

static void ota_task(void *argument)
{
    QueueHandle_t ota_queue;
    QueueHandle_t ota_reply_queue;
    can_receive_frame_t frame;
    ota_result_t result;
    TickType_t boot_tick;
    uint8_t confirmed;

    (void)argument;

    ota_queue = rtos_ota_frame_queue_get();
    ota_reply_queue = rtos_ota_reply_queue_get();
    if((ota_queue == NULL) || (ota_reply_queue == NULL))
    {
        task_block_forever();
    }

    boot_tick = xTaskGetTickCount();
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

        /*
            超时值用 OTA_IDLE_REPLY_MS，它有两个用途：
            ① 没帧可收时让 ota.c 有机会累计"闲着多久" —— Hub 半路挂了，只有这条路能发现
            ② 回复的节流也靠它兜底

            configTICK_RATE_HZ 是 1000，所以 3 就是 3 毫秒、不是 0，不会退化成忙等空转。
        */
        if(xQueueReceive(ota_queue,
                         &frame,
                         pdMS_TO_TICKS(OTA_IDLE_REPLY_MS)) == pdPASS)
        {
            result = ota_on_frame(frame.standard_id,
                                  frame.data,
                                  frame.data_length);
        }
        else
        {
            result = ota_on_idle();
        }

        /*
            ota.c 只"决定"该回什么，不发帧 —— 项目里 CAN 的唯一发送者是 TransmitTask。
            这里只负责把请求转过去，到了那边才真正写邮箱。

            塞不进就算了（超时给 0）：丢一次回复只让 Hub 多等一个超时周期，靠自愈，
            不值得为它卡住收帧。
        */
        if((result.reply != 0U) || (result.finished != 0U))
        {
            ota_reply_request_t request;

            request.kind = (result.finished != 0U) ? CAN_OTA_REPLY_KIND_RESULT
                                                   : CAN_OTA_REPLY_KIND_PROGRESS;
            request.contiguous = result.contiguous;
            request.success = result.success;

            (void)xQueueSend(ota_reply_queue, &request, 0U);
        }

        /*
            整个 OTA 收完了：结果回复已经进了队列，但 TransmitTask 还没取走。

            先等它出队，再留一点余量让 CAN 帧真的离开总线，然后复位整机 ——
            复位之后就没机会再说"我成功了"。**复位才是这次升级的终点**：
            复位 → bootloader 上电 → 发现中转区有完整镜像 → 搬进槽 → 试启动。
            少了这一步，新固件会一直躺在中转区，直到下一次碰巧掉电。

            等不到也照样复位：不能因为 Hub 没收到最后一句就停在这儿。
        */
        if(result.finished != 0U)
        {
            uint32_t wait;

            for(wait = 0U; wait < OTA_RESET_FLUSH_WAIT_MS; wait++)
            {
                if(uxQueueMessagesWaiting(ota_reply_queue) == 0U)
                {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(1U));
            }
            vTaskDelay(pdMS_TO_TICKS(OTA_RESET_FLUSH_MS));

            NVIC_SystemReset();     /* 不返回 */
        }
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
