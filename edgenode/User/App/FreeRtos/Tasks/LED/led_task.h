#ifndef LED_TASK_H_
#define LED_TASK_H_

#include "FreeRTOS.h"
#include "task.h"

extern TaskHandle_t led1_task_handle;
extern TaskHandle_t led2_task_handle;
void led_tasks_create(void);

#endif
