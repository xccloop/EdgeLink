#include "hmi_task.h"
#include "FreeRtos/Tasks/Transmit/transmit_task.h"
#include "FreeRtos/Tasks/task_common.h"
#include "FreeRtos/Queue/rtos_queue.h"
#include "Presentation/Hmi/Control/hmi_control.h"
#include "Output/Log/log.h"
#include "Output/Tcp/tcp.h"
#include "Config/config.h"
#include "KEY/key.h"
#include <stdio.h>
#include <string.h>

static StaticTask_t hmi_task_tcb;
static StackType_t hmi_task_stack[512];   /* 512：任务内调用 printf，栈需求变大 */
TaskHandle_t hmi_task_handle;

static void hmi_task(void *argument)
{
    TickType_t last_wake_time;//这里是记录tick值而不是ms
    (void)argument;
    QueueHandle_t sample_queue;//接受队列
    QueueHandle_t log_queue;//TransmitTask 发来的显示日志
    static hmi_view_data_t hmi_snapshot;
    telemetry_sample_struct sample;//采样
    char log_line[HMI_LOG_LINE_SIZE];

    uint8_t key_event;//按键事件
    uint8_t need_commit;//这一轮有没有新数据要提交
    uint8_t i;
    uint32_t pending_now;
    last_wake_time = xTaskGetTickCount();

    sample_queue =
        rtos_collect_to_hmi_queue_get();
    log_queue = rtos_transmit_to_hmi_log_queue_get();

    if((sample_queue == NULL) || (log_queue == NULL))
    {
        task_block_forever();
    }

    memset(&hmi_snapshot, 0, sizeof(hmi_snapshot));
    hmi_snapshot.node_id = board_id;
    hmi_control_init(board_id);
    while(1)
    {
        need_commit = 0U;

        key_event = key_event_take();
        if((key_event & 0x01U) != 0U)
        {
            hmi_key_event(HMI_KEY_PREVIOUS);
        }

        if((key_event & 0x02U) != 0U)
        {
            hmi_key_event(HMI_KEY_NEXT);
        }

        if((key_event & 0x04U) != 0U)
        {
            hmi_key_event(HMI_KEY_CONFIRM);
        }

        hmi_snapshot.can_state = can_state;

        uint8_t wifi_state = wifi_state_get();
        if(wifi_state == TCP_FAIL){hmi_snapshot.wifi_state = HMI_LINK_OFFLINE;}
        else if(wifi_state == TCP_SUCCESS){hmi_snapshot.wifi_state = HMI_LINK_ONLINE;}
        else {hmi_snapshot.wifi_state = HMI_LINK_UNKNOWN;}

        uint8_t tcp_state = tcp_connected_get();
        if(tcp_state == TCP_FAIL){hmi_snapshot.tcp_state = HMI_LINK_OFFLINE;}
        else if(tcp_state == TCP_SUCCESS){hmi_snapshot.tcp_state = HMI_LINK_ONLINE;}

        (void)tcp_ssid_get(hmi_snapshot.wifi_ssid, sizeof(hmi_snapshot.wifi_ssid));
        (void)tcp_local_ip_get(hmi_snapshot.local_ip, sizeof(hmi_snapshot.local_ip));

        /*
            Log 那边只在"写入成功"和"确认成功"两处改这个数。
            这里只在它真的变了的时候才置 need_commit —— 否则每 1ms 都提交一次，
            屏幕会被要求不停重画。
        */
        pending_now = log_pending_count_get();
        if(pending_now != hmi_snapshot.pending_count)
        {
            hmi_snapshot.pending_count = pending_now;
            need_commit = 1U;
        }

        if(xQueueReceive(sample_queue,&sample,0) == pdPASS)
        {
            hmi_snapshot.temperature_deci_c = (int16_t)(sample.temperature / 10); //除10因为scale为-2
            hmi_snapshot.temperature_valid  = 1U;
            need_commit = 1U;
        }

        /*
            TransmitTask 每发完一条就丢一行日志过来。
            窗口满了就先把上面几行整体往上顶一格，最老的那行被挤出去，
            再把新行放到最下面——和终端一样。
        */
        if(xQueueReceive(log_queue, log_line, 0U) == pdPASS)
        {
            if(hmi_snapshot.log_count >= HMI_LOG_LINES)
            {
                for(i = 0U; i < (HMI_LOG_LINES - 1U); i++)
                {
                    memcpy(hmi_snapshot.logs[i],
                           hmi_snapshot.logs[i + 1U],
                           HMI_LOG_LINE_SIZE);
                }
                hmi_snapshot.log_count = HMI_LOG_LINES - 1U;
            }

            memcpy(hmi_snapshot.logs[hmi_snapshot.log_count],
                   log_line,
                   HMI_LOG_LINE_SIZE);
            hmi_snapshot.log_count++;

            need_commit = 1U;
        }

        /* 温度和日志共用一次提交，别提交两遍。 */
        if(need_commit != 0U)
        {
            (void)hmi_set_view_data(&hmi_snapshot);
        }

        if(hmi_service() == 0U)
        {
            printf("hmi fail");
            while(1)
            {

            }
        }
        vTaskDelayUntil(&last_wake_time, pdMS_TO_TICKS(1U));
    }
}

void hmi_task_create(void)
{
    hmi_task_handle = xTaskCreateStatic(hmi_task,
                                                   "hmi",
                                                   512,
                                                   NULL,
                                                   3,
                                                   hmi_task_stack,
                                                   &hmi_task_tcb);
    configASSERT(hmi_task_handle != NULL);
}
