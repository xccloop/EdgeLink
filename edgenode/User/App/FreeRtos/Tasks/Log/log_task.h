#ifndef LOG_TASK_H_
#define LOG_TASK_H_

#include "FreeRTOS.h"
#include "task.h"

extern TaskHandle_t log_task_handle;
void log_task_create(void);

#endif
