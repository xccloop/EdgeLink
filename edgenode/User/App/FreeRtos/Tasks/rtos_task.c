//现在我们知道创建一个task要配置对应的TCB，栈，我们来创建两个任务，LED交替闪烁
#include "rtos_task.h"
#include "FreeRTOS.h"
#include "task.h"
#include "LED/led.h"

static StaticTask_t led1_task_tcb;
static StackType_t led1_task_stack[256];

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
    TaskHandle_t task_handle;

    task_handle = xTaskCreateStatic(led1_task,
                                    "led1",
                                    256,
                                    NULL,
                                    2,
                                    led1_task_stack,
                                    &led1_task_tcb);
    configASSERT(task_handle != NULL);
}

static StaticTask_t led2_task_tcb;
static StackType_t led2_task_stack[256];

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
    TaskHandle_t task_handle;

    task_handle = xTaskCreateStatic(led2_task,
                                    "led2",
                                    256,
                                    NULL,
                                    2,
                                    led2_task_stack,
                                    &led2_task_tcb);
    configASSERT(task_handle != NULL);
}

void led_tasks_create(void)
{
    led1_task_create();
    led2_task_create();
}
