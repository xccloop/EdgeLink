#ifndef COLLECT_TASK_H_
#define COLLECT_TASK_H_

#include "FreeRTOS.h"
#include "task.h"

extern TaskHandle_t collect_task_handle;
void collect_task_create(void);

#endif
