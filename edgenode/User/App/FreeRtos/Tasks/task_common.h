#ifndef TASK_COMMON_H_
#define TASK_COMMON_H_

#include "FreeRTOS.h"
#include "task.h"

/* 初始化失败时本任务不再访问外设或队列，但继续阻塞让其他任务可以运行。 */
static inline void task_block_forever(void)
{
    while(1)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

#endif
