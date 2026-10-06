#include "stack_monitor_task.h"
#include "FreeRtos/Tasks/Log/log_task.h"
#include "FreeRtos/Tasks/Collect/collect_task.h"
#include "FreeRtos/Tasks/Transmit/transmit_task.h"
#include "FreeRtos/Tasks/LED/led_task.h"
#include "FreeRtos/Tasks/Hmi/hmi_task.h"
#include <stdio.h>

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
        printf("stack free words: log=%lu collect=%lu transmit=%lu led1=%lu led2=%lu hmi=%lu monitor=%lu\r\n",
               (unsigned long)uxTaskGetStackHighWaterMark(log_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(collect_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(transmit_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(led1_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(led2_task_handle),
               (unsigned long)uxTaskGetStackHighWaterMark(hmi_task_handle),
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
