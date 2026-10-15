#ifndef LOG_TASK_H_
#define LOG_TASK_H_

#include "FreeRTOS.h"
#include "task.h"
#include <stdint.h>

extern TaskHandle_t log_task_handle;
void log_task_create(void);
/* 存储故障后置 1，保持到重启；供 HMI 读取，不访问 Flash。 */
uint8_t log_task_storage_failed_get(void);

#endif
