#ifndef HMI_TASK_H_
#define HMI_TASK_H_

#include "FreeRTOS.h"
#include "task.h"

extern TaskHandle_t hmi_task_handle;
void hmi_task_create(void);

#endif
